#include <kernel.h>
#include <stdint.h>
#include <mm.h>
#include <task.h>

struct registers
{
    uint32_t ds;                  // Data segment selector
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; // Pushed by pusha.
    uint32_t int_no, err_code;    // Interrupt number and error code (if applicable)
    uint32_t eip, cs, eflags, useresp, ss; // Pushed by the processor automatically.
};


void isr_default_int(struct registers registers)
{
    //DPRINT("Unhandled interrupt: %d\n", registers.int_no);
}

void isr_reserved_int(void)
{
    panic("Unhandled reserved interrupt\n");
}

void isr_exc_DIV0(void)
{
    panic("Division by 0\n");
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

void isr_exc_OPCODE(struct registers registers)
{
    panic("Invalid opcode (eip: 0x%08x)\n", registers.eip); // Will halt
}

void isr_exc_DOUBLEF(void)
{
    panic("Double Fault\n"); // Will halt
}

void isr_exc_STACKF(void)
{
    panic("Stack Fault\n"); // Will halt
}

void isr_exc_GP(void)
{
    uint32_t eip, error;
    __asm__("   movl 60(%%ebp), %%eax   \n \
            mov %%eax, %0       \n \
        movl 56(%%ebp), %%eax   \n \
            mov %%eax, %1"
        : "=m"(eip), "=m"(error));
    kprintf("GP fault, eip:0x%08x, error:0x%08x\n",eip,error);
    panic("GP fault");
}

void isr_exc_PF(void)
{
    uint32_t faulting_addr, errorCode;
    uint32_t eip;

    asm("   movl 60(%%ebp), %%eax   \n \
                    mov %%eax, %0       \n \
                            mov %%cr2, %%eax    \n \
                                    mov %%eax, %1       \n \
                                            movl 56(%%ebp), %%eax   \n \
                                                        mov %%eax, %2"
                                                        : "=m"(eip), "=m"(faulting_addr), "=m"(errorCode));

    panic("Page fault (pid:%d, eip:0x%08x, cr2:0x%08x, error:0x%08x)\n", getpid(), eip, faulting_addr, errorCode);
}

void isr_clock_int(void)
{
}

void isr_spurious(void)
{
    kprintf("Spurious interrupt\n");
}