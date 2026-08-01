#include <syscall.h>
#include <console.h>
#include <elf.h>
#include <fb.h>
#include <idt.h>
#include <kernel.h>
#include <keyboard.h>
#include <mouse.h>
#include <pipe.h>
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

// The file object behind an fd of the calling task, or 0 if out of range.
static struct file *fd_get(int fd)
{
    struct task *t = task_current();
    if (!t || fd < 0 || fd >= TASK_MAX_FILES) {
        return 0;
    }
    return &t->files[fd];
}

static int sys_write(int fd, const char *buf, uint32_t len)
{
    struct file *f = fd_get(fd);
    if (!f || !buf) {
        return -1;
    }
    switch (f->type) {
    case FD_CONSOLE:
        console_write(buf, len);
        return (int)len;
    case FD_PIPE_W:
        // Pipe data is program-to-program plumbing, not terminal output; it
        // reaches the serial log only if the far end eventually prints it.
        return pipe_write(f->pipe, buf, len);
    case FD_CHANNEL:
        // Output for a terminal window: into the channel's ring. Serial
        // still sees everything, which keeps logs and tests whole.
        console_out_write(f->cid, buf, len);
        for (uint32_t i = 0; i < len; i++) {
            serial_write_c(buf[i]);
        }
        return (int)len;
    default:
        return -1; // initrd files are read-only; FD_NONE is nothing at all
    }
}

// One byte from an interactive fd, or -1 if none is pending right now.
static int fd_trygetc(struct file *f)
{
    if (f->type == FD_CONSOLE) {
        char c = kbd_getc();
        return c ? (unsigned char)c : -1;
    }
    if (f->type == FD_CHANNEL) {
        return console_in_getc(f->cid);
    }
    return -1;
}

// Block until stdin has a byte, yielding so other tasks run while we wait.
// (The keyboard IRQ keeps firing because the syscall trap gate left IF set.)
static int sys_getc(void)
{
    struct file *f = fd_get(0);
    if (!f || (f->type != FD_CONSOLE && f->type != FD_CHANNEL)) {
        return -1;
    }
    for (;;) {
        int c = fd_trygetc(f);
        if (c >= 0) {
            return c;
        }
        task_switch();
    }
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
    for (int i = 3; i < TASK_MAX_FILES; i++) { // 0-2 are stdio
        if (t->files[i].type == FD_NONE) {
            t->files[i].type = FD_FILE;
            t->files[i].node = node;
            t->files[i].offset = 0;
            return i;
        }
    }
    return -1; // no free slot
}

static int sys_read(int fd, char *buf, uint32_t len)
{
    struct file *f = fd_get(fd);
    if (!f || !buf) {
        return -1;
    }
    switch (f->type) {
    case FD_CONSOLE:
    case FD_CHANNEL: {
        // Interactive: block for one byte at a time.
        if (len == 0) {
            return 0;
        }
        int c;
        while ((c = fd_trygetc(f)) < 0) {
            task_switch();
        }
        buf[0] = (char)c;
        return 1;
    }
    case FD_FILE: {
        uint32_t got = vfs_read(f->node, f->offset, len, (uint8_t *)buf);
        f->offset += got;
        return (int)got;
    }
    case FD_PIPE_R:
        return pipe_read(f->pipe, buf, len);
    default:
        return -1;
    }
}

static int sys_close(int fd)
{
    struct file *f = fd_get(fd);
    if (!f || f->type == FD_NONE) {
        return -1;
    }
    file_close(f);
    return 0;
}

// Create a pipe and install its two ends as fresh fds: fds[0] reads what
// fds[1] writes. The ends flow into children via exec2/inheritance.
static int sys_pipe(int *fds)
{
    if (!fds) {
        return -1;
    }
    struct task *t = task_current();
    int rfd = -1, wfd = -1;
    for (int i = 3; i < TASK_MAX_FILES && wfd < 0; i++) { // 0-2 are stdio
        if (t->files[i].type == FD_NONE) {
            if (rfd < 0) {
                rfd = i;
            } else {
                wfd = i;
            }
        }
    }
    if (wfd < 0) {
        return -1; // fewer than two free slots
    }
    struct pipe *p = pipe_create();
    t->files[rfd].type = FD_PIPE_R;
    t->files[rfd].pipe = p;
    t->files[wfd].type = FD_PIPE_W;
    t->files[wfd].pipe = p;
    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

// exec with an fd map: the child's fd i becomes a copy of the caller's fd
// fds[i] (-1 inherits the caller's own fd i, like plain exec). This is the
// spawn-model replacement for fork+dup2 -- the parent wires a pipeline's
// stdio without the child's cooperation. References are taken by spawn_task,
// so a failed exec leaves nothing to undo.
static int sys_exec2(const char *path, const char *const *argv, const int *fds)
{
    struct task *t = task_current();
    struct file stdio[3];
    for (int i = 0; i < 3; i++) {
        int fd = fds ? fds[i] : -1;
        if (fd < 0) {
            stdio[i] = t->files[i];
            continue;
        }
        struct file *f = fd_get(fd);
        if (!f || f->type == FD_NONE) {
            return -1;
        }
        stdio[i] = *f;
    }
    return elf_exec(path, argv, stdio);
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

static int sys_mouse(struct mouse_state *out)
{
    if (!out) {
        return -1;
    }
    mouse_state(&out->x, &out->y, &out->buttons);
    return 0;
}

// Non-blocking stdin read for GUI event loops: next byte or -1.
static int sys_pollc(void)
{
    struct file *f = fd_get(0);
    if (!f) {
        return -1;
    }
    return fd_trygetc(f);
}

// Spawn a program attached to a fresh console channel (see console.h): its
// stdio fds point at rings the caller drains/feeds, instead of the real
// screen and keyboard. Returns the console id.
static int sys_execc(const char *path, const char *const *argv)
{
    int cid = console_alloc();
    if (cid < 0) {
        return -1;
    }
    struct file stdio[3];
    for (int i = 0; i < 3; i++) {
        stdio[i].type = FD_CHANNEL;
        stdio[i].node = 0;
        stdio[i].offset = 0;
        stdio[i].cid = cid;
    }
    int pid = elf_exec(path, argv, stdio);
    if (pid < 0) {
        console_free(cid);
        return -1;
    }
    console_set_pid(cid, pid);
    return cid;
}

static int sys_font(uint8_t *buf)
{
    const uint8_t *font = fb_font_data();
    if (!buf || !font) {
        return -1;
    }
    memcpy(buf, font, FB_FONT_BYTES);
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
    case SYS_CLEAR: {
        // In a terminal window, "clear" is a form feed the terminal renders;
        // only the real console clears the VGA text screen.
        struct file *out = fd_get(1);
        if (out && out->type == FD_CHANNEL) {
            console_out_write(out->cid, "\f", 1);
        } else {
            vga_clear();
        }
        r->eax = 0;
        break;
    }
    case SYS_EXEC:
        // Children inherit the parent's stdio fds, so programs a terminal's
        // shell runs print into the same terminal window.
        r->eax = (uint32_t)elf_exec((const char *)r->ebx,
                                    (const char *const *)r->ecx,
                                    task_current()->files);
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
    case SYS_MOUSE:
        r->eax = (uint32_t)sys_mouse((struct mouse_state *)r->ebx);
        break;
    case SYS_POLLC:
        r->eax = (uint32_t)sys_pollc();
        break;
    case SYS_FONT:
        r->eax = (uint32_t)sys_font((uint8_t *)r->ebx);
        break;
    case SYS_UPTIME:
        r->eax = timer_ticks * (1000 / PIT_HZ);
        break;
    case SYS_EXECC:
        r->eax = (uint32_t)sys_execc((const char *)r->ebx,
                                     (const char *const *)r->ecx);
        break;
    case SYS_CREAD:
        r->eax = (uint32_t)console_out_read((int)r->ebx, (char *)r->ecx, r->edx);
        break;
    case SYS_CWRITE:
        r->eax = (uint32_t)console_in_write((int)r->ebx, (const char *)r->ecx, r->edx);
        break;
    case SYS_CSTAT: {
        // The attached pid while it is alive, 0 once it has died.
        int pid = console_pid((int)r->ebx);
        r->eax = (uint32_t)(task_alive(pid) ? pid : 0);
        break;
    }
    case SYS_CCLOSE:
        console_free((int)r->ebx);
        r->eax = 0;
        break;
    case SYS_KILL:
        r->eax = (uint32_t)task_kill((int)r->ebx);
        break;
    case SYS_PIPE:
        r->eax = (uint32_t)sys_pipe((int *)r->ebx);
        break;
    case SYS_EXEC2:
        r->eax = (uint32_t)sys_exec2((const char *)r->ebx,
                                     (const char *const *)r->ecx,
                                     (const int *)r->edx);
        break;
    default:
        mprintf(LOGLEVEL_DEBUG, "Unknown syscall %d\n", r->eax);
        r->eax = (uint32_t)-1;
        break;
    }
}
