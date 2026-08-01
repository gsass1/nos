// cat -- copy stdin or initrd files to stdout.
#include "ulib.h"

#define BUFSZ 4096

static int copy(int fd, char *buf)
{
    int n;
    while ((n = read(fd, buf, BUFSZ)) > 0) {
        if (write(1, buf, n) != n) {
            put("cat: write error\n");
            return -1;
        }
    }
    return n;
}

void _start(int argc, char **argv)
{
    char *buf = sbrk(BUFSZ);
    if (buf == (char *)-1) {
        put("cat: out of memory\n");
        exit(1);
    }

    if (argc < 2) {
        exit(copy(0, buf) < 0);
    }

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i]);
        if (fd < 0) {
            put("cat: ");
            put(argv[i]);
            put(": not found\n");
            exit(1);
        }
        if (copy(fd, buf) < 0) {
            close(fd);
            exit(1);
        }
        close(fd);
    }
    exit(0);
}
