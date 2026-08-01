// badptr -- feeds the kernel hostile pointers; every call must come back -1
// instead of page-faulting in ring 0 (a panic) or, worse, letting ring 3
// read or write kernel memory through a syscall.
#include "ulib.h"

static int failures;

static void check(const char *what, int got)
{
    if (got != -1) {
        put("badptr: FAIL ");
        put(what);
        put("\n");
        failures = 1;
    }
}

void _start(void)
{
    char *kaddr = (char *)0x00100000;    // kernel identity map
    char *unmapped = (char *)0x7FF00000; // inside the user window, never mapped
    char *wild = (char *)0xDEADBEEF;     // outside the user window entirely

    check("write from kernel memory", write(1, kaddr, 16));
    check("write from unmapped", write(1, unmapped, 16));
    check("write from wild", write(1, wild, 16));
    char c = 'x';
    check("write with huge len", write(1, &c, 0x7FFFFFFF));

    int fd = open("symtable");
    check("read into kernel memory", read(fd, kaddr, 64));
    check("read into unmapped", read(fd, unmapped, 64));
    close(fd);

    check("open with kernel path", open(kaddr));
    check("open with unmapped path", open(unmapped));
    check("pipe into kernel memory", sys1(SYS_PIPE, (int)kaddr));
    check("readdir into unmapped", sys3(SYS_READDIR, 0, (int)unmapped, 64));
    check("exec with unmapped path", exec(unmapped, 0));
    check("mouse into kernel memory", mouse((struct mouse_state *)kaddr));
    check("fbinfo into wild", fbinfo((struct fb_info *)wild));
    check("font into kernel memory", getfont(kaddr));

    // A bogus argv array must not be dereferenced either: exec treats it as
    // empty and the child still runs.
    int pid = exec("hello", (char *const *)unmapped);
    if (pid < 0) {
        put("badptr: FAIL exec with unmapped argv\n");
        failures = 1;
    } else {
        wait(pid);
    }

    if (failures) {
        exit(1);
    }
    put("badptr: all hostile pointers rejected\n");
    exit(0);
}
