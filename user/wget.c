// wget -- HTTP/1.0 GET over the kernel's TCP stack, response to stdout.
//
//   wget <host> [path] [port]
//
// <host> is a name (resolved through SYS_RESOLVE) or a dotted quad. The
// response -- status line, headers and body -- is streamed verbatim, so
// pipes work: `wget example.com / | grep title`.
#include "ulib.h"

#define BUFSZ 2048

// Dotted-quad parser; returns 1 and the wire-order address on success.
static int parse_ip(const char *s, unsigned *out)
{
    unsigned ip = 0;
    for (int part = 0; part < 4; part++) {
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) {
            v = v * 10 + (*s++ - '0');
            digits++;
        }
        if (!digits || v > 255) {
            return 0;
        }
        ip |= (unsigned)v << (part * 8); // wire order: first octet lowest
        if (part < 3 && *s++ != '.') {
            return 0;
        }
    }
    if (*s) {
        return 0;
    }
    *out = ip;
    return 1;
}

static int parse_port(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
        if (v > 65535) {
            return -1;
        }
    }
    return (*s || v == 0) ? -1 : v;
}

void _start(int argc, char **argv)
{
    if (argc < 2) {
        put("usage: wget <host> [path] [port]\n");
        exit(1);
    }
    const char *host = argv[1];
    const char *path = argc > 2 ? argv[2] : "/";
    int port = 80;
    if (argc > 3 && (port = parse_port(argv[3])) < 0) {
        put("wget: bad port\n");
        exit(1);
    }

    unsigned ip;
    if (!parse_ip(host, &ip)) {
        if (resolve(host, &ip) < 0) {
            put("wget: cannot resolve ");
            put(host);
            put("\n");
            exit(1);
        }
    }

    int fd = connect(ip, port);
    if (fd < 0) {
        put("wget: connect failed\n");
        exit(1);
    }

    char req[512];
    int n = 0;
    const char *parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                            "\r\nConnection: close\r\n\r\n" };
    for (int i = 0; i < 5; i++) {
        for (const char *p = parts[i]; *p && n < (int)sizeof(req); p++) {
            req[n++] = *p;
        }
    }
    if (write(fd, req, n) != n) {
        put("wget: send failed\n");
        close(fd);
        exit(1);
    }

    char buf[BUFSZ];
    int got;
    while ((got = read(fd, buf, BUFSZ)) > 0) {
        write(1, buf, got);
    }
    close(fd);
    if (got < 0) {
        put("wget: connection error\n");
        exit(1);
    }
    exit(0);
}
