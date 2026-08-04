// stdio for NOS user programs: formatted output only (there is no FILE).
// Implementations in user/libc/stdio.c; conversions supported are
// %c %s %d %i %u %x %X %p %%, flags '-' and '0', width and .precision
// (both accepting '*'). No floating point, no 64-bit conversions.
#ifndef __USER_LIBC_STDIO_H__
#define __USER_LIBC_STDIO_H__

#include <stddef.h>
#include <stdarg.h>

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

int vprintf(const char *fmt, va_list ap);
int vdprintf(int fd, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

int putchar(int c);
int puts(const char *s);

#endif
