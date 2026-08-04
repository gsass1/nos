// stdlib for NOS user programs: malloc/free plus strtol/atoi/abort.
//
// The allocator is K&R style: one circular free list ordered by address,
// first fit, split on malloc, coalesce with both neighbours on free. It is
// backed by sbrk(), which the kernel grows contiguously (sys_sbrk), so each
// morecore() extension merges with the top of the arena like any freed
// block. Programs may still call sbrk() directly; such holes simply never
// coalesce with malloc's blocks (the address checks are exact).
#include "stdlib.h"
#include "string.h"
#include "limits.h"
#include "../ulib.h"

union header {
    struct {
        union header *next; // next free block: circular, address-ordered
        size_t units;       // block size in header-sized units, incl. header
    } s;
    long long align; // forces 8-byte block (and so payload) alignment
};

#define NALLOC 1024 // minimum morecore() growth, in units (8KB)

static union header base;   // degenerate zero-length list head
static union header *freep; // where the last search left off

static void list_init(void)
{
    if (!freep) {
        base.s.next = freep = &base;
        base.s.units = 0;
    }
}

static union header *morecore(size_t nunits)
{
    if (nunits < NALLOC) {
        nunits = NALLOC;
    }
    void *p = sbrk((int)(nunits * sizeof(union header)));
    if (p == (void *)-1) {
        return 0;
    }
    union header *up = p;
    up->s.units = nunits;
    free(up + 1); // insert (and coalesce) via the normal path
    return freep;
}

void *malloc(size_t nbytes)
{
    if (nbytes == 0 || nbytes >= 0x08000000) { // heap window is 256MB
        return 0;
    }
    size_t nunits = (nbytes + sizeof(union header) - 1) / sizeof(union header) + 1;
    list_init();
    union header *prevp = freep;
    for (union header *p = prevp->s.next;; prevp = p, p = p->s.next) {
        if (p->s.units >= nunits) {
            if (p->s.units == nunits) {
                prevp->s.next = p->s.next;
            } else { // hand out the tail; the head stays listed as-is
                p->s.units -= nunits;
                p += p->s.units;
                p->s.units = nunits;
            }
            freep = prevp;
            return p + 1;
        }
        if (p == freep) { // wrapped around: nothing fits
            p = morecore(nunits);
            if (!p) {
                return 0;
            }
        }
    }
}

void free(void *ap)
{
    if (!ap) {
        return;
    }
    list_init();
    union header *bp = (union header *)ap - 1;
    union header *p = freep;
    for (; !(bp > p && bp < p->s.next); p = p->s.next) {
        if (p >= p->s.next && (bp > p || bp < p->s.next)) {
            break; // block belongs at one end of the arena
        }
    }
    if (bp + bp->s.units == p->s.next) { // join the block above
        bp->s.units += p->s.next->s.units;
        bp->s.next = p->s.next->s.next;
    } else {
        bp->s.next = p->s.next;
    }
    if (p + p->s.units == bp) { // join the block below
        p->s.units += bp->s.units;
        p->s.next = bp->s.next;
    } else {
        p->s.next = bp;
    }
    freep = p;
}

void *calloc(size_t nmemb, size_t size)
{
    if (size && nmemb > (size_t)-1 / size) {
        return 0;
    }
    size_t n = nmemb * size;
    void *p = malloc(n);
    if (p) {
        memset(p, 0, n);
    }
    return p;
}

void *realloc(void *ap, size_t nbytes)
{
    if (!ap) {
        return malloc(nbytes);
    }
    if (!nbytes) {
        free(ap);
        return 0;
    }
    union header *bp = (union header *)ap - 1;
    size_t old = (bp->s.units - 1) * sizeof(union header);
    if (old >= nbytes) {
        return ap; // shrinking in place is always fine
    }
    void *np = malloc(nbytes);
    if (!np) {
        return 0;
    }
    memcpy(np, ap, old);
    free(ap);
    return np;
}

static int digval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    return 99;
}

long strtol(const char *s, char **end, int base)
{
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) {
        p++;
    }
    int neg = 0;
    if (*p == '+' || *p == '-') {
        neg = *p == '-';
        p++;
    }
    if ((base == 0 || base == 16) && p[0] == '0' &&
        (p[1] == 'x' || p[1] == 'X') && digval(p[2]) < 16) {
        base = 16;
        p += 2;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    // Accumulate unsigned, clamped at the signed limit for this sign.
    unsigned long cut = neg ? (unsigned long)LONG_MAX + 1 : (unsigned long)LONG_MAX;
    unsigned long acc = 0;
    int any = 0, over = 0;
    for (; digval(*p) < base; p++) {
        unsigned long d = (unsigned long)digval(*p);
        if (acc > (cut - d) / (unsigned long)base) {
            over = 1;
        } else {
            acc = acc * (unsigned long)base + d;
        }
        any = 1;
    }
    if (end) {
        *end = (char *)(any ? p : s);
    }
    if (over) {
        return neg ? LONG_MIN : LONG_MAX;
    }
    if (neg) {
        return acc > (unsigned long)LONG_MAX ? LONG_MIN : -(long)acc;
    }
    return (long)acc;
}

int atoi(const char *s)
{
    return (int)strtol(s, 0, 10);
}

void abort(void)
{
    static const char msg[] = "abort()\n";
    write(2, msg, sizeof(msg) - 1);
    exit(-1);
    for (;;) { // exit never returns, but -O0 can't see that
    }
}
