#ifndef __SYSCALL_H__
#define __SYSCALL_H__

#include <stdint.h>

// System call numbers. The user-facing ABI is Linux/i386-like:
//   eax = syscall number, ebx/ecx/edx = args, return value in eax.
#define SYS_EXIT    0   // exit(int code)             -> never returns
#define SYS_WRITE   1   // write(int fd, buf, len)    -> bytes written
#define SYS_GETC    2   // getc(void)                 -> next key (blocking)
#define SYS_READDIR 3   // readdir(uint idx, char *name, uint len) -> 0, or -1 at end
#define SYS_CLEAR   4   // clear(void)                 -> clears the screen
#define SYS_EXEC    5   // exec(path, char *const argv[]) -> child pid, or -1
#define SYS_WAIT    6   // wait(int pid)               -> exit status once that task dies
#define SYS_OPEN    7   // open(const char *path)      -> fd (>= 3), or -1
#define SYS_READ    8   // read(int fd, buf, len)      -> bytes read, 0 at EOF, -1 on error
#define SYS_CLOSE   9   // close(int fd)               -> 0, or -1
#define SYS_SBRK    10  // sbrk(int incr)              -> previous break, or -1
#define SYS_FBINFO  11  // fbinfo(struct fb_info *out) -> 0, or -1 if no display
#define SYS_FBMAP   12  // fbmap(void)                 -> fb pointer (enables graphics mode), or -1
#define SYS_FBOFF   13  // fboff(void)                 -> 0; back to text mode
#define SYS_SLEEP   14  // sleep(uint ms)              -> 0 after at least ms elapsed
#define SYS_MOUSE   15  // mouse(struct mouse_state *out) -> 0, or -1
#define SYS_POLLC   16  // pollc(void)                 -> key, or -1 if none pending
#define SYS_FONT    17  // font(uint8_t buf[8192])     -> 0; the BIOS 8x16 font, 32 bytes/glyph
#define SYS_UPTIME  18  // uptime(void)                -> milliseconds since boot

// Returned by SYS_MOUSE: cursor position in framebuffer coordinates,
// buttons bit0=left bit1=right bit2=middle.
struct mouse_state
{
    int32_t x;
    int32_t y;
    uint32_t buttons;
};

// Returned by SYS_FBINFO. pitch is in bytes; pixels are 32-bit 0x00RRGGBB.
struct fb_info
{
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
};

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
