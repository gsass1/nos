// string.h for NOS user programs (also the shim BearSSL builds against).
// Implementations in user/libc/libc.c. size_t comes from the compiler's
// freestanding <stddef.h>, matching the convention in include/stdint.h.
#ifndef __USER_LIBC_STRING_H__
#define __USER_LIBC_STRING_H__

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *hay, const char *needle);
char *strdup(const char *s);

#endif
