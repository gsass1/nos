// stdlib for NOS user programs. Implementations in user/libc/stdlib.c.
#ifndef __USER_LIBC_STDLIB_H__
#define __USER_LIBC_STDLIB_H__

#include <stddef.h>

void *malloc(size_t nbytes);
void free(void *p);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *p, size_t nbytes);

int atoi(const char *s);
long strtol(const char *s, char **end, int base);

void abort(void) __attribute__((noreturn));

#endif
