#ifndef __RTL8139_H__
#define __RTL8139_H__

#include <stdint.h>

// Probe PCI for an RTL8139, bring it up and hook its IRQ. Returns 0 when a
// NIC was found, -1 otherwise (the rest of the net stack then stays inert).
int rtl8139_init(void);

// Nonzero once rtl8139_init succeeded.
int rtl8139_present(void);

// Our MAC address (valid after a successful init).
const uint8_t *rtl8139_mac(void);

// Queue one ethernet frame (max 1514 bytes). Safe from both task and IRQ
// context; the frame is copied into a driver-owned slot before DMA. Returns
// 0, or -1 if the frame is oversized or all TX slots are wedged.
int rtl8139_tx(const void *frame, uint32_t len);

// IRQ handler, called from _asm_irq_net. Drains the RX ring and feeds each
// good frame to net_rx().
void rtl8139_irq(void);

#endif
