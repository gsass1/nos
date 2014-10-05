#ifndef __STRING_H__
#define __STRING_H__

#include <stdint.h>

#define LONG_MIN ((long) 0x80000000L)
#define LONG_MAX 0x7FFFFFFFL
#define ULONG_MAX 0xFFFFFFFFUL

char *strcpy(char *dst, const char *src);
size_t strlen(const char *str);
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *ptr, int c, size_t n);
long strtol(const char *ptr, char **endptr, int base);
char *strncpy(char *s1, const char *s2, size_t n);
int strcmp(const char *s1, const char *s2);

#endif