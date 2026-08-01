#include <irq.h>
#include <mutex.h>
#include <task.h>
#include <stdint.h>

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
