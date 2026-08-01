// PS/2 mouse via the i8042 aux port. IRQ12 (slave PIC) delivers one byte per
// interrupt; packets are 3 bytes: [flags][dx][dy]. The kernel integrates the
// deltas into an absolute cursor position clamped to the framebuffer size, so
// userspace (SYS_MOUSE) just polls x/y/buttons.
#include <fb.h>
#include <io.h>
#include <kernel.h>
#include <mouse.h>

MODULE("MOUS");

#define I8042_DATA   0x60
#define I8042_STATUS 0x64
#define I8042_CMD    0x64

static volatile int32_t mx = FB_WIDTH / 2;
static volatile int32_t my = FB_HEIGHT / 2;
static volatile uint32_t mbuttons;

static uint8_t cycle;
static uint8_t pkt[3];

// The controller is slow; every port access must wait for it. Bounded spins
// so a dead/absent controller can't hang boot.
static void wait_write(void)
{
    for(int t = 0; t < 100000; t++) {
        if(!(inb(I8042_STATUS) & 0x02)) {
            return;
        }
    }
}

static void wait_read(void)
{
    for(int t = 0; t < 100000; t++) {
        if(inb(I8042_STATUS) & 0x01) {
            return;
        }
    }
}

// Send a command byte to the mouse itself (0xD4 = route next byte to aux)
// and consume its ACK.
static void mouse_cmd(uint8_t cmd)
{
    wait_write();
    outb(I8042_CMD, 0xD4);
    wait_write();
    outb(I8042_DATA, cmd);
    wait_read();
    inb(I8042_DATA); // ACK (0xFA)
}

void mouse_init(void)
{
    mprintf(LOGLEVEL_DEFAULT, "Initializing PS/2 mouse\n");

    // Poll the ACK bytes ourselves; IRQ handlers must not steal them.
    asm volatile("cli");

    wait_write();
    outb(I8042_CMD, 0xA8); // enable the aux port

    // Read-modify-write the controller command byte: set bit 1 (aux IRQ12
    // enabled), clear bit 5 (aux clock enabled).
    wait_write();
    outb(I8042_CMD, 0x20);
    wait_read();
    uint8_t cfg = inb(I8042_DATA);
    cfg |= 0x02;
    cfg &= (uint8_t)~0x20;
    wait_write();
    outb(I8042_CMD, 0x60);
    wait_write();
    outb(I8042_DATA, cfg);

    mouse_cmd(0xF6); // set defaults
    mouse_cmd(0xF4); // enable data reporting

    asm volatile("sti");
}

static void process_packet(void)
{
    uint8_t flags = pkt[0];
    if(flags & 0xC0) {
        return; // overflow: deltas are garbage, drop the packet
    }

    // dx/dy are 9-bit two's complement: the sign bits live in the flags byte.
    int32_t dx = pkt[1] - ((flags << 4) & 0x100);
    int32_t dy = pkt[2] - ((flags << 3) & 0x100);

    int32_t x = mx + dx;
    int32_t y = my - dy; // PS/2 dy is positive-up; screen y grows down
    if(x < 0) x = 0;
    if(x > FB_WIDTH - 1) x = FB_WIDTH - 1;
    if(y < 0) y = 0;
    if(y > FB_HEIGHT - 1) y = FB_HEIGHT - 1;

    mx = x;
    my = y;
    mbuttons = flags & 0x07;
}

void mouse_irq(void)
{
    uint8_t status = inb(I8042_STATUS);
    if(!(status & 0x01)) {
        return; // spurious: no data
    }
    uint8_t b = inb(I8042_DATA);

    switch(cycle) {
    case 0:
        if(!(b & 0x08)) {
            return; // bit 3 is always set in a first byte: resync
        }
        pkt[0] = b;
        cycle = 1;
        break;
    case 1:
        pkt[1] = b;
        cycle = 2;
        break;
    default:
        pkt[2] = b;
        cycle = 0;
        process_packet();
        break;
    }
}

void mouse_state(int32_t *x, int32_t *y, uint32_t *buttons)
{
    asm volatile("cli");
    *x = mx;
    *y = my;
    *buttons = mbuttons;
    asm volatile("sti");
}
