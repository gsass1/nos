#ifndef __IRQ_H__
#define __IRQ_H__

#include <stdint.h>

// Save the current interrupt flag and disable interrupts. Returns the previous
// EFLAGS so irq_restore can put IF back exactly as it was -- important because
// callers such as panic() run with interrupts already disabled and must not
// have them silently re-enabled. Single CPU: masking interrupts is the
// kernel's only mutual-exclusion primitive besides mutex.c (which builds on
// this).
static inline uint32_t irq_save(void)
{
    uint32_t flags;
    asm volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    asm volatile("push %0; popf" :: "r"(flags) : "memory", "cc");
}

#endif
