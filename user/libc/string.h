// Minimal libc surface for third-party code built into NOS user programs
// (BearSSL needs exactly these five). Implementations in user/libc/libc.c.
// size_t comes from the compiler's freestanding <stddef.h>, matching the
// convention in include/stdint.h.
#ifndef __USER_LIBC_STRING_H__
#define __USER_LIBC_STRING_H__

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
