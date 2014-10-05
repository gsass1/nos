#ifndef __VSPRINTF_H__
#define __VSPRINTF_H__

#include <va_list.h>

int vsprintf(char *buf, const char *fmt, va_list args);

#endif