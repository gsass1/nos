// browser -- a small graphical HTML browser for NOS. Pages are fetched over
// HTTP/1.0 (HTTPS via BearSSL layered on the socket fd, exactly like wget),
// run through a streaming HTML tokenizer -- no DOM -- and laid out as styled,
// word-wrapped text spans on the 1024x768 framebuffer. Links are numbered
// lynx-style and clickable. No CSS, no JavaScript.
//
//   browser [url]
//
// Keys: g enter a url, j/k/space/u scroll, digits+enter follow link [n]
// (or click it), b back, r reload, q quit. Every page load is traced on
// serial ("browser: loaded <url> status <s> len <n>") so the integration
// tests can drive the UI blind via sendkey.
#include "ulib.h"
#include "gfx.h"
#include "libc/string.h"
#include <bearssl.h>
#include "trust_anchors.h"

#define URL_CAP   512
#define PAGE_CAP  (256 * 1024)
#define TEXT_CAP  (256 * 1024)
#define MAX_SPANS 16384
#define MAX_LINKS 256
#define MAX_HIST  16
#define MAX_HOPS  5

#define MARGIN   8
#define TOPBAR_H 24
#define STATUS_H 20
#define LINE_H   16
#define CHAR_W   8

#define COL_PAPER   0x00FFFFFF
#define COL_CHROME  0x00C0C0C0
#define COL_TEXT    0x00101010
#define COL_LINK    0x000000EE
#define COL_LINKNUM 0x00B04000
#define COL_HEAD    0x00000080
#define COL_RULE    0x00808080

#define F_UL   1
#define F_BOLD 2
#define F_RULE 4

struct span
{
    int x, line;       // pixel x, layout line number
    int off;           // into the text arena
    short len;
    short link;        // index into links[], or -1
    unsigned color;
    unsigned char flags;
};

struct linkent
{
    char href[URL_CAP]; // as written in the page; resolved when followed
};

struct url
{
    int tls, port;
    char host[256];
    char path[URL_CAP];
};

// Big buffers come from sbrk; only the TLS engine's working set is static.
static struct gfx bb;             // backbuffer
static volatile unsigned int *fb; // framebuffer, or the window surface pixels
static struct fb_info info;
static int windowed;              // running as a wm surface window
static unsigned out_pitch;        // bytes per output row

// Surface size when running under wm (must fit WSURF_MAX_W/H).
#define WIN_W 780
#define WIN_H 540
static char *page;                // fetched response (headers, then body)
static char *text;                // decoded layout text arena
static struct span *spans;
static struct linkent *links;

static int textlen, nspans, nlinks, nlines;
static int content_w, vis_lines, scroll_line;

static char cur_url[URL_CAP];
static char title[128];
static char statusmsg[128];
static char hist[MAX_HIST][URL_CAP];
static int histn;

static char g_err[96];
static int g_truncated;

#define MODE_PAGE 0
#define MODE_URL  1
static int mode;
static char urlbuf[URL_CAP];
static int urllen;
static char numbuf[8];
static int numlen;
static int dirty = 1;

static void navigate(const char *absurl, int push);

// ------------------------------------------------------------ small strings

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static void scopy(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void sadd(char *dst, int *pos, int cap, const char *src)
{
    while (*src && *pos < cap - 1) {
        dst[(*pos)++] = *src++;
    }
    dst[*pos] = '\0';
}

static void saddi(char *dst, int *pos, int cap, int v)
{
    char tmp[12];
    int i = 0;
    unsigned u = v < 0 ? -(unsigned)v : (unsigned)v;
    if (v < 0) {
        sadd(dst, pos, cap, "-");
    }
    do {
        tmp[i++] = '0' + u % 10;
        u /= 10;
    } while (u);
    while (i) {
        char c[2] = { tmp[--i], 0 };
        sadd(dst, pos, cap, c);
    }
}

// Case-insensitive "does s start with prefix"; returns length matched or 0.
static int ci_prefix(const char *s, const char *prefix)
{
    int i = 0;
    while (prefix[i]) {
        if (lower(s[i]) != lower(prefix[i])) {
            return 0;
        }
        i++;
    }
    return i;
}

static int ci_contains(const char *s, int n, const char *needle)
{
    for (int i = 0; i < n; i++) {
        int j = 0;
        while (needle[j] && i + j < n && lower(s[i + j]) == lower(needle[j])) {
            j++;
        }
        if (!needle[j]) {
            return 1;
        }
    }
    return 0;
}

// ------------------------------------------------------------------- URLs

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

// http(s) URLs only; scheme-less input defaults to http. Fragments are
// dropped -- there is no in-page anchor scrolling.
static int parse_url(const char *s, struct url *u)
{
    u->tls = 0;
    u->port = 0;
    if (ci_prefix(s, "https://")) {
        u->tls = 1;
        s += 8;
    } else if (ci_prefix(s, "http://")) {
        s += 7;
    } else {
        for (const char *p = s; *p && *p != '/'; p++) {
            if (p[0] == ':' && p[1] == '/' && p[2] == '/') {
                return -1; // some other scheme
            }
        }
    }
    int hl = 0;
    while (*s && *s != '/' && *s != ':' && *s != '#' && hl < (int)sizeof(u->host) - 1) {
        u->host[hl++] = *s++;
    }
    u->host[hl] = '\0';
    if (!hl) {
        return -1;
    }
    if (*s == ':') {
        s++;
        int v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s++ - '0');
            if (v > 65535) {
                return -1;
            }
        }
        u->port = v;
    }
    int pl = 0;
    if (*s != '/') {
        u->path[pl++] = '/';
    }
    while (*s && *s != '#' && pl < (int)sizeof(u->path) - 1) {
        u->path[pl++] = *s++;
    }
    u->path[pl] = '\0';
    if (!u->port) {
        u->port = u->tls ? 443 : 80;
    }
    return 0;
}

// Collapse "/./" and "seg/../" segments in place (up to the query string).
static void normalize_path(char *p)
{
    char out[URL_CAP];
    int o = 0, i = 0;
    while (p[i] && p[i] != '?') {
        if (p[i] == '/' && p[i + 1] == '.' && (p[i + 2] == '/' || p[i + 2] == '\0' || p[i + 2] == '?')) {
            i += 2; // "/." -> ""
            continue;
        }
        if (p[i] == '/' && p[i + 1] == '.' && p[i + 2] == '.' &&
            (p[i + 3] == '/' || p[i + 3] == '\0' || p[i + 3] == '?')) {
            while (o > 0 && out[o - 1] != '/') {
                o--;
            }
            if (o > 0) {
                o--; // drop the slash of the popped segment too
            }
            i += 3;
            continue;
        }
        if (o < (int)sizeof(out) - 1) {
            out[o++] = p[i];
        }
        i++;
    }
    if (o == 0) {
        out[o++] = '/';
    }
    // reattach the query string untouched
    while (p[i] && o < (int)sizeof(out) - 1) {
        out[o++] = p[i++];
    }
    out[o] = '\0';
    scopy(p, out, URL_CAP);
}

// Make href absolute against base. Returns 0, or -1 for links we cannot
// follow (fragments, mailto:, javascript:, ...).
static int resolve_url(const char *base, const char *href, char *out, int cap)
{
    if (!href[0] || href[0] == '#') {
        return -1;
    }
    if (ci_prefix(href, "http://") || ci_prefix(href, "https://")) {
        scopy(out, href, cap);
        return 0;
    }
    // A scheme other than http(s) -- "mailto:x", "javascript:void(0)".
    for (const char *p = href; *p && *p != '/' && *p != '?' && *p != '#'; p++) {
        if (*p == ':') {
            return -1;
        }
    }
    struct url b;
    if (parse_url(base, &b) < 0) {
        return -1;
    }
    int o = 0;
    sadd(out, &o, cap, b.tls ? "https:" : "http:");
    if (href[0] == '/' && href[1] == '/') {
        sadd(out, &o, cap, href); // scheme-relative
        return 0;
    }
    sadd(out, &o, cap, "//");
    sadd(out, &o, cap, b.host);
    if (b.port != (b.tls ? 443 : 80)) {
        sadd(out, &o, cap, ":");
        saddi(out, &o, cap, b.port);
    }
    char path[URL_CAP];
    if (href[0] == '/') {
        scopy(path, href, sizeof(path));
    } else if (href[0] == '?') {
        // same path, new query
        int pl = 0;
        for (int i = 0; b.path[i] && b.path[i] != '?'; i++) {
            if (pl < (int)sizeof(path) - 1) {
                path[pl++] = b.path[i];
            }
        }
        path[pl] = '\0';
        scopy(path + pl, href, sizeof(path) - pl);
    } else {
        // relative: replace everything after the base path's last '/'
        int cut = 0;
        for (int i = 0; b.path[i] && b.path[i] != '?'; i++) {
            if (b.path[i] == '/') {
                cut = i + 1;
            }
        }
        int pl = 0;
        for (int i = 0; i < cut && pl < (int)sizeof(path) - 1; i++) {
            path[pl++] = b.path[i];
        }
        path[pl] = '\0';
        scopy(path + pl, href, sizeof(path) - pl);
    }
    normalize_path(path);
    sadd(out, &o, cap, path);
    return 0;
}

// Bare input from argv or the URL prompt: default http:// for dotted quads
// (the QEMU test addresses), https:// for everything else.
static void make_absolute(const char *in, char *out, int cap)
{
    for (const char *p = in; *p && *p != '/'; p++) {
        if (p[0] == ':' && p[1] == '/' && p[2] == '/') {
            scopy(out, in, cap);
            return;
        }
    }
    char host[256];
    int hl = 0;
    while (in[hl] && in[hl] != '/' && in[hl] != ':' && hl < (int)sizeof(host) - 1) {
        host[hl] = in[hl];
        hl++;
    }
    host[hl] = '\0';
    unsigned ip;
    int o = 0;
    sadd(out, &o, cap, parse_ip(host, &ip) ? "http://" : "https://");
    sadd(out, &o, cap, in);
}

// --------------------------------------------------------------------- TLS
// Same setup as wget (see user/wget.c): BearSSL over the socket fd, chains
// verified against the embedded anchors using RTC time. No -k equivalent.

static br_ssl_client_context sc;
static br_x509_minimal_context xc;
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
static br_sslio_context ioc;

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

// BearSSL's sysrng.c is excluded from the NOS build; entropy is injected
// explicitly below (rdtsc/clock mixing -- not cryptographically strong).
br_prng_seeder br_prng_seeder_system(const char **name)
{
    if (name) {
        *name = "none";
    }
    return 0;
}

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
    case BR_ERR_X509_NOT_TRUSTED: return "certificate not trusted";
    case BR_ERR_X509_EXPIRED:     return "certificate expired / not yet valid";
    case BR_ERR_X509_BAD_SERVER_NAME: return "certificate name mismatch";
    case BR_ERR_BAD_SIGNATURE:    return "bad signature";
    case BR_ERR_UNSUPPORTED_VERSION: return "unsupported TLS version";
    default: return "TLS error";
    }
}

// ------------------------------------------------------------------- fetch

// Case-insensitive header lookup within page[0..hlen); value into out.
static int header_find(int hlen, const char *name, char *out, int cap)
{
    int i = 0;
    while (i < hlen) {
        int m = ci_prefix(page + i, name);
        if (m) {
            i += m;
            while (i < hlen && (page[i] == ' ' || page[i] == '\t')) {
                i++;
            }
            int o = 0;
            while (i < hlen && page[i] != '\r' && page[i] != '\n' && o < cap - 1) {
                out[o++] = page[i++];
            }
            out[o] = '\0';
            return 0;
        }
        while (i < hlen && page[i] != '\n') {
            i++;
        }
        i++;
    }
    return -1;
}

// GET u into page[]; returns the body length (body moved to page[0..]), or
// -1 with g_err set. *status is the HTTP status code, *loc any Location
// header, *is_html from Content-Type (default yes).
static int fetch(const struct url *u, int *status, char *loc, int loccap,
                 int *is_html)
{
    int e = 0;
    g_err[0] = '\0';
    g_truncated = 0;
    *status = 0;
    loc[0] = '\0';
    *is_html = 1;

    unsigned ip;
    if (!parse_ip(u->host, &ip) && resolve(u->host, &ip) < 0) {
        sadd(g_err, &e, sizeof(g_err), "cannot resolve ");
        sadd(g_err, &e, sizeof(g_err), u->host);
        return -1;
    }
    int fd = connect(ip, u->port);
    if (fd < 0) {
        sadd(g_err, &e, sizeof(g_err), "connect failed");
        return -1;
    }

    if (u->tls) {
        br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
        // BearSSL counts days from year 0; Unix day 0 is day 719528.
        unsigned now = time_unix();
        if (now) {
            br_x509_minimal_set_time(&xc, now / 86400 + 719528, now % 86400);
        }
        br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);
        inject_entropy();
        br_ssl_client_reset(&sc, u->host, 0);
        br_sslio_init(&ioc, &sc.eng, sock_read, &fd, sock_write, &fd);
    }

    char req[1024];
    int n = 0;
    sadd(req, &n, sizeof(req), "GET ");
    sadd(req, &n, sizeof(req), u->path);
    sadd(req, &n, sizeof(req), " HTTP/1.0\r\nHost: ");
    sadd(req, &n, sizeof(req), u->host);
    sadd(req, &n, sizeof(req), "\r\nUser-Agent: NOS-browser/0.1\r\nConnection: close\r\n\r\n");
    if (n >= (int)sizeof(req) - 1) {
        close(fd);
        sadd(g_err, &e, sizeof(g_err), "url too long");
        return -1;
    }

    int total = 0;
    if (u->tls) {
        if (br_sslio_write_all(&ioc, req, n) < 0 || br_sslio_flush(&ioc) < 0) {
            int err = br_ssl_engine_last_error(&sc.eng);
            close(fd);
            sadd(g_err, &e, sizeof(g_err), tls_strerror(err));
            return -1;
        }
        int got;
        while (total < PAGE_CAP - 1 &&
               (got = br_sslio_read(&ioc, page + total, PAGE_CAP - 1 - total)) > 0) {
            total += got;
        }
        int err = br_ssl_engine_last_error(&sc.eng);
        close(fd);
        // close_notify or raw TCP EOF are both normal HTTP/1.0 endings; any
        // other error after data is a truncated-but-renderable page.
        if (err != BR_ERR_OK && err != BR_ERR_IO && total == 0) {
            sadd(g_err, &e, sizeof(g_err), tls_strerror(err));
            return -1;
        }
    } else {
        if (write(fd, req, n) != n) {
            close(fd);
            sadd(g_err, &e, sizeof(g_err), "send failed");
            return -1;
        }
        int got;
        while (total < PAGE_CAP - 1 &&
               (got = read(fd, page + total, PAGE_CAP - 1 - total)) > 0) {
            total += got;
        }
        close(fd);
        if (got < 0 && total == 0) {
            sadd(g_err, &e, sizeof(g_err), "connection error");
            return -1;
        }
    }
    if (total >= PAGE_CAP - 1) {
        g_truncated = 1;
    }
    page[total] = '\0';
    if (total == 0) {
        sadd(g_err, &e, sizeof(g_err), "empty reply");
        return -1;
    }

    // Split headers from body ("\r\n\r\n", tolerating bare "\n\n").
    int body = -1, hlen = 0;
    for (int i = 0; i + 1 < total; i++) {
        if (page[i] == '\n' && (page[i + 1] == '\n' ||
            (page[i + 1] == '\r' && i + 2 < total && page[i + 2] == '\n'))) {
            hlen = i + 1;
            body = i + (page[i + 1] == '\n' ? 2 : 3);
            break;
        }
    }
    if (body < 0 || ci_prefix(page, "http/") == 0) {
        // Not HTTP -- treat the whole thing as a plain-text body.
        *is_html = 0;
        *status = 200;
        return total;
    }
    int i = 0;
    while (i < hlen && page[i] != ' ') {
        i++;
    }
    int st = 0;
    while (++i < hlen && page[i] >= '0' && page[i] <= '9') {
        st = st * 10 + (page[i] - '0');
    }
    *status = st;
    header_find(hlen, "location:", loc, loccap);
    char ctype[128];
    if (header_find(hlen, "content-type:", ctype, sizeof(ctype)) == 0 &&
        !ci_contains(ctype, slen(ctype), "html")) {
        *is_html = 0;
    }
    int blen = total - body;
    for (int k = 0; k < blen; k++) {
        page[k] = page[body + k];
    }
    page[blen] = '\0';
    return blen;
}

// ------------------------------------------------------- charset decoding

// Latin-1 0xA0..0xFF -> CP437 glyph (the BIOS font), '?' where there is
// none, 0 = drop (soft hyphen). Covers the accented letters the font has.
static const unsigned char latin1_cp437[96] = {
    ' ',  0xAD, 0x9B, 0x9C, '?',  0x9D, '|',  0x15, '"',  'c',  0xA6, 0xAE,
    0xAA, 0,    'r',  '-',  0xF8, 0xF1, 0xFD, '3',  '\'', 0xE6, 0x14, 0xFA,
    ',',  '1',  0xA7, 0xAF, 0xAC, 0xAB, '?',  0xA8, 'A',  'A',  'A',  'A',
    0x8E, 0x8F, 0x92, 0x80, 'E',  0x90, 'E',  'E',  'I',  'I',  'I',  'I',
    'D',  0xA5, 'O',  'O',  'O',  'O',  0x99, 'x',  'O',  'U',  'U',  'U',
    0x9A, 'Y',  '?',  0xE1, 0x85, 0xA0, 0x83, 'a',  0x84, 0x86, 0x91, 0x87,
    0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B, '?',  0xA4, 0x95, 0xA2,
    0x93, 'o',  0x94, 0xF6, 'o',  0x97, 0xA3, 0x96, 0x81, 'y',  '?',  0x98,
};

static char cp_to_char(unsigned cp)
{
    if (cp < 0x80) {
        return (char)cp;
    }
    if (cp >= 0xA0 && cp <= 0xFF) {
        return (char)latin1_cp437[cp - 0xA0];
    }
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: return '\'';
    case 0x201C: case 0x201D: case 0x201E: return '"';
    case 0x2010: case 0x2011: case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2022: return 0x07; // bullet
    case 0x00B7: return (char)0xFA;
    case 0x2190: return 0x1B; // arrows
    case 0x2191: return 0x18;
    case 0x2192: return 0x1A;
    case 0x2193: return 0x19;
    case 0x20AC: return 'E';
    case 0x200B: case 0xFEFF: return 0; // zero-width
    }
    return '?';
}

// Decode one UTF-8 sequence at src[i]; invalid bytes fall back to Latin-1
// (so legacy pages still mostly render). Returns the next index.
static int utf8_decode(const char *src, int n, int i, unsigned *cp)
{
    unsigned char c = src[i];
    int need, min;
    unsigned v;
    if (c < 0x80) {
        *cp = c;
        return i + 1;
    }
    if ((c & 0xE0) == 0xC0) {
        v = c & 0x1F; need = 1; min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        v = c & 0x0F; need = 2; min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        v = c & 0x07; need = 3; min = 0x10000;
    } else {
        *cp = c;
        return i + 1;
    }
    for (int k = 1; k <= need; k++) {
        if (i + k >= n || (src[i + k] & 0xC0) != 0x80) {
            *cp = c;
            return i + 1;
        }
        v = (v << 6) | (src[i + k] & 0x3F);
    }
    if ((int)v < min) {
        *cp = c;
        return i + 1;
    }
    *cp = v;
    return i + 1 + need;
}

static const struct { const char *name; unsigned cp; } entities[] = {
    { "amp", '&' },     { "lt", '<' },      { "gt", '>' },
    { "quot", '"' },    { "apos", '\'' },   { "nbsp", 0xA0 },
    { "copy", 0xA9 },   { "reg", 0xAE },    { "deg", 0xB0 },
    { "middot", 0xB7 }, { "laquo", 0xAB },  { "raquo", 0xBB },
    { "szlig", 0xDF },  { "auml", 0xE4 },   { "ouml", 0xF6 },
    { "uuml", 0xFC },   { "Auml", 0xC4 },   { "Ouml", 0xD6 },
    { "Uuml", 0xDC },   { "eacute", 0xE9 }, { "egrave", 0xE8 },
    { "agrave", 0xE0 }, { "ccedil", 0xE7 }, { "ndash", 0x2013 },
    { "mdash", 0x2014 },{ "lsquo", 0x2018 },{ "rsquo", 0x2019 },
    { "ldquo", 0x201C },{ "rdquo", 0x201D },{ "hellip", 0x2026 },
    { "bull", 0x2022 }, { "times", 0xD7 },  { "shy", 0xAD },
};

// -------------------------------------------------------------- layout

static int cur_line, cur_x, indent;
static int pre_depth, bold_depth, heading, link_cur;
static int pending_space, did_emit, layout_full;
static int in_title, tlen, title_pend;
static char runbuf[160];
static int runlen;

static unsigned style_color(void)
{
    if (link_cur >= 0) {
        return COL_LINK;
    }
    if (heading) {
        return COL_HEAD;
    }
    return COL_TEXT;
}

static unsigned char style_flags(void)
{
    unsigned char f = 0;
    if (link_cur >= 0) {
        f |= F_UL;
    }
    if (bold_depth > 0 || heading) {
        f |= F_BOLD;
    }
    return f;
}

// Pending block breaks are applied lazily so consecutive block tags don't
// pile up empty lines and pages never start with a gap.
static int want_break, want_blank;

static void apply_breaks(void)
{
    if (!did_emit) {
        want_break = want_blank = 0;
        cur_x = indent;
        return;
    }
    if (want_blank) {
        cur_line += (cur_x > indent) ? 2 : 1;
    } else if (want_break && cur_x > indent) {
        cur_line += 1;
    }
    if (want_break || want_blank) {
        cur_x = indent;
        pending_space = 0;
    }
    want_break = want_blank = 0;
}

static void emit_word(const char *w, int len, unsigned color, unsigned char flags)
{
    if (layout_full || len <= 0) {
        return;
    }
    apply_breaks();
    int right = MARGIN + content_w;
    int first = 1;
    while (len > 0) {
        if (nspans >= MAX_SPANS || textlen + len > TEXT_CAP) {
            layout_full = 1;
            return;
        }
        if (first && pending_space && cur_x > indent) {
            cur_x += CHAR_W;
        }
        if (cur_x + len * CHAR_W > right && cur_x > indent) {
            cur_line++;
            cur_x = indent;
        }
        int maxc = (right - cur_x) / CHAR_W;
        if (maxc < 1) {
            maxc = 1; // degenerate indent: still make progress
        }
        int take = len < maxc ? len : maxc;
        struct span *s = &spans[nspans++];
        s->x = cur_x;
        s->line = cur_line;
        s->off = textlen;
        s->len = (short)take;
        s->link = (short)link_cur;
        s->color = color;
        s->flags = flags;
        for (int k = 0; k < take; k++) {
            text[textlen++] = w[k];
        }
        cur_x += take * CHAR_W;
        w += take;
        len -= take;
        if (len > 0) {
            cur_line++;
            cur_x = indent;
        }
        first = 0;
    }
    pending_space = 0;
    did_emit = 1;
}

static void flush_run(void)
{
    if (runlen) {
        emit_word(runbuf, runlen, style_color(), style_flags());
        runlen = 0;
    }
}

static void newline_now(void)
{
    flush_run();
    apply_breaks();
    if (did_emit) {
        cur_line++;
    }
    cur_x = indent;
    pending_space = 0;
    did_emit = 1; // a <br> at the top of the page still takes a line
}

static void emit_rule(void)
{
    flush_run();
    apply_breaks();
    if (nspans >= MAX_SPANS) {
        layout_full = 1;
        return;
    }
    struct span *s = &spans[nspans++];
    s->x = indent;
    s->line = cur_line;
    s->off = 0;
    s->len = 0;
    s->link = -1;
    s->color = COL_RULE;
    s->flags = F_RULE;
    cur_line++;
    cur_x = indent;
    did_emit = 1;
}

// One decoded character of document text (by codepoint).
static void text_char(unsigned cp)
{
    if (in_title) {
        char mc = cp_to_char(cp);
        if (!mc) {
            return;
        }
        if (mc == ' ' || mc == '\n' || mc == '\t' || mc == '\r') {
            title_pend = 1;
            return;
        }
        if (title_pend && tlen && tlen < (int)sizeof(title) - 1) {
            title[tlen++] = ' ';
        }
        title_pend = 0;
        if (tlen < (int)sizeof(title) - 1) {
            title[tlen++] = mc;
            title[tlen] = '\0';
        }
        return;
    }
    if (layout_full) {
        return;
    }
    if (pre_depth) {
        if (cp == '\n') {
            newline_now();
            return;
        }
        if (cp == '\r') {
            return;
        }
        char mc = (cp == '\t') ? ' ' : cp_to_char(cp);
        if (!mc) {
            return;
        }
        int rep = (cp == '\t') ? 4 : 1;
        while (rep--) {
            if (runlen >= (int)sizeof(runbuf)) {
                flush_run();
            }
            runbuf[runlen++] = mc;
        }
        return;
    }
    if (cp == ' ' || cp == '\n' || cp == '\t' || cp == '\r' || cp == '\f') {
        flush_run();
        pending_space = 1;
        return;
    }
    char mc = cp_to_char(cp);
    if (!mc) {
        return;
    }
    if (runlen >= (int)sizeof(runbuf)) {
        flush_run();
    }
    runbuf[runlen++] = mc;
}

// Feed a C string of ASCII through the text path (used for alt text).
static void text_str(const char *s)
{
    while (*s) {
        text_char((unsigned char)*s++);
    }
}

static const char *skiptag; // "script"/"style": swallow content until close

static void tag_open_a(const char *href)
{
    if (!href[0] || nlinks >= MAX_LINKS) {
        return;
    }
    scopy(links[nlinks].href, href, URL_CAP);
    link_cur = nlinks;
    nlinks++;
    // lynx-style visible number so links can be followed from the keyboard
    char nb[8];
    int o = 0;
    sadd(nb, &o, sizeof(nb), "[");
    saddi(nb, &o, sizeof(nb), nlinks);
    sadd(nb, &o, sizeof(nb), "]");
    emit_word(nb, o, COL_LINKNUM, 0);
}

static void dispatch_tag(const char *name, int closing, const char *href,
                         const char *alt)
{
    if (streq(name, "br")) {
        newline_now();
    } else if (streq(name, "p")) {
        want_blank = 1;
    } else if (streq(name, "div") || streq(name, "tr") || streq(name, "table") ||
               streq(name, "section") || streq(name, "article") ||
               streq(name, "header") || streq(name, "footer") ||
               streq(name, "nav") || streq(name, "form") ||
               streq(name, "aside") || streq(name, "main")) {
        want_break = 1;
    } else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        want_blank = 1;
        heading = closing ? 0 : name[1] - '0';
    } else if (streq(name, "title")) {
        in_title = !closing;
        if (!closing) {
            tlen = 0;
            title[0] = '\0';
            title_pend = 0;
        }
    } else if (streq(name, "a")) {
        if (closing) {
            link_cur = -1;
        } else {
            tag_open_a(href);
        }
    } else if (streq(name, "b") || streq(name, "strong")) {
        bold_depth += closing ? -1 : 1;
        if (bold_depth < 0) {
            bold_depth = 0;
        }
    } else if (streq(name, "ul") || streq(name, "ol") || streq(name, "dl")) {
        want_break = 1;
        indent += closing ? -16 : 16;
        if (indent < MARGIN) {
            indent = MARGIN;
        }
        if (indent > MARGIN + content_w / 2) {
            indent = MARGIN + content_w / 2;
        }
    } else if (streq(name, "li")) {
        if (!closing) {
            want_break = 1;
            char bullet = 0x07;
            emit_word(&bullet, 1, COL_TEXT, 0);
            pending_space = 1;
        } else {
            want_break = 1;
        }
    } else if (streq(name, "blockquote")) {
        want_blank = 1;
        indent += closing ? -24 : 24;
        if (indent < MARGIN) {
            indent = MARGIN;
        }
        if (indent > MARGIN + content_w / 2) {
            indent = MARGIN + content_w / 2;
        }
    } else if (streq(name, "pre")) {
        flush_run();
        want_blank = 1;
        pre_depth += closing ? -1 : 1;
        if (pre_depth < 0) {
            pre_depth = 0;
        }
    } else if (streq(name, "hr")) {
        emit_rule();
    } else if (streq(name, "img")) {
        if (!closing && alt[0]) {
            text_char('[');
            text_str(alt);
            text_char(']');
        }
    } else if (streq(name, "td") || streq(name, "th")) {
        if (!closing) {
            pending_space = 1;
        }
    } else if (streq(name, "script") || streq(name, "style")) {
        if (!closing) {
            skiptag = streq(name, "script") ? "script" : "style";
        }
    }
}

// Parse the tag starting at src[*i] == '<'; advances *i past the '>'.
static void parse_tag(const char *src, int n, int *i)
{
    int j = *i + 1;
    if (j < n && src[j] == '!') {
        if (j + 2 < n && src[j + 1] == '-' && src[j + 2] == '-') {
            j += 3;
            while (j + 2 < n && !(src[j] == '-' && src[j + 1] == '-' && src[j + 2] == '>')) {
                j++;
            }
            *i = j + 2 < n ? j + 3 : n;
        } else {
            while (j < n && src[j] != '>') {
                j++;
            }
            *i = j < n ? j + 1 : n;
        }
        return;
    }
    int closing = 0;
    if (j < n && src[j] == '/') {
        closing = 1;
        j++;
    }
    char name[12];
    int nl = 0;
    while (j < n && ((src[j] >= 'a' && src[j] <= 'z') || (src[j] >= 'A' && src[j] <= 'Z') ||
                     (src[j] >= '0' && src[j] <= '9'))) {
        if (nl < (int)sizeof(name) - 1) {
            name[nl++] = lower(src[j]);
        }
        j++;
    }
    name[nl] = '\0';
    if (!nl) { // stray '<': render it as text
        text_char('<');
        *i += 1;
        return;
    }

    char href[URL_CAP], alt[128];
    href[0] = alt[0] = '\0';
    while (j < n && src[j] != '>') {
        while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' ||
                         src[j] == '\r' || src[j] == '/')) {
            j++;
        }
        if (j >= n || src[j] == '>') {
            break;
        }
        char an[16];
        int al = 0;
        while (j < n && src[j] != '=' && src[j] != '>' && src[j] != ' ' &&
               src[j] != '\t' && src[j] != '\n' && src[j] != '\r' && src[j] != '/') {
            if (al < (int)sizeof(an) - 1) {
                an[al++] = lower(src[j]);
            }
            j++;
        }
        an[al] = '\0';
        char val[URL_CAP];
        int vl = 0;
        val[0] = '\0';
        while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) {
            j++;
        }
        if (j < n && src[j] == '=') {
            j++;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) {
                j++;
            }
            if (j < n && (src[j] == '"' || src[j] == '\'')) {
                char q = src[j++];
                while (j < n && src[j] != q) {
                    if (vl < (int)sizeof(val) - 1) {
                        val[vl++] = src[j];
                    }
                    j++;
                }
                if (j < n) {
                    j++;
                }
            } else {
                while (j < n && src[j] != '>' && src[j] != ' ' && src[j] != '\t' &&
                       src[j] != '\n' && src[j] != '\r') {
                    if (vl < (int)sizeof(val) - 1) {
                        val[vl++] = src[j];
                    }
                    j++;
                }
            }
            val[vl] = '\0';
        }
        if (streq(an, "href")) {
            scopy(href, val, sizeof(href));
        } else if (streq(an, "alt")) {
            scopy(alt, val, sizeof(alt));
        }
    }
    *i = j < n ? j + 1 : n;

    flush_run();
    dispatch_tag(name, closing, href, alt);
}

// Entity at src[i] == '&'; decodes into text_char, returns the next index.
static int parse_entity(const char *src, int n, int i)
{
    char ent[12];
    int e = 0, j = i + 1;
    while (j < n && e < (int)sizeof(ent) - 1 &&
           ((src[j] >= 'a' && src[j] <= 'z') || (src[j] >= 'A' && src[j] <= 'Z') ||
            (src[j] >= '0' && src[j] <= '9') || src[j] == '#' || src[j] == 'x' ||
            src[j] == 'X')) {
        ent[e++] = src[j++];
    }
    ent[e] = '\0';
    if (j >= n || src[j] != ';' || !e) {
        text_char('&');
        return i + 1;
    }
    if (ent[0] == '#') {
        unsigned cp = 0;
        if (ent[1] == 'x' || ent[1] == 'X') {
            for (int k = 2; ent[k]; k++) {
                char c = lower(ent[k]);
                int d = (c >= '0' && c <= '9') ? c - '0'
                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
                if (d < 0) {
                    text_char('&');
                    return i + 1;
                }
                cp = cp * 16 + d;
            }
        } else {
            for (int k = 1; ent[k]; k++) {
                if (ent[k] < '0' || ent[k] > '9') {
                    text_char('&');
                    return i + 1;
                }
                cp = cp * 10 + (ent[k] - '0');
            }
        }
        text_char(cp);
        return j + 1;
    }
    for (unsigned k = 0; k < sizeof(entities) / sizeof(entities[0]); k++) {
        if (streq(ent, entities[k].name)) {
            text_char(entities[k].cp);
            return j + 1;
        }
    }
    text_char('&');
    return i + 1;
}

static void layout_reset(void)
{
    nspans = nlinks = textlen = 0;
    cur_line = 0;
    indent = MARGIN;
    cur_x = indent;
    pre_depth = bold_depth = heading = 0;
    link_cur = -1;
    pending_space = did_emit = layout_full = 0;
    want_break = want_blank = 0;
    in_title = tlen = title_pend = 0;
    runlen = 0;
    skiptag = 0;
    title[0] = '\0';
}

static void layout_html(const char *src, int n)
{
    layout_reset();
    int i = 0;
    while (i < n) {
        if (skiptag) {
            if (src[i] == '<' && i + 1 < n && src[i + 1] == '/') {
                int k = i + 2, m = 0;
                while (skiptag[m] && k < n && lower(src[k]) == skiptag[m]) {
                    k++;
                    m++;
                }
                if (!skiptag[m]) {
                    while (k < n && src[k] != '>') {
                        k++;
                    }
                    i = k < n ? k + 1 : n;
                    skiptag = 0;
                    continue;
                }
            }
            i++;
            continue;
        }
        if (src[i] == '<') {
            parse_tag(src, n, &i);
            continue;
        }
        if (src[i] == '&') {
            i = parse_entity(src, n, i);
            continue;
        }
        unsigned cp;
        i = utf8_decode(src, n, i, &cp);
        text_char(cp);
    }
    flush_run();
    nlines = cur_line + 1;
}

static void layout_plain(const char *src, int n)
{
    layout_reset();
    pre_depth = 1;
    for (int i = 0; i < n; i++) {
        text_char((unsigned char)src[i]);
    }
    flush_run();
    pre_depth = 0;
    nlines = cur_line + 1;
}

// ------------------------------------------------------------- rendering

static void draw_span_text(const struct span *s, int y)
{
    for (int k = 0; k < s->len; k++) {
        gfx_char(&bb, s->x + k * CHAR_W, y, text[s->off + k], s->color);
        if (s->flags & F_BOLD) {
            gfx_char(&bb, s->x + k * CHAR_W + 1, y, text[s->off + k], s->color);
        }
    }
    if (s->flags & F_UL) {
        gfx_fill(&bb, s->x, y + 14, s->len * CHAR_W, 1, s->color);
    }
}

static void draw_text_clipped(int x, int y, const char *s, int maxchars,
                              unsigned color)
{
    for (int i = 0; s[i] && i < maxchars; i++) {
        gfx_char(&bb, x + i * CHAR_W, y, s[i], color);
    }
}

static int max_scroll(void)
{
    return nlines > vis_lines ? nlines - vis_lines : 0;
}

static void draw(void)
{
    int w = bb.w, h = bb.h;
    gfx_fill(&bb, 0, 0, w, TOPBAR_H, COL_CHROME);
    gfx_fill(&bb, 0, TOPBAR_H, w, h - TOPBAR_H - STATUS_H, COL_PAPER);
    gfx_fill(&bb, 0, h - STATUS_H, w, STATUS_H, COL_CHROME);
    gfx_fill(&bb, 0, TOPBAR_H - 1, w, 1, 0x00606060);
    gfx_fill(&bb, 0, h - STATUS_H, w, 1, 0x00606060);

    // Top bar: current URL left, page title right.
    int urlchars = (w - 2 * MARGIN) / CHAR_W;
    int titlechars = slen(title);
    if (titlechars) {
        int tx = w - MARGIN - titlechars * CHAR_W;
        if (tx < w / 2) {
            tx = w / 2;
            titlechars = (w - MARGIN - tx) / CHAR_W;
        }
        draw_text_clipped(tx, 4, title, titlechars, COL_HEAD);
        urlchars = (tx - 2 * MARGIN) / CHAR_W;
    }
    draw_text_clipped(MARGIN, 4, cur_url, urlchars, COL_TEXT);

    for (int i = 0; i < nspans; i++) {
        const struct span *s = &spans[i];
        if (s->line < scroll_line || s->line >= scroll_line + vis_lines) {
            continue;
        }
        int y = TOPBAR_H + 4 + (s->line - scroll_line) * LINE_H;
        if (s->flags & F_RULE) {
            gfx_fill(&bb, MARGIN, y + LINE_H / 2, content_w, 1, s->color);
        } else {
            draw_span_text(s, y);
        }
    }

    // Scrollbar on the right edge of the content area.
    if (nlines > vis_lines) {
        int track_y = TOPBAR_H, track_h = h - TOPBAR_H - STATUS_H;
        gfx_fill(&bb, w - 5, track_y, 5, track_h, 0x00E0E0E0);
        int thumb_h = track_h * vis_lines / nlines;
        if (thumb_h < 16) {
            thumb_h = 16;
        }
        int thumb_y = track_y + (track_h - thumb_h) * scroll_line / max_scroll();
        gfx_fill(&bb, w - 5, thumb_y, 5, thumb_h, 0x00707070);
    }

    // Status bar: input state or the last message, plus the key help.
    char left[160];
    int o = 0;
    left[0] = '\0';
    if (mode == MODE_URL) {
        sadd(left, &o, sizeof(left), "url: ");
        sadd(left, &o, sizeof(left), urlbuf);
        sadd(left, &o, sizeof(left), "_");
    } else if (numlen) {
        sadd(left, &o, sizeof(left), "follow: ");
        sadd(left, &o, sizeof(left), numbuf);
        sadd(left, &o, sizeof(left), "_");
    } else {
        sadd(left, &o, sizeof(left), statusmsg);
    }
    static const char help[] = "g url  j/k/spc scroll  n+ret follow  b back  q quit";
    int helpchars = (int)sizeof(help) - 1;
    int hx = w - MARGIN - helpchars * CHAR_W;
    draw_text_clipped(hx, h - STATUS_H + 2, help, helpchars, 0x00404040);
    draw_text_clipped(MARGIN, h - STATUS_H + 2, left, (hx - 2 * MARGIN) / CHAR_W,
                      COL_TEXT);

    if (!windowed) { // in a window, wm draws the (global) cursor
        struct mouse_state m;
        if (mouse(&m) == 0) {
            gfx_cursor(&bb, m.x, m.y);
        }
    }

    for (int y = 0; y < h; y++) {
        memcpy((void *)((char *)fb + y * out_pitch), bb.buf + y * w, w * 4);
    }
}

// ------------------------------------------------------------ navigation

static const char home_html[] =
    "<title>NOS browser</title>"
    "<h1>NOS browser</h1>"
    "<p>A tiny HTML browser: HTTP/1.0 + TLS, no CSS, no JavaScript.</p>"
    "<p><b>g</b> enter a url &middot; <b>j</b>/<b>k</b>/<b>space</b>/<b>u</b>"
    " scroll &middot; type a link number + <b>enter</b> (or click) to follow"
    " &middot; <b>b</b> back &middot; <b>r</b> reload &middot; <b>q</b> quit</p>"
    "<hr>"
    // google.com chains to GTS Root R1, one of the embedded trust anchors;
    // most other sites' chains won't verify (wget -k exists, browser has no
    // equivalent yet).
    "<p>Try: <a href=\"https://google.com/\">google.com</a></p>";

static void set_status(const char *s)
{
    scopy(statusmsg, s, sizeof(statusmsg));
}

static void navigate(const char *absurl, int push)
{
    char url[URL_CAP];
    scopy(url, absurl, sizeof(url));

    int status = 0, is_html = 1, blen;
    if (streq(url, "about:home")) {
        blen = -2; // sentinel: builtin page, nothing fetched
    } else {
        // Paint before fetching: a TLS handshake takes seconds on an
        // emulated i386, and an unpainted framebuffer shows VRAM garbage.
        char msg[128];
        int o = 0;
        msg[0] = '\0';
        sadd(msg, &o, sizeof(msg), "loading ");
        sadd(msg, &o, sizeof(msg), url);
        sadd(msg, &o, sizeof(msg), " ...");
        set_status(msg);
        draw();
        int hops = 0;
        for (;;) {
            struct url u;
            if (parse_url(url, &u) < 0) {
                set_status("bad url");
                put("browser: error bad url\n");
                return;
            }
            char loc[URL_CAP];
            blen = fetch(&u, &status, loc, sizeof(loc), &is_html);
            if (blen < 0) {
                set_status(g_err);
                put("browser: error ");
                put(g_err);
                put("\n");
                dirty = 1;
                return;
            }
            if (status >= 300 && status < 400 && loc[0] && hops < MAX_HOPS) {
                char nu[URL_CAP];
                if (resolve_url(url, loc, nu, sizeof(nu)) == 0) {
                    scopy(url, nu, sizeof(url));
                    put("browser: redirect ");
                    put(url);
                    put("\n");
                    o = 0;
                    msg[0] = '\0';
                    sadd(msg, &o, sizeof(msg), "redirect: ");
                    sadd(msg, &o, sizeof(msg), url);
                    set_status(msg);
                    draw();
                    hops++;
                    continue;
                }
            }
            break;
        }
    }

    if (push && cur_url[0]) {
        if (histn == MAX_HIST) { // full: forget the oldest entry
            for (int i = 1; i < MAX_HIST; i++) {
                scopy(hist[i - 1], hist[i], URL_CAP);
            }
            histn--;
        }
        scopy(hist[histn++], cur_url, URL_CAP);
    }
    scopy(cur_url, url, sizeof(cur_url));

    if (blen == -2) {
        layout_html(home_html, sizeof(home_html) - 1);
        set_status("welcome");
        put("browser: home\n");
    } else {
        if (is_html) {
            layout_html(page, blen);
        } else {
            layout_plain(page, blen);
        }
        char msg[128];
        int o = 0;
        msg[0] = '\0';
        saddi(msg, &o, sizeof(msg), status);
        sadd(msg, &o, sizeof(msg), ", ");
        saddi(msg, &o, sizeof(msg), blen);
        sadd(msg, &o, sizeof(msg), " bytes, ");
        saddi(msg, &o, sizeof(msg), nlinks);
        sadd(msg, &o, sizeof(msg), " links");
        if (g_truncated) {
            sadd(msg, &o, sizeof(msg), " (truncated)");
        }
        if (layout_full) {
            sadd(msg, &o, sizeof(msg), " (partial layout)");
        }
        set_status(msg);
        put("browser: loaded ");
        put(cur_url);
        put(" status ");
        puti(status);
        put(" len ");
        puti(blen);
        put("\n");
        if (title[0]) {
            put("browser: title ");
            put(title);
            put("\n");
        }
    }
    scroll_line = 0;
    numlen = 0;
    numbuf[0] = '\0';
    dirty = 1;
}

static void follow_link(int idx)
{
    if (idx < 0 || idx >= nlinks) {
        set_status("no such link");
        return;
    }
    char nu[URL_CAP];
    if (resolve_url(cur_url, links[idx].href, nu, sizeof(nu)) < 0) {
        set_status("link not followable");
        return;
    }
    navigate(nu, 1);
}

static void go_back(void)
{
    if (!histn) {
        set_status("no history");
        return;
    }
    char nu[URL_CAP];
    scopy(nu, hist[--histn], sizeof(nu));
    navigate(nu, 0);
}

static void quit(void)
{
    if (!windowed) {
        fboff();
    }
    put("browser: quit\n");
    exit(0);
}

// ----------------------------------------------------------------- input

static void scroll_by(int delta)
{
    scroll_line += delta;
    if (scroll_line > max_scroll()) {
        scroll_line = max_scroll();
    }
    if (scroll_line < 0) {
        scroll_line = 0;
    }
}

static void handle_key(int c)
{
    dirty = 1;
    if (mode == MODE_URL) {
        if (c == 27) { // esc
            mode = MODE_PAGE;
        } else if (c == '\n') {
            mode = MODE_PAGE;
            if (urllen) {
                char abs[URL_CAP];
                make_absolute(urlbuf, abs, sizeof(abs));
                navigate(abs, 1);
            }
        } else if (c == '\b') {
            if (urllen) {
                urlbuf[--urllen] = '\0';
            }
        } else if (c >= 32 && c < 127 && urllen < (int)sizeof(urlbuf) - 1) {
            urlbuf[urllen++] = (char)c;
            urlbuf[urllen] = '\0';
        }
        return;
    }
    if (c >= '0' && c <= '9') {
        if (numlen < (int)sizeof(numbuf) - 1) {
            numbuf[numlen++] = (char)c;
            numbuf[numlen] = '\0';
        }
        return;
    }
    if (c == '\n') {
        if (numlen) {
            int v = 0;
            for (int i = 0; i < numlen; i++) {
                v = v * 10 + (numbuf[i] - '0');
            }
            numlen = 0;
            numbuf[0] = '\0';
            follow_link(v - 1);
        }
        return;
    }
    if (c == '\b' || c == 27) {
        numlen = 0;
        numbuf[0] = '\0';
        return;
    }
    switch (c) {
    case 'q': quit(); break;
    case 'j': scroll_by(1); break;
    case 'k': scroll_by(-1); break;
    case ' ': scroll_by(vis_lines - 1); break;
    case 'u': scroll_by(-(vis_lines - 1)); break;
    case 'b': go_back(); break;
    case 'r': navigate(cur_url, 0); break;
    case 'g':
        mode = MODE_URL;
        urllen = 0;
        urlbuf[0] = '\0';
        break;
    }
}

static void handle_click(int x, int y)
{
    dirty = 1;
    if (y < TOPBAR_H) {
        mode = MODE_URL;
        urllen = 0;
        urlbuf[0] = '\0';
        return;
    }
    if (y >= bb.h - STATUS_H) {
        return;
    }
    int line = scroll_line + (y - TOPBAR_H - 4) / LINE_H;
    for (int i = 0; i < nspans; i++) {
        const struct span *s = &spans[i];
        if (s->line == line && s->link >= 0 && x >= s->x &&
            x < s->x + s->len * CHAR_W) {
            follow_link(s->link);
            return;
        }
    }
}

// ------------------------------------------------------------------ main

void _start(int argc, char **argv)
{
    if (fbinfo(&info) != 0) {
        put("browser: no framebuffer\n");
        exit(1);
    }
    if (gfx_init_font() != 0) {
        put("browser: no font\n");
        exit(1);
    }

    // Fullscreen when the display is free; otherwise (wm owns it) render
    // into a window surface that wm composites and routes input to.
    fb = fbmap();
    if (fb != (volatile unsigned int *)-1) {
        bb.w = (int)info.width;
        bb.h = (int)info.height;
        out_pitch = info.pitch;
    } else {
        fb = wcreate(WIN_W, WIN_H);
        if (fb == (volatile unsigned int *)-1) {
            put("browser: display busy and no window surface available\n");
            exit(1);
        }
        windowed = 1;
        bb.w = WIN_W;
        bb.h = WIN_H;
        out_pitch = WIN_W * 4;
    }

    bb.buf = sbrk(bb.w * bb.h * 4);
    page = sbrk(PAGE_CAP);
    text = sbrk(TEXT_CAP);
    spans = sbrk(MAX_SPANS * (int)sizeof(struct span));
    links = sbrk(MAX_LINKS * (int)sizeof(struct linkent));
    if (bb.buf == (void *)-1 || page == (void *)-1 || text == (void *)-1 ||
        spans == (void *)-1 || links == (void *)-1) {
        put("browser: out of memory\n");
        exit(1);
    }
    content_w = bb.w - 2 * MARGIN;
    vis_lines = (bb.h - TOPBAR_H - STATUS_H - 8) / LINE_H;

    draw(); // output is live now; never show leftover/blank content

    if (argc > 1) {
        char abs[URL_CAP];
        make_absolute(argv[1], abs, sizeof(abs));
        navigate(abs, 0);
    } else {
        navigate("about:home", 0);
    }

    int mx = -1, my = -1, lbtn = 0;
    for (;;) {
        if (windowed) {
            // All input arrives as events forwarded by wm; wm also draws
            // the cursor, so mouse motion alone needs no repaint.
            struct wev ev;
            while (wevent(&ev) == 0) {
                if (ev.type == WEV_KEY) {
                    handle_key(ev.a);
                } else if (ev.type == WEV_MOUSE) {
                    mx = ev.a;
                    my = ev.b;
                    int lb = ev.c & 1;
                    if (lb && !lbtn) {
                        handle_click(mx, my);
                    }
                    lbtn = lb;
                } else if (ev.type == WEV_CLOSE) {
                    quit();
                }
            }
        } else {
            int c;
            while ((c = pollc()) > 0) {
                handle_key(c);
            }
            struct mouse_state m;
            if (mouse(&m) == 0) {
                if (m.x != mx || m.y != my) {
                    mx = m.x;
                    my = m.y;
                    dirty = 1;
                }
                int lb = m.buttons & 1;
                if (lb && !lbtn) {
                    handle_click(mx, my);
                }
                lbtn = lb;
            }
        }
        if (dirty) {
            draw();
            dirty = 0;
        }
        sleep(15);
    }
}
