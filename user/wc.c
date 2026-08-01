#include "ulib.h"

static int whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v';
}

static int count(int fd, const char *name)
{
    char buf[512];
    int lines = 0;
    int words = 0;
    int chars = 0;
    int inword = 0;
    int n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        chars += n;
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                lines++;
            }
            if (whitespace(buf[i])) {
                inword = 0;
            } else if (!inword) {
                words++;
                inword = 1;
            }
        }
    }
    if (n < 0) {
        put("wc: read error\n");
        return -1;
    }

    puti(lines);
    put(" ");
    puti(words);
    put(" ");
    puti(chars);
    if (name) {
        put(" ");
        put(name);
    }
    put("\n");
    return 0;
}

void _start(int argc, char **argv)
{
    if (argc == 1) {
        exit(count(0, 0) < 0);
    }

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i]);
        if (fd < 0) {
            put("wc: cannot open ");
            put(argv[i]);
            put("\n");
            exit(1);
        }
        if (count(fd, argv[i]) < 0) {
            close(fd);
            exit(1);
        }
        close(fd);
    }
    exit(0);
}
