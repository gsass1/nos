#ifndef __TCP_H__
#define __TCP_H__

#include <stdint.h>

// Client-only TCP: enough to open a connection, send a request with
// stop-and-wait reliability, and stream the response. No listen/accept, no
// congestion control, no out-of-order reassembly (dropped segments are the
// peer's to retransmit).

// Segment input from the IP layer. IRQ context (interrupts off).
void tcp_input(uint32_t src_ip, const uint8_t *seg, uint32_t len);

// Task-context socket API; FD_SOCKET files hold the returned handle.
// tcp_connect blocks through the handshake; -1 on timeout/refusal.
int tcp_connect(uint32_t ip, uint16_t port);

// Blocking send of the whole buffer; returns len, or -1 once the connection
// is dead (reset or retransmission given up on).
int tcp_send(int s, const void *buf, uint32_t len);

// Blocking receive: at least one byte unless the peer closed (0 = EOF) or
// the connection died (-1).
int tcp_recv(int s, void *buf, uint32_t len);

// Reference counting, driven by file_addref/file_close. The last release
// sends a fire-and-forget FIN; the slot lingers briefly to answer the
// peer's teardown, then is reclaimed.
void tcp_addref(int s);
void tcp_release(int s);

#endif
