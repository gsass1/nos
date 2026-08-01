#ifndef __GDT_H__
#define __GDT_H__

#include <stdint.h>

struct gdt_entry
{
   uint16_t limit_low;           // The lower 16 bits of the limit.
   uint16_t base_low;            // The lower 16 bits of the base.
   uint8_t  base_middle;         // The next 8 bits of the base.
   uint8_t  access;              // Access flags, determine what ring this segment can be used in.
   uint8_t  granularity;
   uint8_t  base_high;           // The last 8 bits of the base.
} __attribute__((packed));

struct gdt_ptr
{
	uint16_t limit;				// The upper 16 bits of all selector limits.
	uint32_t base;				// The address of the first gdt_entry_t struct.
} __attribute__((packed));

// 32-bit Task State Segment. We use it for exactly one thing: telling the CPU
// which kernel stack (ss0:esp0) to switch to when an interrupt or syscall
// arrives while the CPU is in ring 3. No hardware task switching.
struct tss_entry
{
    uint32_t prev_tss;
    uint32_t esp0;   // Kernel stack pointer loaded on a ring3 -> ring0 transition.
    uint32_t ss0;    // Kernel stack segment (0x10).
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

void gdt_init(void);

// Point the TSS at the kernel stack to use for the NEXT ring3 -> ring0
// transition. The scheduler calls this on every switch to a user task.
void tss_set_esp0(uint32_t esp0);

#endif