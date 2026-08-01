// A freestanding user program. It knows nothing about the kernel except the
// int 0x80 syscall ABI, so it is loaded and run entirely through elf_exec().
#include <syscall.h>

static inline int syscall3(int n, int a, int b, int c)
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

void _start(void)
{
    const char *msg = "Hello from a loaded ELF program (talking only via syscalls)!\n";
    syscall3(SYS_WRITE, 1, (int)msg, slen(msg));
    syscall3(SYS_EXIT, 0, 0, 0);

    // exit() should never return; spin defensively just in case.
    for (;;) {
    }
}
