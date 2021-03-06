/*
 * Copyright (c) 2000-2006 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * @OSF_COPYRIGHT@
 */

/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */

/*
 *  Common code for printf et al.
 *
 *  The calling routine typically takes a variable number of arguments,
 *  and passes the address of the first one.  This implementation
 *  assumes a straightforward, stack implementation, aligned to the
 *  machine's wordsize.  Increasing addresses are assumed to point to
 *  successive arguments (left-to-right), as is the case for a machine
 *  with a downward-growing stack with arguments pushed right-to-left.
 *
 *  To write, for example, fprintf() using this routine, the code
 *
 *	fprintf(fd, format, args)
 *	FILE *fd;
 *	char *format;
 *	{
 *	_doprnt(format, &args, fd);
 *	}
 *
 *  would suffice.  (This example does not handle the fprintf's "return
 *  value" correctly, but who looks at the return value of fprintf
 *  anyway?)
 *
 *  This version implements the following printf features:
 *
 *	%d	decimal conversion
 *	%u	unsigned conversion
 *	%x	hexadecimal conversion
 *	%X	hexadecimal conversion with capital letters
 *      %D      hexdump, ptr & separator string ("%6D", ptr, ":") -> XX:XX:XX:XX:XX:XX
 *              if you use, "%*D" then there's a length, the data ptr and then the separator
 *	%o	octal conversion
 *	%c	character
 *	%s	string
 *	%m.n	field width, precision
 *	%-m.n	left adjustment
 *	%0m.n	zero-padding
 *	%*.*	width and precision taken from arguments
 *
 *  This version does not implement %f, %e, or %g.
 *
 *  As mentioned, this version does not return any reasonable value.
 *
 *  Permission is granted to use, modify, or propagate this code as
 *  long as this notice is incorporated.
 *
 *  Steve Summit 3/25/87
 *
 *  Tweaked for long long support and extended to support the hexdump %D
 *  specifier by dbg 05/02/02.
 */

/*
 * Added formats for decoding device registers:
 *
 * printf("reg = %b", regval, "<base><arg>*")
 *
 * where <base> is the output base expressed as a control character:
 * i.e. '\10' gives octal, '\20' gives hex.  Each <arg> is a sequence of
 * characters, the first of which gives the bit number to be inspected
 * (origin 1), and the rest (up to a control character (<= 32)) give the
 * name of the register.  Thus
 *	printf("reg = %b\n", 3, "\10\2BITTWO\1BITONE")
 * would produce
 *	reg = 3<BITTWO,BITONE>
 *
 * If the second character in <arg> is also a control character, it
 * indicates the last bit of a bit field.  In this case, printf will extract
 * bits <1> to <2> and print it.  Characters following the second control
 * character are printed before the bit field.
 *	printf("reg = %b\n", 0xb, "\10\4\3FIELD1=\2BITTWO\1BITONE")
 * would produce
 *	reg = b<FIELD1=2,BITONE>
 *
 * The %B format is like %b but the bits are numbered from the most
 * significant (the bit weighted 31), which is called 1, to the least
 * significant, called 32.
 */
/*
 * Added for general use:
 *	#	prefix for alternate format:
 *		0x (0X) for hex
 *		leading 0 for octal
 *	+	print '+' if positive
 *	blank	print ' ' if positive
 *
 *	z	signed hexadecimal
 *	r	signed, 'radix'
 *	n	unsigned, 'radix'
 *
 *	D,U,O,Z	same as corresponding lower-case versions
 *		(compatibility)
 */
/*
 * Added support for print long long (64-bit) integers.
 * Use %lld, %Ld or %qd to print a 64-bit int.  Other
 * output bases such as x, X, u, U, o, and O also work.
 */

#include <debug.h>
#include <mach_kdp.h>
#include <platforms.h>
#include <mach/boolean.h>
#include <kern/cpu_number.h>
#include <kern/lock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/misc_protos.h>
#include <stdarg.h>
#include <string.h>
#include <mach_assert.h>
#ifdef  MACH_BSD
#include <sys/msgbuf.h>
#endif
#include <console/serial_protos.h>

#define isdigit(d) ((d) >= '0' && (d) <= '9')
#define Ctod(c) ((c) - '0')

#define MAXBUF (sizeof(long long int) * 8)	/* enough for binary */
static char digs[] = "0123456789abcdef";

#if CONFIG_NO_PRINTF_STRINGS
/* Prevent CPP from breaking the definition below */
#undef printf
#endif

int _consume_printf_args(int a __unused, ...)
{
    return 0;
}
void _consume_kprintf_args(int a __unused, ...)
{
}

static int
printnum(
	unsigned long long int	u,	/* number to print */
	int		base,
	void			(*putc)(int, void *),
	void                    *arg)
{
	char	buf[MAXBUF];	/* build number here */
	char *	p = &buf[MAXBUF-1];
	int nprinted = 0;

	do {
	    *p-- = digs[u % base];
	    u /= base;
	} while (u != 0);

	while (++p != &buf[MAXBUF]) {
	    (*putc)(*p, arg);
	    nprinted++;
	}

	return nprinted;
}

boolean_t	_doprnt_truncates = FALSE;

int
__doprnt(
	const char	*fmt,
	va_list			argp,
						/* character output routine */
	void			(*putc)(int, void *arg),
	void                    *arg,
	int			radix)		/* default radix - for '%r' */
{
	int		length;
	int		prec;
	boolean_t	ladjust;
	char		padc;
	long long		n;
	unsigned long long	u;
	int		plus_sign;
	int		sign_char;
	boolean_t	altfmt, truncate;
	int		base;
	char	c;
	int		capitals;
	int		long_long;
	int             nprinted = 0;

	while ((c = *fmt) != '\0') {
	    if (c != '%') {
		(*putc)(c, arg);
		nprinted++;
		fmt++;
		continue;
	    }

	    fmt++;

	    long_long = 0;
	    length = 0;
	    prec = -1;
	    ladjust = FALSE;
	    padc = ' ';
	    plus_sign = 0;
	    sign_char = 0;
	    altfmt = FALSE;

	    while (TRUE) {
		c = *fmt;
		if (c == '#') {
		    altfmt = TRUE;
		}
		else if (c == '-') {
		    ladjust = TRUE;
		}
		else if (c == '+') {
		    plus_sign = '+';
		}
		else if (c == ' ') {
		    if (plus_sign == 0)
			plus_sign = ' ';
		}
		else
		    break;
		fmt++;
	    }

	    if (c == '0') {
		padc = '0';
		c = *++fmt;
	    }

	    if (isdigit(c)) {
		while(isdigit(c)) {
		    length = 10 * length + Ctod(c);
		    c = *++fmt;
		}
	    }
	    else if (c == '*') {
		length = va_arg(argp, int);
		c = *++fmt;
		if (length < 0) {
		    ladjust = !ladjust;
		    length = -length;
		}
	    }

	    if (c == '.') {
		c = *++fmt;
		if (isdigit(c)) {
		    prec = 0;
		    while(isdigit(c)) {
			prec = 10 * prec + Ctod(c);
			c = *++fmt;
		    }
		}
		else if (c == '*') {
		    prec = va_arg(argp, int);
		    c = *++fmt;
		}
	    }

	    if (c == 'l') {
		c = *++fmt;	/* need it if sizeof(int) < sizeof(long) */
		if (sizeof(int)<sizeof(long))
		    long_long = 1;
		if (c == 'l') {
		    long_long = 1;
		    c = *++fmt;
		}	
	    } else if (c == 'q' || c == 'L') {
	    	long_long = 1;
		c = *++fmt;
	    } 

	    truncate = FALSE;
	    capitals=0;		/* Assume lower case printing */

	    switch(c) {
		case 'b':
		case 'B':
		{
		    register char *p;
		    boolean_t	  any;
		    register int  i;

		    if (long_long) {
			u = va_arg(argp, unsigned long long);
		    } else {
			u = va_arg(argp, unsigned int);
		    }
		    p = va_arg(argp, char *);
		    base = *p++;
		    nprinted += printnum(u, base, putc, arg);

		    if (u == 0)
			break;

		    any = FALSE;
		    while ((i = *p++) != '\0') {
			if (*fmt == 'B')
			    i = 33 - i;
			if (*p <= 32) {
			    /*
			     * Bit field
			     */
			    register int j;
			    if (any)
				(*putc)(',', arg);
			    else {
				(*putc)('<', arg);
				any = TRUE;
			    }
			    nprinted++;
			    j = *p++;
			    if (*fmt == 'B')
				j = 32 - j;
			    for (; (c = *p) > 32; p++) {
				(*putc)(c, arg);
				nprinted++;
			    }
			    nprinted += printnum((unsigned)( (u>>(j-1)) & ((2<<(i-j))-1)),
						 base, putc, arg);
			}
			else if (u & (1<<(i-1))) {
			    if (any)
				(*putc)(',', arg);
			    else {
				(*putc)('<', arg);
				any = TRUE;
			    }
			    nprinted++;
			    for (; (c = *p) > 32; p++) {
				(*putc)(c, arg);
				nprinted++;
			    }
			}
			else {
			    for (; *p > 32; p++)
				continue;
			}
		    }
		    if (any) {
			(*putc)('>', arg);
			nprinted++;
		    }
		    break;
		}

		case 'c':
		    c = va_arg(argp, int);
		    (*putc)(c, arg);
		    nprinted++;
		    break;

		case 's':
		{
		    register const char *p;
		    register const char *p2;

		    if (prec == -1)
			prec = 0x7fffffff;	/* MAXINT */

		    p = va_arg(argp, char *);

		    if (p == NULL)
			p = "";

		    if (length > 0 && !ladjust) {
			n = 0;
			p2 = p;

			for (; *p != '\0' && n < prec; p++)
			    n++;

			p = p2;

			while (n < length) {
			    (*putc)(' ', arg);
			    n++;
			    nprinted++;
			}
		    }

		    n = 0;

		    while ((n < prec) && (!(length > 0 && n >= length))) {
			    if (*p == '\0') {
				    break;
			    }
			    (*putc)(*p++, arg);
			    nprinted++;
			    n++;
		    }

		    if (n < length && ladjust) {
			while (n < length) {
			    (*putc)(' ', arg);
			    n++;
			    nprinted++;
			}
		    }

		    break;
		}

		case 'o':
		    truncate = _doprnt_truncates;
		case 'O':
		    base = 8;
		    goto print_unsigned;

		case 'D': {
		    unsigned char *up;
		    char *q, *p;
		    
			up = (unsigned char *)va_arg(argp, unsigned char *);
			p = (char *)va_arg(argp, char *);
			if (length == -1)
				length = 16;
			while(length--) {
				(*putc)(digs[(*up >> 4)], arg);
				(*putc)(digs[(*up & 0x0f)], arg);
				nprinted += 2;
				up++;
				if (length) {
				    for (q=p;*q;q++) {
						(*putc)(*q, arg);
						nprinted++;
				    }
				}
			}
			break;
		}

		case 'd':
		    truncate = _doprnt_truncates;
		    base = 10;
		    goto print_signed;

		case 'u':
		    truncate = _doprnt_truncates;
		case 'U':
		    base = 10;
		    goto print_unsigned;

		case 'p':
		    altfmt = TRUE;
		    if (sizeof(int)<sizeof(void *)) {
			long_long = 1;
		    }
		case 'x':
		    truncate = _doprnt_truncates;
		    base = 16;
		    goto print_unsigned;

		case 'X':
		    base = 16;
		    capitals=16;	/* Print in upper case */
		    goto print_unsigned;

		case 'z':
		    truncate = _doprnt_truncates;
		    base = 16;
		    goto print_signed;
			
		case 'Z':
		    base = 16;
		    capitals=16;	/* Print in upper case */
		    goto print_signed;

		case 'r':
		    truncate = _doprnt_truncates;
		case 'R':
		    base = radix;
		    goto print_signed;

		case 'n':
		    truncate = _doprnt_truncates;
		case 'N':
		    base = radix;
		    goto print_unsigned;

		print_signed:
		    if (long_long) {
			n = va_arg(argp, long long);
		    } else {
			n = va_arg(argp, int);
		    }
		    if (n >= 0) {
			u = n;
			sign_char = plus_sign;
		    }
		    else {
			u = -n;
			sign_char = '-';
		    }
		    goto print_num;

		print_unsigned:
		    if (long_long) {
			u = va_arg(argp, unsigned long long);
		    } else { 
			u = va_arg(argp, unsigned int);
		    }
		    goto print_num;

		print_num:
		{
		    char	buf[MAXBUF];	/* build number here */
		    register char *	p = &buf[MAXBUF-1];
		    static char digits[] = "0123456789abcdef0123456789ABCDEF";
		    const char *prefix = NULL;

		    if (truncate) u = (long long)((int)(u));

		    if (u != 0 && altfmt) {
			if (base == 8)
			    prefix = "0";
			else if (base == 16)
			    prefix = "0x";
		    }

		    do {
			/* Print in the correct case */
			*p-- = digits[(u % base)+capitals];
			u /= base;
		    } while (u != 0);

		    length -= (int)(&buf[MAXBUF-1] - p);
		    if (sign_char)
			length--;
		    if (prefix)
			length -= (int)strlen(prefix);

		    if (padc == ' ' && !ladjust) {
			/* blank padding goes before prefix */
			while (--length >= 0) {
			    (*putc)(' ', arg);
			    nprinted++;
			}			    
		    }
		    if (sign_char) {
			(*putc)(sign_char, arg);
			nprinted++;
		    }
		    if (prefix) {
			while (*prefix) {
			    (*putc)(*prefix++, arg);
			    nprinted++;
			}
		    }
		    if (padc == '0') {
			/* zero padding goes after sign and prefix */
			while (--length >= 0) {
			    (*putc)('0', arg);
			    nprinted++;
			}			    
		    }
		    while (++p != &buf[MAXBUF]) {
			(*putc)(*p, arg);
			nprinted++;
		    }
		    
		    if (ladjust) {
			while (--length >= 0) {
			    (*putc)(' ', arg);
			    nprinted++;
			}
		    }
		    break;
		}

		case '\0':
		    fmt--;
		    break;

		default:
		    (*putc)(c, arg);
		    nprinted++;
	    }
	fmt++;
	}

	return nprinted;
}

static void
dummy_putc(int ch, void *arg)
{
    void (*real_putc)(char) = arg;
    
    real_putc(ch);
}

int enable_timing = 1;

void 
_doprnt(
	register const char	*fmt,
	va_list			*argp,
						/* character output routine */
	void			(*putc)(char),
	int			radix)		/* default radix - for '%r' */
{
#if 0
	/* timing information */
	if(enable_timing) {
		char tbuf[50], *tp;
		uint64_t t = mach_absolute_time();
		uint64_t ns; absolutetime_to_nanoseconds(t, &ns);
		/* truncate the end of the timing information */
		int tlen = sprintf(tbuf, "[%5llu.%06llu] ", (uint64_t)(ns / NSEC_PER_SEC), (uint64_t)((ns % NSEC_PER_SEC) / 1000));
		for(tp = tbuf; tp < tbuf + tlen; tp++) 
			putc(*tp);
	}
#endif
    __doprnt(fmt, *argp, dummy_putc, putc, radix);
}

#if	MP_PRINTF 
boolean_t	new_printf_cpu_number = FALSE;
#endif	/* MP_PRINTF */


decl_simple_lock_data(,printf_lock)
decl_simple_lock_data(,bsd_log_spinlock)
extern void bsd_log_init(void);
void bsd_log_lock(void);
void bsd_log_unlock(void);

void
printf_init(void)
{
	/*
	 * Lock is only really needed after the first thread is created.
	 */
	simple_lock_init(&printf_lock, 0);
	simple_lock_init(&bsd_log_spinlock, 0);
	bsd_log_init();
}

void
bsd_log_lock(void)
{
	simple_lock(&bsd_log_spinlock);
}

void
bsd_log_unlock(void)
{
	simple_unlock(&bsd_log_spinlock);
}

/* derived from boot_gets */
void
safe_gets(
	char	*str,
	int	maxlen)
{
	register char *lp;
	register int c;
	char *strmax = str + maxlen - 1; /* allow space for trailing 0 */

	lp = str;
	for (;;) {
		c = cngetc();
		switch (c) {
		case '\n':
		case '\r':
			printf("\n");
			*lp++ = 0;
			return;
			
		case '\b':
		case '#':
		case '\177':
			if (lp > str) {
				printf("\b \b");
				lp--;
			}
			continue;

		case '@':
		case 'u'&037:
			lp = str;
			printf("\n\r");
			continue;

		default:
			if (c >= ' ' && c < '\177') {
				if (lp < strmax) {
					*lp++ = c;
					printf("%c", c);
				}
				else {
					printf("%c", '\007'); /* beep */
				}
			}
		}
	}
}

extern int disableConsoleOutput;

void
conslog_putc(
	char c)
{
	if ((debug_mode && !disable_debug_output) || !disableConsoleOutput)
		cnputc(c);

#ifdef	MACH_BSD
	if (debug_mode == 0)
		log_putc(c);
#endif
}

void
cons_putc_locked(
	char c)
{
	if ((debug_mode && !disable_debug_output) || !disableConsoleOutput)
		cnputc(c);
}

int
printf(const char *fmt, ...)
{
	va_list	listp;

	if (fmt) {
		disable_preemption();
		va_start(listp, fmt);
		_doprnt(fmt, &listp, conslog_putc, 16);
		va_end(listp);
		enable_preemption();
	}
	return 0;
}

void
consdebug_putc(char c)
{
	if ((debug_mode && !disable_debug_output) || !disableConsoleOutput)
		cnputc(c);

	debug_putc(c);

	if (!console_is_serial())
		if (!disable_serial_output)
			PE_kputc(c);
}

void
consdebug_putc_unbuffered(char c)
{
	if ((debug_mode && !disable_debug_output) || !disableConsoleOutput)
		cnputc_unbuffered(c);

	debug_putc(c);

	if (!console_is_serial())
		if (!disable_serial_output)
			PE_kputc(c);
}

void
consdebug_log(char c)
{
	debug_putc(c);
}

int
kdb_printf(const char *fmt, ...)
{
	va_list	listp;

	va_start(listp, fmt);
	_doprnt(fmt, &listp, consdebug_putc, 16);
	va_end(listp);
	return 0;
}

int
kdb_log(const char *fmt, ...)
{
	va_list	listp;

	va_start(listp, fmt);
	_doprnt(fmt, &listp, consdebug_log, 16);
	va_end(listp);
	return 0;
}

int
kdb_printf_unbuffered(const char *fmt, ...)
{
	va_list	listp;

	va_start(listp, fmt);
	_doprnt(fmt, &listp, consdebug_putc_unbuffered, 16);
	va_end(listp);
	return 0;
}
 
#ifdef __arm__  /* I don't want to rebuild my symbolsets. */
#undef CONFIG_EMBEDDED
#define CONFIG_EMBEDDED 0
#endif

#if !CONFIG_EMBEDDED

static void
copybyte(int c, void *arg)
{
	/*
	 * arg is a pointer (outside pointer) to the pointer
	 * (inside pointer) which points to the character.
	 * We pass a double pointer, so that we can increment
	 * the inside pointer.
	 */
	char** p = arg;	/* cast outside pointer */
	**p = c;	/* store character */
	(*p)++;		/* increment inside pointer */
}

/*
 * Deprecation Warning:
 *	sprintf() is being deprecated. Please use snprintf() instead.
 */
int
sprintf(char *buf, const char *fmt, ...)
{
        va_list listp;
	char *copybyte_str;

        va_start(listp, fmt);
        copybyte_str = buf;
        __doprnt(fmt, listp, copybyte, &copybyte_str, 16);
        va_end(listp);
	*copybyte_str = '\0';
        return (int)strlen(buf);
}
#endif /* !CONFIG_EMBEDDED */
