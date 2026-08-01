// Deliberately faults from ring 3. Exists to prove fault isolation: the
// kernel must kill this task (page fault: NULL is a supervisor-only page)
// and the shell must keep running afterwards.
#include <syscall.h>

static inline int sys3(int n, int a, int b, int c)
{
    int r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static int slen(const char *s)
{
    int n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void put(const char *s) { sys3(SYS_WRITE, 1, (int)s, slen(s)); }

void _start(void)
{
    put("crash: dereferencing NULL from ring 3...\n");

    *(volatile int *)0 = 42;

    // If we get here, ring 3 memory protection is broken.
    put("crash: STILL ALIVE - kernel memory is user-writable!\n");
    sys3(SYS_EXIT, 1, 0, 0);
    for (;;) {
    }
}
