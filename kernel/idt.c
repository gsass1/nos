#include <idt.h>
#include <kernel.h>
#include <string.h>

MODULE("IDT ");

extern  void _asm_default_int(void);
extern  void _asm_reserved_int(void);
extern  void _asm_exc_DIV0(void);
extern  void _asm_exc_DEBUG(void);
extern  void _asm_exc_BP(void);
extern  void _asm_exc_NOMATH(void);
extern  void _asm_exc_MF(void);
extern  void _asm_exc_TSS(void);
extern  void _asm_exc_SWAP(void);
extern  void _asm_exc_AC(void);
extern  void _asm_exc_MC(void);
extern  void _asm_exc_XM(void);
extern  void _asm_exc_NMI(void);
extern  void _asm_exc_OVRFLW(void);
extern  void _asm_exc_BOUNDS(void);
extern  void _asm_exc_OPCODE(void);
extern  void _asm_exc_DOUBLEF(void);
extern  void _asm_exc_STACKF(void);
extern  void _asm_exc_GP(void);
extern  void _asm_exc_PF(void);
extern  void _asm_irq_0(void);
extern  void _asm_irq_1(void);
extern  void _asm_irq_7(void);

struct idt_entry idt_entries[256];
struct idt_ptr idt_ptr;

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
{
    idt_entries[num].base_lo = base & 0xFFFF;
    idt_entries[num].base_hi = (base >> 16) & 0xFFFF;

    idt_entries[num].sel     = sel;
    idt_entries[num].always0 = 0;
    // We must uncomment the OR below when we get to using user-mode.
    // It sets the interrupt gate's privilege level to 3.
    idt_entries[num].flags   = flags /* | 0x60 */;
}

static void set_default_handlers(void)
{
    int i;

    for(i = 0; i < 32; i++) {
        idt_set_gate(i, (uint32_t)_asm_reserved_int, 0x8, 0x8E);
    }

    for(i = 32; i < 256; i++) {
        idt_set_gate(i, (uint32_t)_asm_default_int, 0x8, 0x8E);      
    }
}

void idt_init(void)
{
    mprintf(LOGLEVEL_DEFAULT, "Initializing IDT\n");

    idt_ptr.limit = sizeof(struct idt_entry) * 256 -1;
    idt_ptr.base  = (uint32_t)&idt_entries;

    memset(&idt_entries, 0, sizeof(struct idt_entry) * 256);

    set_default_handlers();

    idt_set_gate(0, (uint32_t) _asm_exc_DIV0, 0x8, 0x8E); // #DivisionError
    idt_set_gate(1, (uint32_t) _asm_exc_DEBUG, 0x8, 0x8E); // #Debug
    idt_set_gate(2, (uint32_t) _asm_exc_NMI, 0x8, 0x8E); // #NMI
    idt_set_gate(3, (uint32_t) _asm_exc_BP, 0x8, 0x8E); // #Breakpoint
    idt_set_gate(4, (uint32_t) _asm_exc_OVRFLW, 0x8, 0x8E); // #OVERFLOW
    idt_set_gate(5, (uint32_t) _asm_exc_BOUNDS, 0x8, 0x8E); // #Bounds
    idt_set_gate(6, (uint32_t) _asm_exc_OPCODE, 0x8, 0x8E); // #Invalid Opcode
    idt_set_gate(7, (uint32_t) _asm_exc_NOMATH, 0x8, 0x8E); // #No Math Coprocessor
    idt_set_gate(8, (uint32_t) _asm_exc_DOUBLEF, 0x8, 0x8E); // #Double Fault
    idt_set_gate(9, (uint32_t) _asm_exc_MF, 0x8, 0x8E); // #CoProcessor Segment Overrun (not used after the i386)
    idt_set_gate(10, (uint32_t) _asm_exc_TSS, 0x8, 0x8E); // #Invalid TSS
    idt_set_gate(11, (uint32_t) _asm_exc_SWAP, 0x8, 0x8E); // #Segment not present in memory (used for SWAP-ing ram)
    idt_set_gate(12, (uint32_t) _asm_exc_STACKF, 0x8, 0x8E); // #Stack Fault (stack operation and ss load)
    idt_set_gate(13, (uint32_t) _asm_exc_GP, 0x8, 0x8E);    // #Global Protection Fault
    idt_set_gate(14, (uint32_t) _asm_exc_PF, 0x8, 0x8E); // #Page Fault
    idt_set_gate(15, (uint32_t) _asm_exc_MF, 0x8, 0x8E); // #Math fault (floating point)
    idt_set_gate(16, (uint32_t) _asm_exc_AC, 0x8, 0x8E); // #Alignment check
    idt_set_gate(17, (uint32_t) _asm_exc_MC, 0x8, 0x8E); // #Machine check
    idt_set_gate(18, (uint32_t) _asm_exc_XM, 0x8, 0x8E); // #SIMD floating-point exception

    idt_set_gate(32, (uint32_t) _asm_irq_0, 0x08, 0x8E);
    idt_set_gate(33, (uint32_t) _asm_irq_1, 0x08, 0x8E);
    idt_set_gate(40, (uint32_t) _asm_irq_7, 0x08, 0x8E);

    asm volatile("lidtl %0": "=m" (idt_ptr));
}

void idt_install_handler(uint8_t num, uint32_t base)
{
    idt_set_gate(num, base, 0x8, 0x8E);
}