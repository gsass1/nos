#include "ulib.h"
#include <stdlib.h>

#define BUFSZ 1024

static int matchhere(const char *re, const char *text);

static int matchstar(int c, const char *re, const char *text)
{
    do {
        if (matchhere(re, text)) {
            return 1;
        }
    } while (*text && (*text++ == c || c == '.'));
    return 0;
}

static int matchhere(const char *re, const char *text)
{
    if (!re[0]) {
        return 1;
    }
    if (re[1] == '*') {
        return matchstar(re[0], re + 2, text);
    }
    if (re[0] == '$' && !re[1]) {
        return !*text;
    }
    if (*text && (re[0] == '.' || re[0] == *text)) {
        return matchhere(re + 1, text + 1);
    }
    return 0;
}

static int match(const char *re, const char *text)
{
    if (re[0] == '^') {
        return matchhere(re + 1, text);
    }
    do {
        if (matchhere(re, text)) {
            return 1;
        }
    } while (*text++);
    return 0;
}

static int grep(int fd, const char *pattern)
{
    char *buf = malloc(BUFSZ);
    if (!buf) {
        return -1;
    }
    int capacity = BUFSZ;
    int used = 0;
    int n;

    for (;;) {
        if (used == capacity - 1) {
            char *more = realloc(buf, (size_t)capacity + BUFSZ);
            if (!more) {
                free(buf);
                return -1;
            }
            buf = more;
            capacity += BUFSZ;
        }
        n = read(fd, buf + used, capacity - used - 1);
        if (n <= 0) {
            break;
        }
        used += n;
        buf[used] = '\0';

        int start = 0;
        for (int i = 0; i < used; i++) {
            if (buf[i] != '\n') {
                continue;
            }
            buf[i] = '\0';
            if (match(pattern, buf + start)) {
                buf[i] = '\n';
                write(1, buf + start, i - start + 1);
            } else {
                buf[i] = '\n';
            }
            start = i + 1;
        }
        if (start) {
            used -= start;
            for (int i = 0; i < used; i++) {
                buf[i] = buf[start + i];
            }
        }
    }

    if (used) {
        buf[used] = '\0';
        if (match(pattern, buf)) {
            write(1, buf, used);
        }
    }
    free(buf);
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        put("usage: grep pattern [file ...]\n");
        exit(1);
    }
    if (argc == 2) {
        exit(grep(0, argv[1]) < 0);
    }

    for (int i = 2; i < argc; i++) {
        int fd = open(argv[i]);
        if (fd < 0) {
            put("grep: cannot open ");
            put(argv[i]);
            put("\n");
            exit(1);
        }
        if (grep(fd, argv[1]) < 0) {
            put("grep: read or memory error\n");
            close(fd);
            exit(1);
        }
        close(fd);
    }
    exit(0);
}
