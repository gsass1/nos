#include <mutex.h>
#include <task.h>
#include <stdint.h>

// Save the current interrupt flag and disable interrupts. Returns the previous
// EFLAGS so irq_restore can put IF back exactly as it was -- important because
// callers such as panic() run with interrupts already disabled and must not
// have them silently re-enabled.
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

void mutex_lock(struct mutex *mutex)
{
    // The test-and-set must be atomic with respect to preemption. On this
    // single-CPU kernel, masking interrupts around it is enough; if the lock is
    // already held we drop the mask and yield so the holder can run.
    for (;;) {
        uint32_t flags = irq_save();
        if (!mutex->locked) {
            mutex->locked = 1;
            irq_restore(flags);
            return;
        }
        irq_restore(flags);
        task_switch();
    }
}

void mutex_unlock(struct mutex *mutex)
{
    uint32_t flags = irq_save();
    mutex->locked = 0;
    irq_restore(flags);
}
