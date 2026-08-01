// cat -- print the contents of initrd files. Exercises the whole new syscall
// surface: argv from exec, open/read/close, sbrk for its buffer, and a real
// exit status.
#include "ulib.h"

#define BUFSZ 4096

void _start(int argc, char **argv)
{
    if (argc < 2) {
        put("usage: cat <file> [...]\n");
        exit(1);
    }

    char *buf = sbrk(BUFSZ);
    if (buf == (char *)-1) {
        put("cat: out of memory\n");
        exit(1);
    }

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i]);
        if (fd < 0) {
            put("cat: ");
            put(argv[i]);
            put(": not found\n");
            exit(1);
        }
        int n;
        while ((n = read(fd, buf, BUFSZ)) > 0) {
            write(1, buf, n);
        }
        close(fd);
    }
    exit(0);
}
