#ifndef __NET_H__
#define __NET_H__

#include <stdint.h>

// IPv4 addresses are passed around in wire (network byte) order: the uint32_t
// IP4(10,0,2,15) laid down in little-endian memory is byte-for-byte what goes
// on the wire, so addresses can be memcpy'd and compared directly.
#define IP4(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                         ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static inline uint16_t htons(uint16_t v) { return (v >> 8) | (v << 8); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v)
{
    return ((v & 0x000000FF) << 24) | ((v & 0x0000FF00) << 8) |
           ((v & 0x00FF0000) >> 8) | (v >> 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

// The static slirp (QEMU user-mode networking) topology. Everything off-host
// routes through the gateway, so the ARP layer only ever resolves one MAC.
extern const uint32_t net_ip;      // us:       10.0.2.15
extern const uint32_t net_gateway; // slirp:    10.0.2.2
extern const uint32_t net_dns;     // resolver: 10.0.2.3

// Probe for a NIC and bring the stack up. Safe to call with no NIC: the
// stack just stays down and every entry point fails cleanly.
void net_init(void);

// Nonzero once a NIC was found.
int net_up(void);

// Entry point for received ethernet frames; called by the driver from IRQ
// context (interrupts off), so everything downstream of it runs atomically.
void net_rx(const uint8_t *frame, uint32_t len);

// Send one IPv4 packet carrying `seg` (a transport-layer segment, max 1480
// bytes). Callable from task and IRQ context. Fails (-1) until the gateway
// MAC is known -- callers on the task side ensure that first.
int net_ip_tx(uint32_t dst_ip, uint8_t proto, const void *seg, uint32_t len);

// Blocking (task context only): ARP the gateway until it answers. Everything
// TX-side needs this to have succeeded once.
int net_ensure_gateway(void);

// Ones'-complement checksum, split so pseudo-headers can be folded in:
// accumulate with net_csum_add (len may be odd only on the final call),
// finish with net_csum_fin.
uint32_t net_csum_add(uint32_t sum, const void *data, uint32_t len);
uint16_t net_csum_fin(uint32_t sum);

// Blocking DNS A lookup through the slirp resolver (task context only).
// On success writes the address (wire order) to *ip_out and returns 0.
int dns_resolve(const char *name, uint32_t *ip_out);

#endif
