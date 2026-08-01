#ifndef __SYSCALL_H__
#define __SYSCALL_H__

#include <stdint.h>

// System call numbers. The user-facing ABI is Linux/i386-like:
//   eax = syscall number, ebx/ecx/edx = args, return value in eax.
#define SYS_EXIT    0   // exit(int code)             -> never returns
#define SYS_WRITE   1   // write(int fd, buf, len)    -> bytes written
#define SYS_GETC    2   // getc(void)                 -> next key (blocking)
#define SYS_READDIR 3   // readdir(uint idx, char *name) -> 0, or -1 at end
#define SYS_CLEAR   4   // clear(void)                 -> clears the screen
#define SYS_EXEC    5   // exec(const char *path)      -> child pid, or -1
#define SYS_WAIT    6   // wait(int pid)               -> 0 once that task exits

// Saved register frame as laid out by SAVE_REGS in interrupt.S, from the
// lowest saved address upward. The dispatcher reads args from here and writes
// the return value back into `eax` so RESTORE_REGS/popad hands it to the caller.
// (On a ring3 -> ring0 syscall the CPU also pushes user esp/ss above eflags;
// the dispatcher never touches those, so this struct stays valid for both.)
struct regs
{
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags;
};

void syscall_init(void);

// Called from _asm_syscall with a pointer to the saved register frame.
void syscall_dispatch(struct regs *r);

#endif
