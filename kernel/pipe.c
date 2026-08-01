#include <pipe.h>
#include <kernel.h>
#include <task.h>

MODULE("PIPE");

#define PIPE_SZ 512

struct pipe
{
    uint8_t buf[PIPE_SZ];
    uint32_t head, tail;  // head = next write, tail = next read
    int readers, writers; // live fd references to each end
};

// Same discipline as console.c: the two ends run in different tasks, so mask
// interrupts around ring/refcount updates so an ill-timed preemption can't
// tear them.
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

struct pipe *pipe_create(void)
{
    struct pipe *p = kmalloc(sizeof(struct pipe));
    p->head = p->tail = 0;
    p->readers = 1;
    p->writers = 1;
    return p;
}

void pipe_addref(struct pipe *p, int writable)
{
    uint32_t flags = irq_save();
    if (writable) {
        p->writers++;
    } else {
        p->readers++;
    }
    irq_restore(flags);
}

void pipe_release(struct pipe *p, int writable)
{
    uint32_t flags = irq_save();
    if (writable) {
        p->writers--;
    } else {
        p->readers--;
    }
    int dead = p->readers == 0 && p->writers == 0;
    irq_restore(flags);
    if (dead) {
        kfree(p);
    }
}

int pipe_read(struct pipe *p, char *buf, uint32_t len)
{
    if (len == 0) {
        return 0;
    }
    for (;;) {
        uint32_t flags = irq_save();
        uint32_t n = 0;
        while (n < len && p->tail != p->head) {
            buf[n++] = (char)p->buf[p->tail];
            p->tail = (p->tail + 1) % PIPE_SZ;
        }
        int writers = p->writers;
        irq_restore(flags);
        if (n > 0) {
            return (int)n;
        }
        if (writers == 0) {
            return 0; // EOF: empty and every write end is closed
        }
        task_switch(); // empty but writers remain: wait for data
    }
}

int pipe_write(struct pipe *p, const char *buf, uint32_t len)
{
    uint32_t done = 0;
    while (done < len) {
        uint32_t flags = irq_save();
        if (p->readers == 0) {
            irq_restore(flags);
            return -1; // nothing can ever drain this; don't block forever
        }
        while (done < len) {
            uint32_t next = (p->head + 1) % PIPE_SZ;
            if (next == p->tail) {
                break; // full
            }
            p->buf[p->head] = (uint8_t)buf[done++];
            p->head = next;
        }
        irq_restore(flags);
        if (done < len) {
            task_switch(); // full: wait for a reader to drain some
        }
    }
    return (int)len;
}
