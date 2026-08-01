#ifndef __STDINT_H__
#define __STDINT_H__

// size_t (and NULL) come from the compiler's freestanding <stddef.h>. Its
// include guards are what let this header coexist in one translation unit
// with third-party code (BearSSL) that includes <stddef.h> itself.
#include <stddef.h>

typedef unsigned int   uint32_t;
typedef          int   sint32_t;
typedef          int   int32_t;
typedef unsigned short uint16_t;
typedef          short sint16_t;
typedef          short int16_t;
typedef unsigned char  uint8_t;
typedef          char  sint8_t;
typedef signed char    int8_t;
typedef unsigned long long uint64_t;
typedef          long long int64_t;

#endif
