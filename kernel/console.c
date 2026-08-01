#include <console.h>
#include <kernel.h>

MODULE("CONS");

#define OUT_SZ 4096
#define IN_SZ  256

struct console
{
    int used;
    int pid;
    uint8_t out[OUT_SZ];
    uint32_t ohead, otail; // head = next write, tail = next read
    uint8_t in[IN_SZ];
    uint32_t ihead, itail;
};

static struct console consoles[NCONSOLE];

// Producers and consumers run in different tasks; mask interrupts around ring
// updates so an ill-timed preemption can't tear an index pair.
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

int console_valid(int cid)
{
    return cid >= 0 && cid < NCONSOLE && consoles[cid].used;
}

int console_alloc(void)
{
    for (int i = 0; i < NCONSOLE; i++) {
        if (!consoles[i].used) {
            consoles[i].used = 1;
            consoles[i].pid = -1;
            consoles[i].ohead = consoles[i].otail = 0;
            consoles[i].ihead = consoles[i].itail = 0;
            return i;
        }
    }
    return -1;
}

void console_free(int cid)
{
    if (console_valid(cid)) {
        consoles[cid].used = 0;
    }
}

void console_set_pid(int cid, int pid)
{
    if (console_valid(cid)) {
        consoles[cid].pid = pid;
    }
}

int console_pid(int cid)
{
    return console_valid(cid) ? consoles[cid].pid : -1;
}

int console_out_write(int cid, const char *buf, uint32_t len)
{
    if (!console_valid(cid)) {
        return -1;
    }
    struct console *c = &consoles[cid];
    uint32_t flags = irq_save();
    uint32_t n = 0;
    while (n < len) {
        uint32_t next = (c->ohead + 1) % OUT_SZ;
        if (next == c->otail) {
            break; // full: drop the rest rather than block
        }
        c->out[c->ohead] = (uint8_t)buf[n++];
        c->ohead = next;
    }
    irq_restore(flags);
    return (int)n;
}

int console_out_read(int cid, char *buf, uint32_t len)
{
    if (!console_valid(cid)) {
        return -1;
    }
    struct console *c = &consoles[cid];
    uint32_t flags = irq_save();
    uint32_t n = 0;
    while (n < len && c->otail != c->ohead) {
        buf[n++] = (char)c->out[c->otail];
        c->otail = (c->otail + 1) % OUT_SZ;
    }
    irq_restore(flags);
    return (int)n;
}

int console_in_write(int cid, const char *buf, uint32_t len)
{
    if (!console_valid(cid)) {
        return -1;
    }
    struct console *c = &consoles[cid];
    uint32_t flags = irq_save();
    uint32_t n = 0;
    while (n < len) {
        uint32_t next = (c->ihead + 1) % IN_SZ;
        if (next == c->itail) {
            break;
        }
        c->in[c->ihead] = (uint8_t)buf[n++];
        c->ihead = next;
    }
    irq_restore(flags);
    return (int)n;
}

int console_in_getc(int cid)
{
    if (!console_valid(cid)) {
        return -1;
    }
    struct console *c = &consoles[cid];
    uint32_t flags = irq_save();
    int r = -1;
    if (c->itail != c->ihead) {
        r = c->in[c->itail];
        c->itail = (c->itail + 1) % IN_SZ;
    }
    irq_restore(flags);
    return r;
}
