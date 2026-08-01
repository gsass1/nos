// Minimal IPv4 stack over the RTL8139: ethernet demux, just-enough ARP (the
// slirp gateway is the only next hop), IPv4 send/receive without options or
// fragments, and the UDP client side of a DNS resolver.
//
// Context rules: net_rx and everything it calls run in IRQ context with
// interrupts off. Task-side senders share the TX frame buffer and ARP/DNS
// state with that IRQ path, so they mask interrupts around every touch (the
// pipe.c discipline). Blocking waits (ARP, DNS) live in task-context loops
// that poll under the PIT, the same pattern sys_sleep/sys_wait use.
#include <kernel.h>
#include <net.h>
#include <pit.h>
#include <rtl8139.h>
#include <string.h>
#include <task.h>
#include <tcp.h>

MODULE("NET ");

#define ETH_TYPE_IP  0x0800
#define ETH_TYPE_ARP 0x0806

#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17

struct eth_hdr
{
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type;
} __attribute__((packed));

struct arp_pkt
{
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op; // 1 = request, 2 = reply
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} __attribute__((packed));

struct ip_hdr
{
    uint8_t vhl, tos;
    uint16_t len, id, frag;
    uint8_t ttl, proto;
    uint16_t csum;
    uint32_t src, dst;
} __attribute__((packed));

struct udp_hdr
{
    uint16_t sport, dport, len, csum;
} __attribute__((packed));

const uint32_t net_ip      = IP4(10, 0, 2, 15);
const uint32_t net_gateway = IP4(10, 0, 2, 2);
const uint32_t net_dns     = IP4(10, 0, 2, 3);

static const uint8_t eth_bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint8_t gw_mac[6];
static volatile int gw_valid;

// Shared frame assembly buffer. Only ever touched with interrupts masked.
static uint8_t txframe[1514];

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

int net_up(void)
{
    return rtl8139_present();
}

uint32_t net_csum_add(uint32_t sum, const void *data, uint32_t len)
{
    const uint8_t *p = data;
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint32_t)p[0] << 8;
    }
    return sum;
}

uint16_t net_csum_fin(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return htons((uint16_t)~sum);
}

// ---------------------------------------------------------------- ethernet

static void eth_tx(const uint8_t *dst_mac, uint16_t type,
                   const void *payload, uint32_t len)
{
    if (len > 1500) {
        return;
    }
    uint32_t flags = irq_save();
    struct eth_hdr *eth = (struct eth_hdr *)txframe;
    memcpy(eth->dst, (void *)dst_mac, 6);
    memcpy(eth->src, (void *)rtl8139_mac(), 6);
    eth->type = htons(type);
    memcpy(txframe + sizeof(*eth), (void *)payload, len);
    rtl8139_tx(txframe, sizeof(*eth) + len);
    irq_restore(flags);
}

// --------------------------------------------------------------------- ARP

static void arp_send(uint16_t op, const uint8_t *dst_mac, uint32_t target_ip)
{
    struct arp_pkt arp;
    arp.htype = htons(1); // ethernet
    arp.ptype = htons(ETH_TYPE_IP);
    arp.hlen = 6;
    arp.plen = 4;
    arp.op = htons(op);
    memcpy(arp.sha, (void *)rtl8139_mac(), 6);
    arp.spa = net_ip;
    memcpy(arp.tha, (void *)dst_mac, 6);
    arp.tpa = target_ip;
    eth_tx(op == 1 ? eth_bcast : dst_mac, ETH_TYPE_ARP, &arp, sizeof(arp));
}

static void arp_input(const uint8_t *pkt, uint32_t len)
{
    if (len < sizeof(struct arp_pkt)) {
        return;
    }
    const struct arp_pkt *arp = (const struct arp_pkt *)pkt;
    if (arp->htype != htons(1) || arp->ptype != htons(ETH_TYPE_IP)) {
        return;
    }
    if (arp->op == htons(2) && arp->spa == net_gateway) {
        memcpy(gw_mac, (void *)arp->sha, 6);
        gw_valid = 1;
    } else if (arp->op == htons(1) && arp->tpa == net_ip) {
        // Slirp ARPs for us before delivering; answer or inbound data stalls.
        arp_send(2, arp->sha, arp->spa);
    }
}

int net_ensure_gateway(void)
{
    if (!rtl8139_present()) {
        return -1;
    }
    for (int attempt = 0; attempt < 4 && !gw_valid; attempt++) {
        arp_send(1, eth_bcast, net_gateway);
        uint32_t start = timer_ticks;
        while (!gw_valid && timer_ticks - start < PIT_HZ / 2) {
            task_switch();
        }
    }
    return gw_valid ? 0 : -1;
}

// -------------------------------------------------------------------- IPv4

int net_ip_tx(uint32_t dst_ip, uint8_t proto, const void *seg, uint32_t len)
{
    static uint16_t ip_id;
    if (!rtl8139_present() || !gw_valid || len > 1480) {
        return -1;
    }
    uint32_t flags = irq_save();
    struct eth_hdr *eth = (struct eth_hdr *)txframe;
    struct ip_hdr *ip = (struct ip_hdr *)(txframe + sizeof(*eth));
    memcpy(eth->dst, gw_mac, 6);
    memcpy(eth->src, (void *)rtl8139_mac(), 6);
    eth->type = htons(ETH_TYPE_IP);
    ip->vhl = 0x45;
    ip->tos = 0;
    ip->len = htons(sizeof(*ip) + len);
    ip->id = htons(ip_id++);
    ip->frag = htons(0x4000); // DF
    ip->ttl = 64;
    ip->proto = proto;
    ip->csum = 0;
    ip->src = net_ip;
    ip->dst = dst_ip;
    ip->csum = net_csum_fin(net_csum_add(0, ip, sizeof(*ip)));
    memcpy((uint8_t *)ip + sizeof(*ip), (void *)seg, len);
    int r = rtl8139_tx(txframe, sizeof(*eth) + sizeof(*ip) + len);
    irq_restore(flags);
    return r;
}

// --------------------------------------------------------------- DNS / UDP

// One resolve in flight at a time; concurrent callers queue on `busy`.
static struct {
    volatile int busy;
    volatile int done;
    uint16_t id;
    uint16_t port;
    uint32_t addr;
} dnsq;

// Walk a (possibly compression-terminated) DNS name; returns the offset just
// past it within this record, or 0 on malformed input.
static uint32_t dns_skip_name(const uint8_t *p, uint32_t len, uint32_t off)
{
    while (off < len) {
        uint8_t l = p[off];
        if (l == 0) {
            return off + 1;
        }
        if ((l & 0xC0) == 0xC0) { // compression pointer ends the name
            return off + 2;
        }
        off += 1 + l;
    }
    return 0;
}

static void dns_input(const uint8_t *p, uint32_t len)
{
    if (len < 12 || !dnsq.busy || dnsq.done) {
        return;
    }
    uint16_t id = ((uint16_t)p[0] << 8) | p[1];
    uint16_t flags = ((uint16_t)p[2] << 8) | p[3];
    uint16_t qdcount = ((uint16_t)p[4] << 8) | p[5];
    uint16_t ancount = ((uint16_t)p[6] << 8) | p[7];
    if (id != dnsq.id || !(flags & 0x8000)) {
        return;
    }
    uint32_t off = 12;
    for (uint16_t i = 0; i < qdcount; i++) {
        off = dns_skip_name(p, len, off);
        if (!off || off + 4 > len) {
            return;
        }
        off += 4; // qtype + qclass
    }
    for (uint16_t i = 0; i < ancount; i++) {
        off = dns_skip_name(p, len, off);
        if (!off || off + 10 > len) {
            return;
        }
        uint16_t type = ((uint16_t)p[off] << 8) | p[off + 1];
        uint16_t rdlen = ((uint16_t)p[off + 8] << 8) | p[off + 9];
        off += 10;
        if (off + rdlen > len) {
            return;
        }
        if (type == 1 && rdlen == 4) { // A record
            memcpy(&dnsq.addr, p + off, 4);
            dnsq.done = 1;
            return;
        }
        off += rdlen;
    }
}

static void udp_input(const uint8_t *seg, uint32_t len)
{
    if (len < sizeof(struct udp_hdr)) {
        return;
    }
    const struct udp_hdr *udp = (const struct udp_hdr *)seg;
    uint32_t dlen = ntohs(udp->len);
    if (dlen < sizeof(*udp) || dlen > len) {
        return;
    }
    if (udp->sport == htons(53) && udp->dport == htons(dnsq.port)) {
        dns_input(seg + sizeof(*udp), dlen - sizeof(*udp));
    }
}

// Build and send one DNS A query for `name` from `sport`. UDP checksum 0
// (legitimately "not computed" in IPv4; slirp accepts it).
static int dns_send_query(const char *name, uint16_t id, uint16_t sport)
{
    uint8_t pkt[sizeof(struct udp_hdr) + 12 + 130 + 4];
    struct udp_hdr *udp = (struct udp_hdr *)pkt;
    uint8_t *q = pkt + sizeof(*udp);
    uint32_t nlen = strlen(name);
    if (nlen == 0 || nlen > 128) {
        return -1;
    }
    q[0] = id >> 8;
    q[1] = id & 0xFF;
    q[2] = 0x01; // RD
    q[3] = 0x00;
    q[4] = 0x00; q[5] = 0x01; // one question
    q[6] = q[7] = q[8] = q[9] = q[10] = q[11] = 0;
    uint32_t off = 12;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= nlen; i++) {
        if (name[i] == '.' || name[i] == '\0') {
            uint32_t label = i - start;
            if (label == 0 || label > 63) {
                return -1;
            }
            q[off++] = (uint8_t)label;
            memcpy(q + off, (void *)(name + start), label);
            off += label;
            start = i + 1;
        }
    }
    q[off++] = 0;             // root label
    q[off++] = 0; q[off++] = 1; // type A
    q[off++] = 0; q[off++] = 1; // class IN
    uint32_t total = sizeof(*udp) + off;
    udp->sport = htons(sport);
    udp->dport = htons(53);
    udp->len = htons((uint16_t)total);
    udp->csum = 0;
    return net_ip_tx(net_dns, IP_PROTO_UDP, pkt, total);
}

int dns_resolve(const char *name, uint32_t *ip_out)
{
    static uint16_t next_id = 1;

    if (net_ensure_gateway() < 0) {
        return -1;
    }

    // Claim the single query slot.
    for (;;) {
        uint32_t flags = irq_save();
        int free = !dnsq.busy;
        if (free) {
            dnsq.busy = 1;
            dnsq.done = 0;
            dnsq.id = next_id++;
            dnsq.port = (uint16_t)(0xC000 | (dnsq.id & 0xFFF));
        }
        irq_restore(flags);
        if (free) {
            break;
        }
        task_switch();
    }

    int found = 0;
    for (int attempt = 0; attempt < 3 && !found; attempt++) {
        if (dns_send_query(name, dnsq.id, dnsq.port) < 0) {
            break;
        }
        uint32_t start = timer_ticks;
        while (!dnsq.done && timer_ticks - start < PIT_HZ) {
            task_switch();
        }
        found = dnsq.done;
    }
    if (found) {
        *ip_out = dnsq.addr;
    }
    dnsq.busy = 0;
    return found ? 0 : -1;
}

// ------------------------------------------------------------------- input

static void ip_input(const uint8_t *pkt, uint32_t len)
{
    if (len < sizeof(struct ip_hdr)) {
        return;
    }
    const struct ip_hdr *ip = (const struct ip_hdr *)pkt;
    if (ip->vhl != 0x45 || ip->dst != net_ip) {
        return; // no options, no fragments, must be for us
    }
    if (ip->frag & htons(0x3FFF)) {
        return; // fragment (offset or MF set): dropped, peers must not frag
    }
    uint32_t total = ntohs(ip->len);
    if (total < sizeof(*ip) || total > len) {
        return; // shorter-than-header or truncated on the wire
    }
    const uint8_t *seg = pkt + sizeof(*ip);
    uint32_t seglen = total - sizeof(*ip);
    if (ip->proto == IP_PROTO_TCP) {
        tcp_input(ip->src, seg, seglen);
    } else if (ip->proto == IP_PROTO_UDP) {
        udp_input(seg, seglen);
    }
}

void net_rx(const uint8_t *frame, uint32_t len)
{
    if (len < sizeof(struct eth_hdr)) {
        return;
    }
    const struct eth_hdr *eth = (const struct eth_hdr *)frame;
    const uint8_t *payload = frame + sizeof(*eth);
    uint32_t plen = len - sizeof(*eth);
    if (eth->type == htons(ETH_TYPE_ARP)) {
        arp_input(payload, plen);
    } else if (eth->type == htons(ETH_TYPE_IP)) {
        ip_input(payload, plen);
    }
}

void net_init(void)
{
    if (rtl8139_init() < 0) {
        mprintf(LOGLEVEL_DEFAULT, "No ethernet device found\n");
        return;
    }
    kprintf("net: ip 10.0.2.15 gw 10.0.2.2 dns 10.0.2.3 (slirp)\n");
}
