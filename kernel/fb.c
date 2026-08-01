// Bochs/QEMU "BGA" display driver: a linear framebuffer programmed through
// two I/O ports. This is QEMU's std VGA (PCI 1234:1111); real hardware would
// need a VBE/GOP path instead.
#include <fb.h>
#include <io.h>
#include <kernel.h>
#include <pci.h>

MODULE("FB  ");

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID     0
#define VBE_DISPI_INDEX_XRES   1
#define VBE_DISPI_INDEX_YRES   2
#define VBE_DISPI_INDEX_BPP    3
#define VBE_DISPI_INDEX_ENABLE 4

#define VBE_DISPI_DISABLED     0x00
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_LFB_ENABLED  0x40

static int present;
static uint32_t phys;

// The BGA framebuffer and VGA text mode share the same VRAM: drawing pixels
// destroys the text-mode font (plane 2) that SeaBIOS loaded at boot, and
// disabling BGA does not reprogram the VGA into text mode. So we save the
// font once at init, and on fb_disable() do a full mode-3 register set plus
// a font restore.
static uint8_t saved_font[256 * 32];
static int font_saved;

// Standard VGA 80x25 text mode (mode 3) register values.
static const uint8_t text_misc = 0x67;
static const uint8_t text_seq[5] = { 0x03, 0x00, 0x03, 0x00, 0x02 };
static const uint8_t text_crtc[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E,
    0x00, 0x00, 0x00, 0x50, 0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF,
};
static const uint8_t text_gc[9] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF,
};
static const uint8_t text_ac[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B,
    0x3C, 0x3D, 0x3E, 0x3F, 0x0C, 0x00, 0x0F, 0x08, 0x00,
};

static uint16_t bga_read(uint16_t index)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static void bga_write(uint16_t index, uint16_t value)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

// Point host writes/reads at font plane 2, mapped linearly at 0xA0000.
static void vga_font_access_begin(void)
{
    outw(0x3C4, 0x0402); // sequencer: write to plane 2 only
    outw(0x3C4, 0x0604); // sequencer: sequential (not odd/even) addressing
    outw(0x3CE, 0x0204); // graphics: read from plane 2
    outw(0x3CE, 0x0005); // graphics: read/write mode 0
    outw(0x3CE, 0x0406); // graphics: map VRAM at 0xA0000 (64KB), graphics mode
}

// Back to normal text-mode plane 0/1 odd/even addressing at 0xB8000.
static void vga_font_access_end(void)
{
    outw(0x3C4, 0x0302); // sequencer: write planes 0+1
    outw(0x3C4, 0x0204); // sequencer: odd/even addressing
    outw(0x3CE, 0x0004); // graphics: read plane 0
    outw(0x3CE, 0x1005); // graphics: odd/even read/write
    outw(0x3CE, 0x0E06); // graphics: map at 0xB8000 (32KB), text mode
}

static void vga_save_font(void)
{
    vga_font_access_begin();
    const uint8_t *vram = (const uint8_t *)0xA0000;
    for (uint32_t i = 0; i < sizeof(saved_font); i++) {
        saved_font[i] = vram[i];
    }
    vga_font_access_end();
    font_saved = 1;
}

static void vga_restore_text_mode(void)
{
    outb(0x3C2, text_misc);

    for (uint8_t i = 0; i < 5; i++) {
        outb(0x3C4, i);
        outb(0x3C5, text_seq[i]);
    }

    // Unlock CRTC registers 0-7, then write the full set.
    outb(0x3D4, 0x11);
    outb(0x3D5, text_crtc[0x11] & 0x7F);
    for (uint8_t i = 0; i < 25; i++) {
        outb(0x3D4, i);
        outb(0x3D5, text_crtc[i]);
    }

    for (uint8_t i = 0; i < 9; i++) {
        outb(0x3CE, i);
        outb(0x3CF, text_gc[i]);
    }

    // Attribute controller: reading 0x3DA resets its index/data flip-flop.
    inb(0x3DA);
    for (uint8_t i = 0; i < 21; i++) {
        outb(0x3C0, i);
        outb(0x3C0, text_ac[i]);
    }
    outb(0x3C0, 0x20); // re-enable video output

    if (font_saved) {
        vga_font_access_begin();
        uint8_t *vram = (uint8_t *)0xA0000;
        for (uint32_t i = 0; i < sizeof(saved_font); i++) {
            vram[i] = saved_font[i];
        }
        vga_font_access_end();
    }
}

void fb_init(void)
{
    uint8_t bus, dev;
    if (!pci_find_device(0x1234, 0x1111, &bus, &dev)) {
        mprintf(LOGLEVEL_DEFAULT, "No BGA display device found\n");
        return;
    }

    uint16_t id = bga_read(VBE_DISPI_INDEX_ID);
    if ((id & 0xFFF0) != 0xB0C0) {
        mprintf(LOGLEVEL_DEFAULT, "PCI 1234:1111 present but BGA id 0x%08x is unknown\n", id);
        return;
    }

    // BAR0 is the linear framebuffer (memory BAR: mask the low flag bits).
    phys = pci_config_read(bus, dev, 0, 0x10) & 0xFFFFFFF0U;
    present = 1;

    // VRAM is pristine right now: stash the BIOS-loaded text font so we can
    // put it back after graphics drawing tramples plane 2.
    vga_save_font();

    mprintf(LOGLEVEL_DEFAULT, "BGA display at PCI %d:%d, framebuffer at 0x%08x\n",
            bus, dev, phys);
}

int fb_present(void)
{
    return present;
}

uint32_t fb_phys_addr(void)
{
    return phys;
}

int fb_enable(void)
{
    if (!present) {
        return -1;
    }
    // The mode must be programmed while disabled, then enabled with LFB on.
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write(VBE_DISPI_INDEX_XRES, FB_WIDTH);
    bga_write(VBE_DISPI_INDEX_YRES, FB_HEIGHT);
    bga_write(VBE_DISPI_INDEX_BPP, FB_BPP);
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
    return 0;
}

void fb_disable(void)
{
    if (!present) {
        return;
    }
    // Keep the register dance atomic: a concurrent console print while the
    // VRAM window is remapped for the font copy would scribble on plane data.
    asm volatile("cli");
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    vga_restore_text_mode();
    asm volatile("sti");
}
