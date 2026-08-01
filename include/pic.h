#ifndef __PIC_H__
#define __PIC_H__

#include <stdint.h>

void pic_init(void);

// Clear the mask bit for one IRQ line (0-15) so it can fire. Lines 8-15 also
// need the IRQ2 cascade, which pic_init leaves unmasked.
void pic_unmask(uint8_t irq);

#endif