// Minimal client TCP. Design notes:
//
// - tcp_input runs in IRQ context (interrupts off) and does only bounded
//   work: update state, copy in-order data into the socket's RX ring, emit
//   an ACK. Task-side entry points touch the same state under irq_save.
// - There are no timer callbacks anywhere. Every retransmission timer is a
//   blocking task-context loop polling timer_ticks and yielding -- the same
//   pattern as sys_sleep/sys_wait. The handshake retransmits the SYN,
//   tcp_send stop-and-waits each chunk (the caller's buffer is the
//   retransmission store), and close is fire-and-forget (exit() closes fds
//   with interrupts masked, so it must not block).
// - A closed socket lingers in TS_LINGER to ACK the peer's FIN; the slot is
//   reclaimed once teardown completes, or by age when tcp_connect needs it.
#include <kernel.h>
#include <net.h>
#include <pit.h>
#include <string.h>
#include <task.h>
#include <tcp.h>

MODULE("TCP ");

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_SOCKS 4
#define TCP_RXBUF 16384
#define TCP_MSS   1460

#define TCP_RTO_TICKS   (PIT_HZ / 2) // per-try retransmit timeout
#define TCP_SEND_TRIES  8
#define TCP_SYN_TRIES   4
#define TCP_LINGER_TICKS (2 * PIT_HZ)

struct tcp_hdr
{
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t off;   // data offset in the high nibble
    uint8_t flags;
    uint16_t win, csum, urp;
} __attribute__((packed));

enum tcp_state
{
    TS_FREE = 0,
    TS_SYN_SENT,
    TS_ESTABLISHED,
    TS_LINGER, // closed locally; only answering the peer's teardown
};

struct tcp_sock
{
    int state;
    int refs;
    uint32_t rip;
    uint16_t rport, lport;
    // Sequence space (host order): snd_una..snd_nxt is in flight.
    uint32_t snd_nxt, snd_una, rcv_nxt;
    int peer_fin; // EOF once the RX ring drains
    int reset;    // RST received or retransmissions exhausted
    int fin_sent;
    uint32_t close_ticks;
    // RX ring: head is written by tcp_input (IRQ), tail by tcp_recv (task).
    uint32_t rx_head, rx_tail;
    uint8_t rx[TCP_RXBUF];
};

static struct tcp_sock socks[TCP_SOCKS];
static uint8_t segbuf[sizeof(struct tcp_hdr) + 4 + TCP_MSS];

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

// a - b in sequence space; positive when a is later.
static inline int32_t seq_diff(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

static uint16_t tcp_window(struct tcp_sock *ts)
{
    uint32_t used = (ts->rx_head - ts->rx_tail + TCP_RXBUF) % TCP_RXBUF;
    uint32_t space = TCP_RXBUF - 1 - used;
    return space > 0xFFFF ? 0xFFFF : (uint16_t)space;
}

// Assemble and send one segment. `seq` is passed explicitly so retransmits
// can replay from snd_una. with_mss adds the MSS option (SYN only).
static int tcp_output(struct tcp_sock *ts, uint8_t fl, uint32_t seq,
                      const void *data, uint32_t len, int with_mss)
{
    uint32_t flags = irq_save();
    struct tcp_hdr *th = (struct tcp_hdr *)segbuf;
    uint32_t hlen = sizeof(*th) + (with_mss ? 4 : 0);
    th->sport = htons(ts->lport);
    th->dport = htons(ts->rport);
    th->seq = htonl(seq);
    th->ack = (fl & TCP_ACK) ? htonl(ts->rcv_nxt) : 0;
    th->off = (uint8_t)((hlen / 4) << 4);
    th->flags = fl;
    th->win = htons(tcp_window(ts));
    th->csum = 0;
    th->urp = 0;
    if (with_mss) {
        segbuf[20] = 2; // kind: MSS
        segbuf[21] = 4;
        segbuf[22] = TCP_MSS >> 8;
        segbuf[23] = TCP_MSS & 0xFF;
    }
    if (len) {
        memcpy(segbuf + hlen, data, len);
    }
    uint32_t tlen = hlen + len;
    uint8_t ph[12]; // pseudo-header for the checksum
    memcpy(ph, &net_ip, 4);
    memcpy(ph + 4, &ts->rip, 4);
    ph[8] = 0;
    ph[9] = 6; // IPPROTO_TCP
    ph[10] = (uint8_t)(tlen >> 8);
    ph[11] = (uint8_t)tlen;
    uint32_t sum = net_csum_add(0, ph, sizeof(ph));
    sum = net_csum_add(sum, segbuf, tlen);
    th->csum = net_csum_fin(sum);
    int r = net_ip_tx(ts->rip, 6, segbuf, tlen);
    irq_restore(flags);
    return r;
}

static uint32_t ring_put(struct tcp_sock *ts, const uint8_t *p, uint32_t n)
{
    uint32_t put = 0;
    while (put < n) {
        uint32_t next = (ts->rx_head + 1) % TCP_RXBUF;
        if (next == ts->rx_tail) {
            break; // full: the unaccepted rest is the peer's to retransmit
        }
        ts->rx[ts->rx_head] = p[put++];
        ts->rx_head = next;
    }
    return put;
}

void tcp_input(uint32_t src_ip, const uint8_t *seg, uint32_t len)
{
    if (len < sizeof(struct tcp_hdr)) {
        return;
    }
    const struct tcp_hdr *th = (const struct tcp_hdr *)seg;
    uint32_t doff = (uint32_t)(th->off >> 4) * 4;
    if (doff < sizeof(*th) || doff > len) {
        return;
    }
    const uint8_t *payload = seg + doff;
    uint32_t plen = len - doff;

    struct tcp_sock *ts = 0;
    for (int i = 0; i < TCP_SOCKS; i++) {
        if (socks[i].state != TS_FREE && socks[i].rip == src_ip &&
            htons(socks[i].rport) == th->sport &&
            htons(socks[i].lport) == th->dport) {
            ts = &socks[i];
            break;
        }
    }
    if (!ts) {
        return; // no listeners in this stack; strays are dropped
    }

    uint32_t seq = ntohl(th->seq);
    uint32_t ack = ntohl(th->ack);

    if (th->flags & TCP_RST) {
        ts->reset = 1;
        if (ts->state == TS_LINGER) {
            ts->state = TS_FREE; // nobody left to observe the reset
        }
        return;
    }

    if (ts->state == TS_SYN_SENT) {
        if ((th->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
            ack == ts->snd_nxt) {
            ts->rcv_nxt = seq + 1;
            ts->snd_una = ack;
            ts->state = TS_ESTABLISHED;
            tcp_output(ts, TCP_ACK, ts->snd_nxt, 0, 0, 0);
        }
        return;
    }

    if (th->flags & TCP_ACK) {
        if (seq_diff(ack, ts->snd_una) > 0 && seq_diff(ack, ts->snd_nxt) <= 0) {
            ts->snd_una = ack;
        }
    }

    // In-order data goes into the ring; anything else (including zero-window
    // probes and old retransmits) just provokes a fresh ACK.
    int need_ack = 0;
    if (plen > 0) {
        if (seq == ts->rcv_nxt) {
            ts->rcv_nxt += ring_put(ts, payload, plen);
        }
        need_ack = 1;
    }
    // A FIN counts once every byte before it is consumed; a retransmitted
    // FIN (seq just below rcv_nxt) falls through to the re-ACK.
    if (th->flags & TCP_FIN) {
        if (!ts->peer_fin && seq + plen == ts->rcv_nxt) {
            ts->rcv_nxt += 1;
            ts->peer_fin = 1;
        }
        need_ack = 1;
    }
    if (need_ack) {
        tcp_output(ts, TCP_ACK, ts->snd_nxt, 0, 0, 0);
    }

    // A lingering socket is done once our FIN is acked and the peer's FIN
    // has arrived: full teardown, slot reusable.
    if (ts->state == TS_LINGER && ts->peer_fin &&
        seq_diff(ts->snd_una, ts->snd_nxt) >= 0) {
        ts->state = TS_FREE;
    }
}

static struct tcp_sock *sock_get(int s)
{
    if (s < 0 || s >= TCP_SOCKS || socks[s].state == TS_FREE) {
        return 0;
    }
    return &socks[s];
}

int tcp_connect(uint32_t ip, uint16_t port)
{
    static uint16_t next_port;

    if (net_ensure_gateway() < 0) {
        return -1;
    }

    uint32_t flags = irq_save();
    int idx = -1;
    for (int i = 0; i < TCP_SOCKS; i++) {
        // Reclaim lingers whose peer never finished tearing down.
        if (socks[i].state == TS_LINGER &&
            timer_ticks - socks[i].close_ticks > TCP_LINGER_TICKS) {
            socks[i].state = TS_FREE;
        }
        if (socks[i].state == TS_FREE && idx < 0) {
            idx = i;
        }
    }
    if (idx < 0) {
        irq_restore(flags);
        return -1;
    }
    struct tcp_sock *ts = &socks[idx];
    uint32_t iss = (timer_ticks << 8) | ((uint32_t)idx << 4);
    // Zero the control fields; the RX ring's contents don't matter (its
    // indices were just cleared) and skipping 16KB of memset is worth it.
    memset(ts, 0, (uint32_t)((uint8_t *)ts->rx - (uint8_t *)ts));
    ts->state = TS_SYN_SENT;
    ts->refs = 1;
    ts->rip = ip;
    ts->rport = port;
    ts->lport = (uint16_t)(0xD000 | (next_port++ & 0xFFF));
    ts->snd_una = iss;
    ts->snd_nxt = iss + 1; // the SYN consumes one sequence number
    irq_restore(flags);

    for (int attempt = 0; attempt < TCP_SYN_TRIES; attempt++) {
        tcp_output(ts, TCP_SYN, iss, 0, 0, 1);
        uint32_t start = timer_ticks;
        while (ts->state == TS_SYN_SENT && !ts->reset &&
               timer_ticks - start < TCP_RTO_TICKS * (uint32_t)(attempt + 1)) {
            task_switch();
        }
        if (ts->state == TS_ESTABLISHED) {
            return idx;
        }
        if (ts->reset) {
            break;
        }
    }
    uint32_t f2 = irq_save();
    ts->state = TS_FREE;
    irq_restore(f2);
    return -1;
}

int tcp_send(int s, const void *buf, uint32_t len)
{
    struct tcp_sock *ts = sock_get(s);
    if (!ts || ts->state != TS_ESTABLISHED || ts->reset || ts->fin_sent) {
        return -1;
    }
    const uint8_t *p = buf;
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > TCP_MSS) {
            chunk = TCP_MSS;
        }
        uint32_t flags = irq_save();
        uint32_t seq = ts->snd_nxt;
        ts->snd_nxt += chunk;
        irq_restore(flags);

        int acked = 0;
        for (int attempt = 0; attempt < TCP_SEND_TRIES && !acked; attempt++) {
            if (tcp_output(ts, TCP_ACK | TCP_PSH, seq, p + sent, chunk, 0) < 0) {
                break;
            }
            uint32_t start = timer_ticks;
            while (!ts->reset && timer_ticks - start < TCP_RTO_TICKS) {
                if (seq_diff(ts->snd_una, seq + chunk) >= 0) {
                    acked = 1;
                    break;
                }
                task_switch();
            }
            if (ts->reset) {
                break;
            }
        }
        if (!acked) {
            ts->reset = 1; // undeliverable: poison the connection
            return -1;
        }
        sent += chunk;
    }
    return (int)sent;
}

int tcp_recv(int s, void *buf, uint32_t len)
{
    struct tcp_sock *ts = sock_get(s);
    if (!ts) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    uint8_t *p = buf;
    for (;;) {
        uint32_t flags = irq_save();
        uint32_t got = 0;
        while (got < len && ts->rx_tail != ts->rx_head) {
            p[got++] = ts->rx[ts->rx_tail];
            ts->rx_tail = (ts->rx_tail + 1) % TCP_RXBUF;
        }
        int fin = ts->peer_fin, rst = ts->reset;
        irq_restore(flags);
        if (got > 0) {
            // The drain opened receive window; a peer stalled at zero window
            // only learns that from an ACK, so always send one.
            tcp_output(ts, TCP_ACK, ts->snd_nxt, 0, 0, 0);
            return (int)got;
        }
        if (fin) {
            return 0; // clean EOF
        }
        if (rst || ts->state != TS_ESTABLISHED) {
            return -1;
        }
        task_switch();
    }
}

void tcp_addref(int s)
{
    struct tcp_sock *ts = sock_get(s);
    if (ts) {
        uint32_t flags = irq_save();
        ts->refs++;
        irq_restore(flags);
    }
}

// Last reference gone: send one FIN and stop caring. This runs from exit()
// with interrupts already masked, so it must not block; TS_LINGER +
// tcp_input handle the rest of the teardown opportunistically.
void tcp_release(int s)
{
    struct tcp_sock *ts = sock_get(s);
    if (!ts) {
        return;
    }
    uint32_t flags = irq_save();
    if (--ts->refs > 0) {
        irq_restore(flags);
        return;
    }
    if (ts->state == TS_ESTABLISHED && !ts->reset && !ts->fin_sent) {
        uint32_t seq = ts->snd_nxt;
        ts->snd_nxt += 1; // the FIN consumes one sequence number
        ts->fin_sent = 1;
        ts->state = TS_LINGER;
        ts->close_ticks = timer_ticks;
        tcp_output(ts, TCP_FIN | TCP_ACK, seq, 0, 0, 0);
    } else {
        ts->state = TS_FREE;
    }
    irq_restore(flags);
}
