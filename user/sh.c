// nsh -- the NOS shell, as a standalone ring-3 binary. It is loaded from the
// initrd by elf_exec() and reaches the kernel only through int 0x80.
#include "ulib.h"

#define MAX_ARGS 8

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

static void run(char *line)
{
    // Split the line into words in place; argv is NULL-terminated for exec.
    char *argv[MAX_ARGS + 1];
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ') {
            *p++ = '\0';
        }
        if (!*p) {
            break;
        }
        argv[argc++] = p;
        while (*p && *p != ' ') {
            p++;
        }
    }
    argv[argc] = 0;
    if (argc == 0) {
        return;
    }

    if (streq(argv[0], "help")) {
        put("builtins: help  ls  clear  exit\n");
        put("anything else runs a program from the initrd (try: cat symtable)\n");
    } else if (streq(argv[0], "ls")) {
        cmd_ls();
    } else if (streq(argv[0], "clear")) {
        sys0(SYS_CLEAR);
    } else if (streq(argv[0], "exit")) {
        put("bye!\n");
        exit(0);
    } else {
        // Not a builtin: run it as a program from the initrd (passing the
        // remaining words as its argv) and wait for it to finish.
        int pid = exec(argv[0], argv);
        if (pid < 0) {
            put("unknown command: ");
            put(argv[0]);
            put("\n");
        } else {
            int status = wait(pid);
            if (status != 0) {
                put("[exit status ");
                puti(status);
                put("]\n");
            }
        }
    }
}

void _start(void)
{
    char buf[128];

    put("nsh - NOS userspace shell (ring 3). Type 'help'.\n");

    for (;;) {
        put("\nnsh$ ");

        int i = 0;
        for (;;) {
            int c = getc();
            if (c == '\n') {
                putch('\n');
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
                putch((char)c); // local echo
            }
        }
        buf[i] = '\0';
        run(buf);
    }
}
