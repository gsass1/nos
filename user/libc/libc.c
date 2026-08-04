// string.h implementations for NOS user programs. Besides direct callers,
// GCC itself emits calls to memcpy/memmove/memset for struct copies and
// large initializers, so this object is linked into every program.
#include "string.h"
#include "stdlib.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) {
        if (*x != *y) {
            return *x < *y ? -1 : 1;
        }
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++)) {
    }
    return dst;
}

// Standard semantics: stops at n, pads the remainder with NULs, and does
// NOT terminate if src is n or longer.
char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    for (; n && *src; n--) {
        *d++ = *src++;
    }
    while (n--) {
        *d++ = '\0';
    }
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

// Appends at most n bytes of src and always terminates.
char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    for (; n && *src; n--) {
        *d++ = *src++;
    }
    *d = '\0';
    return dst;
}

int strcmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (*x && *x == *y) {
        x++;
        y++;
    }
    return *x - *y;
}

int strncmp(const char *a, const char *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (; n; n--, x++, y++) {
        if (*x != *y || !*x) {
            return *x - *y;
        }
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) {
            return (char *)s;
        }
        if (!*s) {
            return 0;
        }
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = 0;
    for (;; s++) {
        if (*s == (char)c) {
            last = s;
        }
        if (!*s) {
            return (char *)last;
        }
    }
}

char *strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) {
        return (char *)hay;
    }
    for (; *hay; hay++) {
        if (*hay == *needle && !memcmp(hay, needle, n)) {
            return (char *)hay;
        }
    }
    return 0;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}
