#include <io.h>
#include <kernel.h>
#include <pic.h>

MODULE("PIC ");

#define ICW1_ICW4   0x01        /* ICW4 (not) needed */
#define ICW1_SINGLE 0x02        /* Single (cascade) mode */
#define ICW1_INTERVAL4  0x04        /* Call address interval 4 (8) */
#define ICW1_LEVEL  0x08        /* Level triggered (edge) mode */
#define ICW1_INIT   0x10        /* Initialization - required! */

#define ICW4_8086   0x01        /* 8086/88 (MCS-80/85) mode */
#define ICW4_AUTO   0x02        /* Auto (normal) EOI */
#define ICW4_BUF_SLAVE  0x08        /* Buffered mode/slave */
#define ICW4_BUF_MASTER 0x0C        /* Buffered mode/master */
#define ICW4_SFNM   0x10        /* Special fully nested (not) */

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21

#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1

void pic_init(void)
{
    mprintf(LOGLEVEL_DEFAULT, "Initializing PIC\n");

    // Init ICW1
    outb(PIC1_COMMAND, ICW1_INIT + ICW1_ICW4);   // Master PIC
    outb(PIC2_COMMAND, ICW1_INIT + ICW1_ICW4);   // Slave PIC


    // Init ICW2
    outb(PIC1_DATA, 0x20);  // Start vector = 32 (the 32 first are reserved to exceptions), range 0x20-0x27
    outb(PIC2_DATA, 0x70);  // Start vector = 96 (64 vectors further), range 0x70-0x77

    // Init ICW3
    outb(PIC1_DATA, 4);
    outb(PIC2_DATA, 2);

    // Init ICW4
    outb(0x21, ICW4_8086);
    outb(0xA1, ICW4_8086);

    // Mask everything except IRQ0 (timer), IRQ1 (keyboard), IRQ2 (the slave
    // cascade) and IRQ12 (PS/2 mouse). Unhandled lines stay masked: their
    // handlers wouldn't EOI properly and would wedge the PIC.
    outb(0x21, 0xF8);
    outb(0xA1, 0xEF);
}

void pic_unmask(uint8_t irq)
{
    if (irq > 15) {
        return; // not a legacy PIC line; don't wrap onto an unrelated one
    }
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq & 7;
    outb(port, inb(port) & ~(1 << bit));
}