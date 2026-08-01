#include <idt.h>
#include <kernel.h>
#include <stdint.h>
#include <mm.h>
#include <task.h>

void isr_default_int(void)
{
}

void isr_reserved_int(void)
{
    panic("Unhandled reserved interrupt\n");
}

// A fault raised while the CPU was in ring 3 is the program's fault, not the
// kernel's: report it and kill just that task. Ring 0 faults are kernel bugs
// and still panic. exit() never returns; the trap frame simply dies with the
// task's kernel stack when the reaper frees it.
static void fault_kill_user(struct fault_frame *f, const char *what)
{
    kprintf("%s (pid %d) killed: %s at eip 0x%08x\n",
            getpname(), getpid(), what, f->eip);
    exit();
}

void isr_exc_DIV0(struct fault_frame *f)
{
    if (f->cs & 3) {
        fault_kill_user(f, "division by zero");
    }
    panic("Division by 0 (eip: 0x%08x)\n", f->eip);
}

void isr_exc_DEBUG(void)
{
}

void isr_exc_BP(void)
{
    panic("Breakpoint interrupt received\n");
}

void isr_exc_NOMATH(void)
{
    panic("Math coprocessor not available\n");
}

void isr_exc_MF(void)
{
    panic("Coprocessor segment overrun\n");
}

void isr_exc_TSS(void)
{
    panic("Invalid TSS\n");
}

void isr_exc_SWAP(void)
{
    panic("Segment not present in memory, but SWAP is not implemented\n");
}

void isr_exc_AC(void)
{
    panic("Aligment check exception\n");
}

void isr_exc_MC(void)
{
    panic("Machine check exception\n");
}

void isr_exc_XM(void)
{
    panic("SIMD Floating-Point Exception\n");
}

void isr_exc_NMI(void)
{
    panic("Non-maskable interrupt received\n");
}

void isr_exc_OVRFLW(void)
{
    panic("Overflow interrupt\n");
}

void isr_exc_BOUNDS(void)
{
    panic("Bound interrupt\n"); // Will halt
}

void isr_exc_OPCODE(struct fault_frame *f)
{
    if (f->cs & 3) {
        fault_kill_user(f, "invalid opcode");
    }
    panic("Invalid opcode (eip: 0x%08x)\n", f->eip); // Will halt
}

void isr_exc_DOUBLEF(void)
{
    panic("Double Fault\n"); // Will halt
}

void isr_exc_STACKF(void)
{
    panic("Stack Fault\n"); // Will halt
}

void isr_exc_GP(struct fault_frame *f)
{
    if (f->cs & 3) {
        fault_kill_user(f, "general protection fault");
    }
    panic("GP fault (eip:0x%08x, error:0x%08x)\n", f->eip, f->err);
}

void isr_exc_PF(struct fault_frame *f)
{
    uint32_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));

    if (f->cs & 3) {
        kprintf("%s (pid %d) killed: page fault at eip 0x%08x (addr 0x%08x, error 0x%08x)\n",
                getpname(), getpid(), f->eip, cr2, f->err);
        exit();
    }
    panic("Page fault (pid:%d, eip:0x%08x, cr2:0x%08x, error:0x%08x)\n",
          getpid(), f->eip, cr2, f->err);
}

void isr_clock_int(void)
{
}

void isr_spurious(void)
{
    kprintf("Spurious interrupt\n");
}