// Tiny freestanding "libc" for NOS user programs: int 0x80 syscall wrappers
// (ABI in include/syscall.h) plus the few helpers every program wants.
// Header-only; everything is static inline so unused helpers cost nothing.
#ifndef __ULIB_H__
#define __ULIB_H__

#include <syscall.h>

static inline int sys0(int n)
{
    int r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n) : "memory");
    return r;
}

static inline int sys1(int n, int a)
{
    int r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}

static inline int sys3(int n, int a, int b, int c)
{
    int r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static inline void exit(int code)
{
    sys1(SYS_EXIT, code);
    for (;;) { // the kernel never returns here
    }
}

static inline int write(int fd, const void *buf, int len) { return sys3(SYS_WRITE, fd, (int)buf, len); }
static inline int read(int fd, void *buf, int len)        { return sys3(SYS_READ, fd, (int)buf, len); }
static inline int open(const char *path)                  { return sys1(SYS_OPEN, (int)path); }
static inline int openmode(const char *path, int flags)   { return sys3(SYS_OPENMODE, (int)path, flags, 0); }
static inline int listdir(const char *path, int idx, char *name) { return sys3(SYS_LISTDIR, (int)path, idx, (int)name); }
static inline int close(int fd)                           { return sys1(SYS_CLOSE, fd); }
static inline int getc(void)                              { return sys0(SYS_GETC); }
static inline int wait(int pid)                           { return sys1(SYS_WAIT, pid); }
static inline int exec(const char *path, char *const argv[])
{
    return sys3(SYS_EXEC, (int)path, (int)argv, 0);
}

// fds[0] reads what fds[1] writes; both are fresh fds of the caller.
static inline int pipe(int fds[2]) { return sys1(SYS_PIPE, (int)fds); }

// exec with explicit stdio: the child's fd i becomes a copy of the caller's
// fd fds[i], or is inherited as in exec() where fds[i] is -1.
static inline int exec2(const char *path, char *const argv[], const int fds[3])
{
    return sys3(SYS_EXEC2, (int)path, (int)argv, (int)fds);
}

// Grow the heap by incr bytes; returns the start of the new memory,
// or (void *)-1 on failure.
static inline void *sbrk(int incr)
{
    int r = sys1(SYS_SBRK, incr);
    return (void *)r;
}

static inline int fbinfo(struct fb_info *out) { return sys1(SYS_FBINFO, (int)out); }
static inline int fboff(void)                 { return sys0(SYS_FBOFF); }
static inline int sleep(int ms)               { return sys1(SYS_SLEEP, ms); }
static inline int mouse(struct mouse_state *m){ return sys1(SYS_MOUSE, (int)m); }
static inline int pollc(void)                 { return sys0(SYS_POLLC); }
static inline int getfont(void *buf8192)      { return sys1(SYS_FONT, (int)buf8192); }
static inline int uptime_ms(void)             { return sys0(SYS_UPTIME); }

// Console channels: run a program in a captured terminal (see SYS_EXECC).
static inline int execc(const char *path, char *const argv[])
{
    return sys3(SYS_EXECC, (int)path, (int)argv, 0);
}
static inline int cread(int cid, void *buf, int len)  { return sys3(SYS_CREAD, cid, (int)buf, len); }
static inline int cwrite(int cid, const void *buf, int len) { return sys3(SYS_CWRITE, cid, (int)buf, len); }
static inline int cstat(int cid)                      { return sys1(SYS_CSTAT, cid); }
static inline int cclose(int cid)                     { return sys1(SYS_CCLOSE, cid); }
static inline int kill(int pid)                       { return sys1(SYS_KILL, pid); }

// Enters graphics mode; returns the mapped 32bpp framebuffer, or (void *)-1.
static inline void *fbmap(void)
{
    int r = sys0(SYS_FBMAP);
    return (void *)r;
}

static inline int slen(const char *s)
{
    int n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static inline int streq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == b[i];
}

static inline void put(const char *s)  { write(1, s, slen(s)); }
static inline void putch(char c)       { write(1, &c, 1); }

static inline void puti(int v)
{
    char tmp[12];
    int i = 0;
    unsigned u = v < 0 ? -(unsigned)v : (unsigned)v;
    if (v < 0) {
        putch('-');
    }
    do {
        tmp[i++] = '0' + u % 10;
        u /= 10;
    } while (u);
    while (i) {
        putch(tmp[--i]);
    }
}

#endif
