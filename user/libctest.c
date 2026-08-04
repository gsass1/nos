// libctest -- self-checks for the user libc: malloc/free/calloc/realloc,
// printf/snprintf formatting, string.h, and strtol. Prints one FAIL line
// per broken check and a single all-clear marker the test suite polls for.
#include "ulib.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        failed++;
    }
}

static void check_fmt(const char *want, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (strcmp(buf, want) != 0 || n != (int)strlen(want)) {
        printf("FAIL: fmt \"%s\" -> \"%s\" (want \"%s\")\n", fmt, buf, want);
        failed++;
    }
}

static void test_fmt(void)
{
    check_fmt("-123", "%d", -123);
    check_fmt("2147483647", "%d", 2147483647);
    check_fmt("-2147483648", "%d", -2147483647 - 1);
    check_fmt("4294967295", "%u", 4294967295U);
    check_fmt("deadbeef", "%x", 0xdeadbeefU);
    check_fmt("BEEF", "%X", 0xbeefU);
    check_fmt("  42", "%4d", 42);
    check_fmt("42  ", "%-4d", 42);
    check_fmt("0042", "%04d", 42);
    check_fmt(" -42", "%4d", -42);
    check_fmt("-042", "%04d", -42);
    check_fmt("00123", "%.5d", 123);
    check_fmt("ab", "%.2s", "abcdef");
    check_fmt("   ab", "%5.2s", "abcdef");
    check_fmt("x=7", "x%c%d", '=', 7);
    check_fmt("100%", "%d%%", 100);
    check_fmt("(null)", "%s", (char *)0);
    check_fmt("   7", "%*d", 4, 7);
    check_fmt("0x10", "%p", (void *)16);

    char small[4];
    int n = snprintf(small, sizeof(small), "%d", 123456);
    check(n == 6 && strcmp(small, "123") == 0, "snprintf truncation");
    check(snprintf(0, 0, "%s!", "abcd") == 5, "snprintf(NULL,0) length");
}

static void test_string(void)
{
    check(strcmp("abc", "abc") == 0 && strcmp("abc", "abd") < 0 &&
          strcmp("b", "a") > 0, "strcmp");
    check(strncmp("abcde", "abcxx", 3) == 0 && strncmp("abc", "abd", 3) < 0,
          "strncmp");
    const char *h = "one two one";
    check(strchr(h, 'o') == h && strrchr(h, 'o') == h + 8 && !strchr(h, 'z'),
          "strchr/strrchr");
    check(strstr(h, "two") == h + 4 && strstr(h, "") == h &&
          !strstr(h, "three"), "strstr");

    char buf[32];
    strcpy(buf, "left");
    strcat(buf, "+right");
    strncat(buf, "XYZ", 1);
    check(strcmp(buf, "left+rightX") == 0, "strcpy/strcat/strncat");
    memset(buf, 'x', sizeof(buf));
    strncpy(buf, "ab", 5);
    check(buf[1] == 'b' && buf[2] == 0 && buf[4] == 0 && buf[5] == 'x',
          "strncpy pads");

    char *dup = strdup("copy me");
    check(dup && dup != h && strcmp(dup, "copy me") == 0, "strdup");
    free(dup);
}

static void test_strtol(void)
{
    char *end;
    check(strtol("  -42yz", &end, 10) == -42 && *end == 'y', "strtol decimal");
    check(strtol("0x1F", 0, 0) == 31 && strtol("0755", 0, 0) == 493 &&
          strtol("101", 0, 2) == 5, "strtol bases");
    check(strtol("99999999999", 0, 10) == 2147483647L, "strtol clamps");
    check(strtol("-2147483648", 0, 10) == -2147483647L - 1, "strtol LONG_MIN");
    check(strtol("zz", &end, 10) == 0 && end[-1] != 'z', "strtol no digits");
    check(atoi("123") == 123, "atoi");
}

static void test_malloc(void)
{
    char *a = malloc(100);
    char *b = malloc(200);
    check(a && b && (((unsigned)(size_t)a | (unsigned)(size_t)b) & 7) == 0,
          "malloc alignment");
    memset(a, 0xAA, 100);
    memset(b, 0xBB, 200);
    check((unsigned char)a[99] == 0xAA && (unsigned char)b[0] == 0xBB,
          "blocks distinct");

    free(a);
    char *a2 = malloc(100);
    check(a2 == a, "freed block reused");

    memset(a2, 0x5A, 100);
    char *g = realloc(a2, 5000);
    check(g && (unsigned char)g[99] == 0x5A, "realloc preserves contents");
    check(realloc(g, 50) == g, "realloc shrinks in place");
    free(g);
    free(b);

    unsigned *z = calloc(300, 4);
    int zok = z != 0;
    for (int i = 0; zok && i < 300; i++) {
        if (z[i]) {
            zok = 0;
        }
    }
    check(zok, "calloc zeroes");
    check(!calloc(0x20000000, 16), "calloc overflow rejected");
    free(z);

    char *big = malloc(150000); // spans several sbrk growth chunks
    check(big != 0, "large malloc");
    if (big) {
        big[0] = 1;
        big[149999] = 2;
        check(big[0] == 1 && big[149999] == 2, "large malloc usable");
    }
    free(big);

    // Churn: interleaved alloc/free with content verification catches
    // corruption from bad splits or over-eager coalescing.
    char *p[64];
    int len[64];
    for (int i = 0; i < 64; i++) {
        len[i] = i * 37 % 512 + 1;
        p[i] = malloc((size_t)len[i]);
        memset(p[i], i, (size_t)len[i]);
    }
    for (int i = 0; i < 64; i += 2) {
        free(p[i]);
    }
    for (int i = 0; i < 64; i += 2) {
        len[i] = i * 53 % 256 + 1;
        p[i] = malloc((size_t)len[i]);
        memset(p[i], i ^ 0x7F, (size_t)len[i]);
    }
    int ok = 1;
    for (int i = 0; i < 64; i++) {
        int val = (i & 1) ? i : i ^ 0x7F;
        for (int j = 0; j < len[i]; j++) {
            if ((unsigned char)p[i][j] != (unsigned char)val) {
                ok = 0;
            }
        }
        free(p[i]);
    }
    check(ok, "malloc churn integrity");
}

int main(void)
{
    test_fmt();
    test_string();
    test_strtol();
    test_malloc();

    if (failed) {
        printf("libctest: %d checks failed\n", failed);
        return 1;
    }
    puts("libctest: all tests passed");
    return 0;
}
