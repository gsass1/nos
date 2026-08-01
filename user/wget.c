// wget -- HTTP/1.0 GET over the kernel's TCP stack, response to stdout.
// HTTPS is handled entirely in userland by BearSSL layered over the plain
// socket fd; the kernel knows nothing about TLS.
//
//   wget [-k] <url> [port]
//
// <url> is `http://host/path`, `https://host[:port]/path`, a bare host, or
// a dotted quad; bare hosts may be followed by a path and port as separate
// arguments (`wget 10.0.2.100 / 80`). Certificates are verified against the
// embedded trust anchors (user/trust_anchors.h) using the RTC's wall-clock
// time; -k keeps the encryption but skips the trust check, for sites whose
// chain doesn't reach an embedded root.
#include "ulib.h"
#include <bearssl.h>
#include "trust_anchors.h"

#define BUFSZ 2048

static br_ssl_client_context sc;
static br_x509_minimal_context xc;
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
static br_sslio_context ioc;

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

// ------------------------------------------------------------------- TLS

static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
    int r = read(*(int *)ctx, buf, len);
    return r <= 0 ? -1 : r; // TCP EOF/error both end the TLS transport
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
    int r = write(*(int *)ctx, buf, len);
    return r <= 0 ? -1 : r;
}

// -k support: an X.509 engine that forwards to the real minimal validator
// but swallows only the "chain does not reach a trust anchor" verdict. The
// certificate is still parsed and its key still used, so the session stays
// encrypted -- it just isn't authenticated.
typedef struct {
    const br_x509_class *vtable;
    const br_x509_class **inner;
} x509_noanchor_context;

static void xwc_start_chain(const br_x509_class **ctx, const char *server_name)
{
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    // Insecure mode also skips the hostname check: pass no expected name.
    // (SNI still goes out on the wire; the engine takes it from reset().)
    (void)server_name;
    (*xwc->inner)->start_chain(xwc->inner, 0);
}

static void xwc_start_cert(const br_x509_class **ctx, uint32_t length)
{
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    (*xwc->inner)->start_cert(xwc->inner, length);
}

static void xwc_append(const br_x509_class **ctx, const unsigned char *buf,
                       size_t len)
{
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    (*xwc->inner)->append(xwc->inner, buf, len);
}

static void xwc_end_cert(const br_x509_class **ctx)
{
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    (*xwc->inner)->end_cert(xwc->inner);
}

static unsigned xwc_end_chain(const br_x509_class **ctx)
{
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    unsigned r = (*xwc->inner)->end_chain(xwc->inner);
    return r == BR_ERR_X509_NOT_TRUSTED ? 0 : r;
}

static const br_x509_pkey *xwc_get_pkey(const br_x509_class *const *ctx,
                                        unsigned *usages)
{
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    return (*xwc->inner)->get_pkey((const br_x509_class *const *)xwc->inner,
                                   usages);
}

static const br_x509_class x509_noanchor_vtable = {
    sizeof(x509_noanchor_context),
    xwc_start_chain,
    xwc_start_cert,
    xwc_append,
    xwc_end_cert,
    xwc_end_chain,
    xwc_get_pkey,
};

static x509_noanchor_context xwc;

// BearSSL's sysrng.c (rdrand//dev/urandom seeders) is excluded from the NOS
// build, but ssl_engine references the symbol; "no system seeder" is the
// answer -- entropy is injected explicitly below.
br_prng_seeder br_prng_seeder_system(const char **name)
{
    if (name) {
        *name = "none";
    }
    return 0;
}

// The engine refuses to handshake without entropy. There is no real entropy
// source on this machine; TSC + wall clock + uptime is unpredictable enough
// for a hobby OS and honest about what it is: NOT cryptographically strong.
static void inject_entropy(void)
{
    unsigned seed[8];
    unsigned lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    seed[0] = lo;
    seed[1] = hi;
    seed[2] = time_unix();
    seed[3] = (unsigned)uptime_ms();
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    seed[4] = lo;
    seed[5] = hi ^ 0x9E3779B9;
    seed[6] = seed[0] * 2654435761u;
    seed[7] = seed[2] ^ (seed[3] << 16);
    br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof(seed));
}

static const char *tls_strerror(int err)
{
    switch (err) {
    case BR_ERR_X509_NOT_TRUSTED: return "certificate not trusted (try -k)";
    case BR_ERR_X509_EXPIRED:     return "certificate expired / not yet valid";
    case BR_ERR_X509_BAD_SERVER_NAME: return "certificate name mismatch";
    case BR_ERR_BAD_SIGNATURE:    return "bad signature";
    case BR_ERR_UNSUPPORTED_VERSION: return "unsupported TLS version";
    default: return "TLS error";
    }
}

static void tls_fail(const char *when)
{
    int err = br_ssl_engine_last_error(&sc.eng);
    put("wget: ");
    put(when);
    put(": ");
    put(tls_strerror(err));
    put(" (code ");
    puti(err);
    put(")\n");
    exit(1);
}

// --------------------------------------------------------------------- main

void _start(int argc, char **argv)
{
    int insecure = 0;
    const char *args[3];
    int nargs = 0;
    for (int i = 1; i < argc && nargs < 3; i++) {
        if (streq(argv[i], "-k")) {
            insecure = 1;
        } else {
            args[nargs++] = argv[i];
        }
    }
    if (nargs < 1) {
        put("usage: wget [-k] <url> [port]\n");
        exit(1);
    }

    // Split scheme://host[:port]/path into pieces (all optional but host).
    const char *url = args[0];
    int tls = 0, port = 0;
    if (url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p') {
        if (url[4] == 's' && url[5] == ':' && url[6] == '/' && url[7] == '/') {
            tls = 1;
            url += 8;
        } else if (url[4] == ':' && url[5] == '/' && url[6] == '/') {
            url += 7;
        }
    }
    static char host[256];
    int hl = 0;
    while (*url && *url != '/' && *url != ':' && hl < (int)sizeof(host) - 1) {
        host[hl++] = *url++;
    }
    host[hl] = '\0';
    if (hl == 0) {
        put("wget: bad url\n");
        exit(1);
    }
    static char portbuf[8];
    if (*url == ':') {
        int pl = 0;
        url++;
        while (*url && *url != '/' && pl < (int)sizeof(portbuf) - 1) {
            portbuf[pl++] = *url++;
        }
        portbuf[pl] = '\0';
        if ((port = parse_port(portbuf)) < 0) {
            put("wget: bad port\n");
            exit(1);
        }
    }
    const char *path = *url ? url : (nargs > 1 ? args[1] : "/");
    if (nargs > 2 && (port = parse_port(args[2])) < 0) {
        put("wget: bad port\n");
        exit(1);
    }
    if (!port) {
        port = tls ? 443 : 80;
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

    if (tls) {
        br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
        // X.509 validity is checked against the RTC. BearSSL counts days
        // from year 0; the Unix epoch is day 719528 of that count.
        unsigned now = time_unix();
        if (now) {
            br_x509_minimal_set_time(&xc, now / 86400 + 719528, now % 86400);
        }
        if (insecure) {
            xwc.vtable = &x509_noanchor_vtable;
            xwc.inner = &xc.vtable;
            br_ssl_engine_set_x509(&sc.eng, &xwc.vtable);
        }
        br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);
        inject_entropy();
        br_ssl_client_reset(&sc, host, 0);
        br_sslio_init(&ioc, &sc.eng, sock_read, &fd, sock_write, &fd);
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

    if (tls) {
        if (br_sslio_write_all(&ioc, req, n) < 0 || br_sslio_flush(&ioc) < 0) {
            tls_fail("handshake/send");
        }
        char buf[BUFSZ];
        int got;
        while ((got = br_sslio_read(&ioc, buf, BUFSZ)) > 0) {
            write(1, buf, got);
        }
        // Ended by close_notify (engine closed, last_error OK) or by raw TCP
        // EOF (BR_ERR_IO): both are how HTTP/1.0 servers end a response.
        int err = br_ssl_engine_last_error(&sc.eng);
        close(fd);
        if (err != BR_ERR_OK && err != BR_ERR_IO) {
            tls_fail("read");
        }
        exit(0);
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
