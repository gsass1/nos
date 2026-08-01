// disktest -- exercise the ext2 filesystem from userspace.
//
// With no args: lists /disk, reads /disk/hello.txt, lists /disk/subdir,
// reads /disk/subdir/README, creates and writes /disk/written.txt, then
// reads it back -- all through the int 0x80 syscall ABI.
//
// With "verify": lists /disk, reads /disk/hello.txt and /disk/written.txt
// (proving the guest-written file persists across a reboot).
#include "ulib.h"

#define BUFSZ 512

static int do_listdir(const char *path)
{
    char name[128];
    int i = 0;
    while (listdir(path, i, name) == 0) {
        put("  ");
        put(name);
        put("\n");
        i++;
    }
    return i;
}

static int cat_file(const char *path, char *buf)
{
    int fd = open(path);
    if (fd < 0) return -1;
    int n;
    while ((n = read(fd, buf, BUFSZ)) > 0) {
        write(1, buf, n);
    }
    close(fd);
    return n; // 0 at EOF, -1 on error
}

void _start(int argc, char **argv)
{
    char *buf = sbrk(BUFSZ);
    if (buf == (char *)-1) {
        put("disktest: out of memory\n");
        exit(1);
    }

    // --- Listing /disk ---
    put("disktest: listing /disk\n");
    int n = do_listdir("disk");
    put("disktest: ");
    puti(n);
    put(" entries\n");

    // --- Read a host-created file ---
    put("disktest: reading /disk/hello.txt\n");
    if (cat_file("disk/hello.txt", buf) < 0) {
        put("disktest: FAIL cannot read hello.txt\n");
        exit(1);
    }
    put("\ndisktest: read ok\n");

    // --- Nested listing and read ---
    put("disktest: listing /disk/subdir\n");
    int n2 = do_listdir("disk/subdir");
    put("disktest: subdir ");
    puti(n2);
    put(" entries\n");

    put("disktest: reading /disk/subdir/README\n");
    if (cat_file("disk/subdir/README", buf) < 0) {
        put("disktest: FAIL cannot read README\n");
        exit(1);
    }
    put("\ndisktest: nested read ok\n");

    if (argc >= 2 && streq(argv[1], "verify")) {
        // --- Read back the guest-written file after reboot ---
        put("disktest: reading /disk/written.txt\n");
        if (cat_file("disk/written.txt", buf) < 0) {
            put("disktest: FAIL cannot read written.txt\n");
            exit(1);
        }
        put("\ndisktest: verify ok\n");
        put("disktest: PASS\n");
        exit(0);
    }

    // --- Create and write a new file ---
    put("disktest: creating /disk/written.txt\n");
    int fd = openmode("disk/written.txt", O_CREATE | O_TRUNC | O_WRONLY);
    if (fd < 0) {
        put("disktest: FAIL cannot create written.txt\n");
        exit(1);
    }
    const char *msg = "NOS ext2 persistent guest write";
    int w = write(fd, msg, slen(msg));
    if (w != slen(msg)) {
        put("disktest: FAIL short write\n");
        close(fd);
        exit(1);
    }
    // --- Access-mode regression: reading from a write-only fd must fail ---
    int wfd = openmode("disk/written.txt", O_WRONLY);
    if (wfd < 0) {
        put("disktest: FAIL cannot reopen written.txt\n");
        exit(1);
    }
    int r = read(wfd, buf, BUFSZ);
    if (r >= 0) {
        put("disktest: FAIL read on write-only fd succeeded\n");
        close(wfd);
        exit(1);
    }
    close(wfd);
    put("disktest: write-only read rejected ok\n");

    close(fd);
    put("disktest: write ok\n");

    // --- Read it back immediately ---
    put("disktest: reading back /disk/written.txt\n");
    if (cat_file("disk/written.txt", buf) < 0) {
        put("disktest: FAIL cannot read back\n");
        exit(1);
    }
    put("\ndisktest: readback ok\n");

    put("disktest: PASS\n");
    exit(0);
}
