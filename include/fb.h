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

#endif
