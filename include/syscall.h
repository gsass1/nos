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
#define SYS_EXECC   19  // execc(path, argv)           -> console id; program runs attached to it
#define SYS_CREAD   20  // cread(cid, buf, len)        -> bytes drained from the console's output
#define SYS_CWRITE  21  // cwrite(cid, buf, len)       -> bytes fed into the console's input
#define SYS_CSTAT   22  // cstat(cid)                  -> attached pid while alive, else 0
#define SYS_CCLOSE  23  // cclose(cid)                 -> 0; frees the console slot
#define SYS_KILL    24  // kill(pid)                   -> 0, or -1 if no such running task
#define SYS_PIPE    25  // pipe(int fds[2])            -> 0; fds[0] = read end, fds[1] = write end
#define SYS_EXEC2   26  // exec2(path, argv, int fds[3]) -> pid; child fd i = caller's fd fds[i], -1 = inherit
#define SYS_RESOLVE 27  // resolve(name, uint32_t *ip) -> 0, or -1; DNS A lookup, ip in wire order
#define SYS_CONNECT 28  // connect(uint32_t ip, port)  -> socket fd, or -1; read/write/close as usual
#define SYS_TIME    29  // time(void)                  -> unix seconds (UTC), or 0 if the RTC was unreadable

// Window surfaces: offscreen pixel buffers shared between a client and the
// display server (wm), X11-style. The client renders into its buffer; the
// server composites it into a window and forwards input as events. Clients
// get at most one surface; it is torn down automatically when they die.
#define SYS_WCREATE 30  // wcreate(w, h)               -> 32bpp pixel buffer, or -1
#define SYS_WEVENT  31  // wevent(struct wev *out)     -> 0, or -1 if none pending
#define SYS_WSTAT   32  // wstat(slot, struct wsurf_info *out) -> 0, or -1 if unused
#define SYS_WMAP    33  // wmap(slot)                  -> server mapping of the pixels, or -1
#define SYS_WSEND   34  // wsend(slot, const struct wev *ev) -> 0, or -1
#define SYS_WUNMAP  35  // wunmap(slot)                -> 0; server releases its mapping

#define SYS_OPENMODE 36 // openmode(const char *path, int flags) -> fd, or -1
#define SYS_LISTDIR  37  // listdir(const char *path, uint idx, char *name[128]) -> 0, or -1 at end
#define SYS_MKDIR    38  // mkdir(const char *path) -> 0, or -1

// Flags for SYS_OPENMODE (backward-compatible: SYS_OPEN is read-only).
#define O_RDONLY  0x0
#define O_WRONLY  0x1
#define O_RDWR    0x2
#define O_CREATE  0x4
#define O_TRUNC   0x8

// Window-surface limits (part of the ABI: servers poll slots 0..WSURF_SLOTS-1).
#define WSURF_SLOTS 3
#define WSURF_MAX_W 800
#define WSURF_MAX_H 600

// Events carried by SYS_WEVENT/SYS_WSEND.
#define WEV_KEY   1 // a = character
#define WEV_MOUSE 2 // a = x, b = y, c = buttons (window-relative coordinates)
#define WEV_CLOSE 3 // server asks the client to exit

struct wev
{
    int32_t type;
    int32_t a, b, c;
};

// Returned by SYS_WSTAT: a snapshot of one surface slot.
struct wsurf_info
{
    int32_t alive; // owner task still running
    int32_t w, h;
    int32_t owner; // owner task id
    char name[16]; // owner's program name
};

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
