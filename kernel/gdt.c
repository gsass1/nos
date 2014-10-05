#include <gdt.h>
#include <kernel.h>

MODULE("GDT ");

static struct gdt_entry gdt_entries[5];
static struct gdt_ptr gdt_ptr;

static void gdt_set_gate(sint32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt_entries[num].base_low    = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high   = (base >> 24) & 0xFF;

    gdt_entries[num].limit_low   = (limit & 0xFFFF);
    gdt_entries[num].granularity = (limit >> 16) & 0x0F;

    gdt_entries[num].granularity |= gran & 0xF0;
    gdt_entries[num].access      = access;
}

void gdt_init(void)
{
    mprintf(LOGLEVEL_DEFAULT, "Initializing GDT\n");

    gdt_ptr.limit = (sizeof(struct gdt_entry) * 5) - 1;
    gdt_ptr.base  = (uint32_t)&gdt_entries;

    gdt_set_gate(0, 0, 0, 0, 0);                // Null segment
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // Code segment
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Data segment
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // User mode code segment
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User mode data segment

    __asm__("lgdtl %0": "=m" (gdt_ptr):);
    __asm__("movw $0x10, %ax");
    __asm__("movw %dx, %ax");
    __asm__("movw %es, %ax");
    __asm__("movw %fs, %ax");
    __asm__("movw %gs, %ax");
    __asm__("movw %ss, %ax");
    __asm__("ljmp $0x08, $_flush    \n \
            _flush:                 \n");
}