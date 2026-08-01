// RTL8139 PCI ethernet driver (the QEMU `-device rtl8139` NIC).
//
// RX is a single ring buffer the chip DMAs into; the IRQ handler walks it and
// hands each frame to net_rx(), so all receive-side protocol processing runs
// in IRQ context with interrupts off. TX uses the chip's four descriptor
// slots, each backed by a driver-owned bounce buffer: callers' frames are
// copied in, so rtl8139_tx is safe from task context and from within the RX
// IRQ (protocol replies) alike. The kernel heap/BSS is identity-mapped, so
// buffer virtual addresses double as the physical addresses the chip needs.
#include <idt.h>
#include <io.h>
#include <kernel.h>
#include <mm.h>
#include <net.h>
#include <pci.h>
#include <pic.h>
#include <rtl8139.h>
#include <string.h>

MODULE("RTL8");

#define RTL_VENDOR 0x10EC
#define RTL_DEVICE 0x8139

// Register offsets from the I/O base (BAR0).
#define REG_IDR0    0x00 // MAC address, 6 bytes
#define REG_TSD0    0x10 // TX status, 4 x 4 bytes
#define REG_TSAD0   0x20 // TX buffer physical address, 4 x 4 bytes
#define REG_RBSTART 0x30 // RX ring physical address
#define REG_CR      0x37 // command
#define REG_CAPR    0x38 // RX read pointer (offset - 16)
#define REG_IMR     0x3C
#define REG_ISR     0x3E
#define REG_TCR     0x40
#define REG_RCR     0x44
#define REG_CONFIG1 0x52

#define CR_BUFE 0x01 // RX buffer empty
#define CR_TE   0x04
#define CR_RE   0x08
#define CR_RST  0x10

#define ISR_ROK   0x0001
#define ISR_RER   0x0002
#define ISR_TOK   0x0004
#define ISR_TER   0x0008
#define ISR_RXOVW 0x0010
#define ISR_FOVW  0x0040

#define TSD_OWN 0x2000 // set by the chip when the slot's DMA has completed

// RCR: unlimited DMA burst, no-wrap mode (overflowing packets run past the 8K
// ring into the slack below), accept broadcast/multicast/our-MAC.
#define RCR_CONFIG 0x0000078E

// TCR: standard interframe gap, unlimited DMA burst.
#define TCR_CONFIG 0x03000700

// 8K ring + 16 byte header + slack for one wrapped max-size frame.
#define RX_RING_SIZE 8192
#define RX_BUF_SIZE  (RX_RING_SIZE + 16 + 2048)

#define TX_SLOTS     4
#define TX_POLL_LIMIT 100000U

extern void _asm_irq_net(void);

static uint16_t io_base;
static int nic_present;
static uint8_t mac[6];
static uint8_t *rx_buf;
static uint32_t rx_off;

static uint8_t tx_bufs[TX_SLOTS][2048] __attribute__((aligned(4)));
static int tx_next;
static int tx_used[TX_SLOTS]; // slot has been handed to the chip at least once

// Same discipline as pipe.c: tx state is shared between task context and the
// RX IRQ (which transmits protocol replies), so mask interrupts around it.
static inline uint32_t irq_save(void)
{
    uint32_t flags;
    asm volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    asm volatile("push %0; popf" :: "r"(flags) : "memory", "cc");
}

int rtl8139_present(void)
{
    return nic_present;
}

const uint8_t *rtl8139_mac(void)
{
    return mac;
}

int rtl8139_tx(const void *frame, uint32_t len)
{
    if (len > 1514) {
        return -1;
    }
    uint32_t flags = irq_save();
    int slot = tx_next;
    if (tx_used[slot]) {
        // Wait for the slot's previous frame to leave for the FIFO. QEMU
        // completes this instantly; the bound only guards real hardware.
        uint32_t i;
        for (i = 0; i < TX_POLL_LIMIT; i++) {
            if (inl(io_base + REG_TSD0 + slot * 4) & TSD_OWN) {
                break;
            }
        }
        if (i == TX_POLL_LIMIT) {
            irq_restore(flags);
            return -1;
        }
    }
    memcpy(tx_bufs[slot], frame, len);
    if (len < 60) { // pad to the ethernet minimum; the chip does not
        memset(tx_bufs[slot] + len, 0, 60 - len);
        len = 60;
    }
    outl(io_base + REG_TSAD0 + slot * 4, (uint32_t)tx_bufs[slot]);
    outl(io_base + REG_TSD0 + slot * 4, len); // OWN=0: start DMA
    tx_used[slot] = 1;
    tx_next = (slot + 1) % TX_SLOTS;
    irq_restore(flags);
    return 0;
}

static void rx_drain(void)
{
    while (!(inb(io_base + REG_CR) & CR_BUFE)) {
        uint8_t *pkt = rx_buf + rx_off;
        uint16_t status = pkt[0] | (pkt[1] << 8);
        uint16_t len = pkt[2] | (pkt[3] << 8); // includes the 4-byte FCS
        if (!(status & 1) || len < 8 || len > 1518 + 4) {
            // The ring is out of sync (bad packet or driver bug). Restart
            // receive from a clean state rather than walking garbage.
            mprintf(LOGLEVEL_DEBUG, "rx ring desync (status 0x%x len %d)\n",
                    status, len);
            outb(io_base + REG_CR, CR_TE);
            outb(io_base + REG_CR, CR_TE | CR_RE);
            outl(io_base + REG_RCR, RCR_CONFIG);
            rx_off = 0;
            outw(io_base + REG_CAPR, (uint16_t)(rx_off - 16));
            return;
        }
        net_rx(pkt + 4, len - 4);
        // Packets are dword-aligned in the ring; in no-wrap mode a frame can
        // run past RX_RING_SIZE into the slack, and the offset wraps here.
        rx_off = (rx_off + 4 + len + 3) & ~3U;
        if (rx_off >= RX_RING_SIZE) {
            rx_off -= RX_RING_SIZE;
        }
        outw(io_base + REG_CAPR, (uint16_t)(rx_off - 16));
    }
}

void rtl8139_irq(void)
{
    if (!nic_present) {
        return;
    }
    uint16_t isr = inw(io_base + REG_ISR);
    outw(io_base + REG_ISR, isr); // write-1-to-clear
    if (isr & (ISR_ROK | ISR_RXOVW | ISR_FOVW)) {
        rx_drain();
    }
    // TOK/TER need no work: TX completion is observed via TSD_OWN in
    // rtl8139_tx when a slot is reused.
}

int rtl8139_init(void)
{
    uint8_t bus, dev;
    if (!pci_find_device(RTL_VENDOR, RTL_DEVICE, &bus, &dev)) {
        return -1;
    }

    // Enable I/O decoding and bus mastering (the chip is a DMA master).
    uint32_t cmd = pci_config_read(bus, dev, 0, 0x04);
    pci_config_write(bus, dev, 0, 0x04, cmd | 0x5);

    io_base = (uint16_t)(pci_config_read(bus, dev, 0, 0x10) & ~0x3U);
    uint8_t irq_line = (uint8_t)(pci_config_read(bus, dev, 0, 0x3C) & 0xFF);

    outb(io_base + REG_CONFIG1, 0x00); // power on
    outb(io_base + REG_CR, CR_RST);
    for (uint32_t i = 0; i < TX_POLL_LIMIT; i++) {
        if (!(inb(io_base + REG_CR) & CR_RST)) {
            break;
        }
    }

    for (int i = 0; i < 6; i++) {
        mac[i] = inb(io_base + REG_IDR0 + i);
    }

    rx_buf = kmalloc_a(RX_BUF_SIZE);
    rx_off = 0;
    outl(io_base + REG_RBSTART, (uint32_t)rx_buf);

    outw(io_base + REG_IMR, ISR_ROK | ISR_RER | ISR_TOK | ISR_TER |
                            ISR_RXOVW | ISR_FOVW);
    outb(io_base + REG_CR, CR_RE | CR_TE);
    outl(io_base + REG_RCR, RCR_CONFIG);
    outl(io_base + REG_TCR, TCR_CONFIG);

    // The IRQ line comes from config space; the PICs are based at 0x20/0x70.
    uint8_t vector = irq_line < 8 ? 0x20 + irq_line : 0x70 + (irq_line - 8);
    idt_install_handler(vector, (uint32_t)_asm_irq_net);
    pic_unmask(irq_line);

    nic_present = 1;
    kprintf("rtl8139: io 0x%x irq %d mac %02x:%02x:%02x:%02x:%02x:%02x\n",
            io_base, irq_line, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}
