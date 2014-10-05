#ifndef __KERNEL_H__
#define __KERNEL_H__

#include <stdint.h>

#define LOGLEVEL_DEFAULT 0
#define LOGLEVEL_DEBUG 1

#define MODULE(X) static const char *__module_name = X;

#define mprintf(X, ...) __mprintf(X, __module_name, __VA_ARGS__);

void halt(void);

void DPRINT(const char *fmt, ...);

/* Print a string to the console and log */
void kprintf(const char *fmt, ...);

/* Displays the error string and halts the system */
void panic(const char *fmt, ...);

void __mprintf(uint8_t type, const char *fmt, ...);

extern uint32_t kernel_base;
extern uint32_t kernel_end;

#endif