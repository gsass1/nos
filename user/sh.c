// nsh -- the NOS shell, as a standalone freestanding binary. It is loaded from
// the initrd by elf_exec() and reaches the kernel only through int 0x80, so it
// is a genuine program rather than kernel code (and becomes a ring-3 process
// unchanged once user mode lands).
#include <syscall.h>

static inline int sys0(int n)
{
    int r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n) : "memory");
    return r;
}

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

static void put(const char *s)     { sys3(SYS_WRITE, 1, (int)s, slen(s)); }
static void putc(char c)           { sys3(SYS_WRITE, 1, (int)&c, 1); }
static int  getc(void)             { return sys0(SYS_GETC); }

static int streq(const char *a, const char *b)
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

static void cmd_ls(void)
{
    char name[128];
    int i = 0;
    while (sys3(SYS_READDIR, i, (int)name, sizeof(name)) == 0) {
        put(name);
        put("\n");
        i++;
    }
}

static void run(const char *cmd)
{
    if (cmd[0] == '\0') {
        return;
    } else if (streq(cmd, "help")) {
        put("builtins: help  ls  clear  exit\n");
    } else if (streq(cmd, "ls")) {
        cmd_ls();
    } else if (streq(cmd, "clear")) {
        sys0(SYS_CLEAR);
    } else if (streq(cmd, "exit")) {
        put("bye!\n");
        sys3(SYS_EXIT, 0, 0, 0);
    } else {
        // Not a builtin: try to run it as a program from the initrd, then wait
        // for it to finish before showing the prompt again.
        int pid = sys3(SYS_EXEC, (int)cmd, 0, 0);
        if (pid < 0) {
            put("unknown command: ");
            put(cmd);
            put("\n");
        } else {
            sys3(SYS_WAIT, pid, 0, 0);
        }
    }
}

void _start(void)
{
    char buf[128];

    put("nsh - NOS userspace shell (its own binary). Type 'help'.\n");

    for (;;) {
        put("\nnsh$ ");

        int i = 0;
        for (;;) {
            int c = getc();
            if (c == '\n') {
                putc('\n');
                break;
            }
            if (c == '\b') {
                if (i > 0) {
                    i--;
                    put("\b \b"); // erase the last character on screen
                }
                continue;
            }
            if (i < (int)sizeof(buf) - 1) {
                buf[i++] = (char)c;
                putc((char)c); // local echo
            }
        }
        buf[i] = '\0';
        run(buf);
    }
}
