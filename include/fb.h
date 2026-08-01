#ifndef __FB_H__
#define __FB_H__

#include <stdint.h>

// Fixed mode for now; the Bochs/QEMU VBE (BGA) interface could do others.
#define FB_WIDTH  1024
#define FB_HEIGHT 768
#define FB_BPP    32
#define FB_PITCH  (FB_WIDTH * (FB_BPP / 8))
#define FB_SIZE   (FB_PITCH * FB_HEIGHT)

// Probe PCI for the QEMU std VGA device and its framebuffer BAR.
void fb_init(void);

// Non-zero once fb_init found a framebuffer device.
int fb_present(void);

// Physical address of the linear framebuffer (valid if fb_present()).
uint32_t fb_phys_addr(void);

// Switch the display into / out of the 1024x768x32 linear-framebuffer mode.
// Disabling returns to VGA text mode with the text contents intact.
int fb_enable(void);
void fb_disable(void);

// The saved BIOS text font (256 glyphs x 32 bytes, 8x16 in rows 0-15), or 0
// if no display was found. FB_FONT_BYTES is the SYS_FONT copy size.
#define FB_FONT_BYTES (256 * 32)
const uint8_t *fb_font_data(void);

#endif
