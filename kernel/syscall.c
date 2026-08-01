#include <syscall.h>
#include <elf.h>
#include <fb.h>
#include <idt.h>
#include <kernel.h>
#include <keyboard.h>
#include <pit.h>
#include <serial.h>
#include <string.h>
#include <task.h>
#include <vfs.h>
#include <vga.h>

MODULE("SYSC");

extern void _asm_syscall(void);

void syscall_init(void)
{
    mprintf(LOGLEVEL_DEFAULT, "Initializing syscalls (int 0x80)\n");

    // DPL3 32-bit trap gate: reachable from ring 3 and leaves the interrupt
    // flag untouched, so IRQs (keyboard, timer) keep firing during a syscall.
    idt_set_gate_user(0x80, (uint32_t)_asm_syscall, 0x08, 0xEF);
}

static void console_write(const char *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        vga_putc(buf[i]);
        serial_write_c(buf[i]);
    }
}

static int sys_write(int fd, const char *buf, uint32_t len)
{
    (void)fd; // only the console exists for now
    if (!buf) {
        return -1;
    }
    console_write(buf, len);
    return (int)len;
}

// Block until a key is available, yielding so other tasks run while we wait.
// The keyboard IRQ fills kbd_getc's buffer; it works here because the trap gate
// left interrupts enabled.
static int sys_getc(void)
{
    char c;
    while ((c = kbd_getc()) == 0) {
        task_switch();
    }
    return (unsigned char)c;
}

static int sys_readdir(uint32_t index, char *name_out, uint32_t len)
{
    if (!name_out || len == 0) {
        return -1;
    }
    struct dirent *node = vfs_readdir(fs_root, index);
    if (!node) {
        return -1;
    }
    // Never write past the caller's buffer; truncate and always NUL-terminate.
    strncpy(name_out, node->name, len - 1);
    name_out[len - 1] = '\0';
    return 0;
}

// Block (yielding to other tasks) until the given task has exited, i.e. is no
// longer in the ready queue. Returns its exit status.
static int sys_wait(int pid)
{
    while (task_alive(pid)) {
        task_switch();
    }
    return task_exit_code(pid);
}

static int sys_open(const char *path)
{
    if (!path) {
        return -1;
    }
    struct fs_node *node = vfs_finddir(fs_root, (char *)path);
    if (!node) {
        return -1;
    }
    struct task *t = task_current();
    for (int i = 0; i < TASK_MAX_FILES; i++) {
        if (!t->files[i].node) {
            t->files[i].node = node;
            t->files[i].offset = 0;
            return i + 3; // fds 0-2 are the console
        }
    }
    return -1; // no free slot
}

static int sys_read(int fd, char *buf, uint32_t len)
{
    if (!buf) {
        return -1;
    }
    if (fd == 0) {
        // stdin: one blocking keypress at a time.
        if (len == 0) {
            return 0;
        }
        buf[0] = (char)sys_getc();
        return 1;
    }
    struct task *t = task_current();
    int slot = fd - 3;
    if (slot < 0 || slot >= TASK_MAX_FILES || !t->files[slot].node) {
        return -1;
    }
    uint32_t got = vfs_read(t->files[slot].node, t->files[slot].offset, len,
                            (uint8_t *)buf);
    t->files[slot].offset += got;
    return (int)got;
}

static int sys_close(int fd)
{
    struct task *t = task_current();
    int slot = fd - 3;
    if (slot < 0 || slot >= TASK_MAX_FILES || !t->files[slot].node) {
        return -1;
    }
    t->files[slot].node = 0;
    return 0;
}

// Grow (never shrink) the user heap. Returns the previous break, so
// sbrk(incr) hands the caller [old, old+incr) as fresh zeroed memory.
static int sys_sbrk(int incr)
{
    struct task *t = task_current();
    if (!t->brk) {
        return -1; // kernel thread: no user heap
    }
    uint32_t old = t->brk;
    if (incr <= 0) {
        return (int)old;
    }
    uint32_t new_brk = old + (uint32_t)incr;
    if (new_brk < old || new_brk > USER_HEAP_MAX) {
        return -1;
    }
    // Map any new pages. The task's own directory is the active one, so the
    // pages are usable (and zeroable) immediately.
    for (uint32_t a = old & ~0xFFFU; a < new_brk; a += 0x1000) {
        struct page *pg = get_page(a, 1, t->page_directory);
        if (!pg->frame) {
            alloc_frame(pg, 0 /* user */, 1 /* writable */);
            memset((void *)a, 0, 0x1000);
        }
    }
    t->brk = new_brk;
    return (int)old;
}

static int sys_fbinfo(struct fb_info *out)
{
    if (!out || !fb_present()) {
        return -1;
    }
    out->width = FB_WIDTH;
    out->height = FB_HEIGHT;
    out->pitch = FB_PITCH;
    out->bpp = FB_BPP;
    return 0;
}

// Switch to graphics mode and map the framebuffer into the calling process at
// USER_FB_BASE. The pages are MMIO, not RAM: PTEs are written directly and
// free_frame knows to skip the allocator bitmap for them on process exit.
static int sys_fbmap(void)
{
    struct task *t = task_current();
    if (!fb_present() || !t->brk /* kernel thread */) {
        return -1;
    }
    if (fb_enable() < 0) {
        return -1;
    }
    uint32_t phys = fb_phys_addr();
    for (uint32_t off = 0; off < FB_SIZE; off += 0x1000) {
        struct page *pg = get_page(USER_FB_BASE + off, 1, t->page_directory);
        pg->present = 1;
        pg->rw = 1;
        pg->user = 1;
        pg->frame = (phys + off) >> 12;
    }
    return (int)USER_FB_BASE;
}

static int sys_fboff(void)
{
    fb_disable();
    // The text plane shares VRAM with the framebuffer, so graphics drawing
    // trashed whatever text was on screen. Reset to a clean console.
    vga_clear();
    return 0;
}

// Block for at least ms milliseconds, yielding the CPU while waiting.
static int sys_sleep(uint32_t ms)
{
    uint32_t ticks = (ms * PIT_HZ + 999) / 1000;
    uint32_t start = timer_ticks;
    while (timer_ticks - start < ticks) {
        task_switch();
    }
    return 0;
}

void syscall_dispatch(struct regs *r)
{
    switch (r->eax) {
    case SYS_EXIT:
        exit((int)r->ebx); // never returns
        break;
    case SYS_WRITE:
        r->eax = (uint32_t)sys_write((int)r->ebx, (const char *)r->ecx, r->edx);
        break;
    case SYS_GETC:
        r->eax = (uint32_t)sys_getc();
        break;
    case SYS_READDIR:
        r->eax = (uint32_t)sys_readdir(r->ebx, (char *)r->ecx, r->edx);
        break;
    case SYS_CLEAR:
        vga_clear();
        r->eax = 0;
        break;
    case SYS_EXEC:
        r->eax = (uint32_t)elf_exec((const char *)r->ebx,
                                    (const char *const *)r->ecx);
        break;
    case SYS_WAIT:
        r->eax = (uint32_t)sys_wait((int)r->ebx);
        break;
    case SYS_OPEN:
        r->eax = (uint32_t)sys_open((const char *)r->ebx);
        break;
    case SYS_READ:
        r->eax = (uint32_t)sys_read((int)r->ebx, (char *)r->ecx, r->edx);
        break;
    case SYS_CLOSE:
        r->eax = (uint32_t)sys_close((int)r->ebx);
        break;
    case SYS_SBRK:
        r->eax = (uint32_t)sys_sbrk((int)r->ebx);
        break;
    case SYS_FBINFO:
        r->eax = (uint32_t)sys_fbinfo((struct fb_info *)r->ebx);
        break;
    case SYS_FBMAP:
        r->eax = (uint32_t)sys_fbmap();
        break;
    case SYS_FBOFF:
        r->eax = (uint32_t)sys_fboff();
        break;
    case SYS_SLEEP:
        r->eax = (uint32_t)sys_sleep(r->ebx);
        break;
    default:
        mprintf(LOGLEVEL_DEBUG, "Unknown syscall %d\n", r->eax);
        r->eax = (uint32_t)-1;
        break;
    }
}
