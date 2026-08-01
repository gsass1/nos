#ifndef __MOUSE_H__
#define __MOUSE_H__

#include <stdint.h>

// Initialize the i8042 aux (PS/2 mouse) port and enable streaming. Call after
// kbd_init; IRQ12 must be unmasked (slave PIC) for packets to arrive.
void mouse_init(void);

// IRQ12 handler: assembles 3-byte packets into the tracked cursor state.
void mouse_irq(void);

// Snapshot the current state: position clamped to the framebuffer resolution,
// buttons bit0=left bit1=right bit2=middle.
void mouse_state(int32_t *x, int32_t *y, uint32_t *buttons);

#endif
