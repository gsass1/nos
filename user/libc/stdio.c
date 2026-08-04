// Formatted output for NOS user programs. One core (format) walks the
// format string and pushes bytes at a sink: either a bounded buffer
// (vsnprintf, which still counts everything so the return value is the
// would-be length) or an fd batched through a small buffer to keep the
// syscall count sane. 'l'/'h' length modifiers parse and are ignored
// (ILP32: long == int); there are no 64-bit conversions because user
// programs link without libgcc's divide helpers.
#include "stdio.h"
#include "string.h"
#include "../ulib.h"

struct sink {
    int fd;         // fd sink; -1 for the buffer sink
    char *buf;      // buffer sink destination (may be null when cap == 0)
    size_t cap;     // buffer sink capacity, including the NUL
    char pend[128]; // fd sink: batched bytes not yet written
    size_t npend;
    size_t total; // everything emitted (or that would have been)
};

static void sink_flush(struct sink *o)
{
    if (o->npend) {
        write(o->fd, o->pend, (int)o->npend);
        o->npend = 0;
    }
}

static void emit(struct sink *o, char c)
{
    if (o->fd >= 0) {
        o->pend[o->npend++] = c;
        if (o->npend == sizeof(o->pend)) {
            sink_flush(o);
        }
    } else if (o->total + 1 < o->cap) {
        o->buf[o->total] = c;
    }
    o->total++;
}

// Render v in the given base into dst (most significant digit first);
// returns the digit count. dst must hold 11 bytes (32-bit octal worst case).
static int fmt_num(char *dst, unsigned v, unsigned base, int caps)
{
    const char *dig = caps ? "0123456789ABCDEF" : "0123456789abcdef";
    char rev[11];
    int n = 0;
    do {
        rev[n++] = dig[v % base];
        v /= base;
    } while (v);
    for (int i = 0; i < n; i++) {
        dst[i] = rev[n - 1 - i];
    }
    return n;
}

static void format(struct sink *o, const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') {
            emit(o, *f);
            continue;
        }
        f++;
        int left = 0, zero = 0;
        for (;; f++) {
            if (*f == '-') {
                left = 1;
            } else if (*f == '0') {
                zero = 1;
            } else {
                break;
            }
        }
        int width = 0;
        if (*f == '*') {
            width = va_arg(ap, int);
            if (width < 0) { // negative width means left-aligned
                left = 1;
                width = -width;
            }
            f++;
        } else {
            while (*f >= '0' && *f <= '9') {
                width = width * 10 + (*f++ - '0');
            }
        }
        int prec = -1;
        if (*f == '.') {
            f++;
            if (*f == '*') {
                prec = va_arg(ap, int); // negative means unspecified
                f++;
            } else {
                prec = 0;
                while (*f >= '0' && *f <= '9') {
                    prec = prec * 10 + (*f++ - '0');
                }
            }
        }
        while (*f == 'l' || *f == 'h') {
            f++;
        }

        char tmp[13];
        const char *s = tmp;
        int len, sign = 0, numeric = 1;
        switch (*f) {
        case 'd':
        case 'i': {
            int v = va_arg(ap, int);
            unsigned u = (unsigned)v;
            if (v < 0) {
                sign = 1;
                u = -u;
            }
            len = fmt_num(tmp, u, 10, 0);
            break;
        }
        case 'u':
            len = fmt_num(tmp, va_arg(ap, unsigned), 10, 0);
            break;
        case 'x':
            len = fmt_num(tmp, va_arg(ap, unsigned), 16, 0);
            break;
        case 'X':
            len = fmt_num(tmp, va_arg(ap, unsigned), 16, 1);
            break;
        case 'p':
            tmp[0] = '0';
            tmp[1] = 'x';
            len = 2 + fmt_num(tmp + 2, (unsigned)(size_t)va_arg(ap, void *), 16, 0);
            break;
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            len = 1;
            numeric = 0;
            break;
        case 's':
            s = va_arg(ap, const char *);
            if (!s) {
                s = "(null)";
            }
            len = 0;
            while (s[len] && (prec < 0 || len < prec)) {
                len++;
            }
            numeric = 0;
            break;
        case '%':
            emit(o, '%');
            continue;
        default: // unknown conversion: emit it verbatim
            emit(o, '%');
            if (!*f) {
                return;
            }
            emit(o, *f);
            continue;
        }

        // For integers, precision means minimum digits; otherwise the '0'
        // flag zero-pads to the field width ('-' and precision override it).
        int zeros = 0;
        if (numeric && prec > len) {
            zeros = prec - len;
        } else if (numeric && zero && !left && prec < 0 && width > len + sign) {
            zeros = width - len - sign;
        }
        int body = sign + zeros + len;
        if (!left) {
            for (int i = body; i < width; i++) {
                emit(o, ' ');
            }
        }
        if (sign) {
            emit(o, '-');
        }
        for (int i = 0; i < zeros; i++) {
            emit(o, '0');
        }
        for (int i = 0; i < len; i++) {
            emit(o, s[i]);
        }
        if (left) {
            for (int i = body; i < width; i++) {
                emit(o, ' ');
            }
        }
    }
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    struct sink o = { .fd = -1, .buf = buf, .cap = cap };
    format(&o, fmt, ap);
    if (cap) {
        buf[o.total < cap ? o.total : cap - 1] = '\0';
    }
    return (int)o.total;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
    struct sink o = { .fd = fd };
    format(&o, fmt, ap);
    sink_flush(&o);
    return (int)o.total;
}

int vprintf(const char *fmt, va_list ap)
{
    return vdprintf(1, fmt, ap);
}

int dprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(1, fmt, ap);
    va_end(ap);
    return n;
}

int putchar(int c)
{
    char ch = (char)c;
    write(1, &ch, 1);
    return (unsigned char)ch;
}

int puts(const char *s)
{
    write(1, s, (int)strlen(s));
    write(1, "\n", 1);
    return 0;
}
