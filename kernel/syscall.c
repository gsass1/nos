#include <syscall.h>
#include <elf.h>
#include <idt.h>
#include <kernel.h>
#include <keyboard.h>
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
// longer in the ready queue.
static int sys_wait(int pid)
{
    while (task_alive(pid)) {
        task_switch();
    }
    return 0;
}

void syscall_dispatch(struct regs *r)
{
    switch (r->eax) {
    case SYS_EXIT:
        exit(); // never returns
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
        r->eax = (uint32_t)elf_exec((const char *)r->ebx);
        break;
    case SYS_WAIT:
        r->eax = (uint32_t)sys_wait((int)r->ebx);
        break;
    default:
        mprintf(LOGLEVEL_DEBUG, "Unknown syscall %d\n", r->eax);
        r->eax = (uint32_t)-1;
        break;
    }
}
