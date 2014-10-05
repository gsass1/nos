#ifndef __DEBUG_H__
#define __DEBUG_H__

#define BOCHSBREAK() asm volatile("xchg %bx, %bx");

#endif