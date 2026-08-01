#!/usr/bin/env python3
"""Canned HTTP responder for the net tests.

QEMU's slirp guestfwd runs this once per connection with the TCP stream on
stdin/stdout, so the tests never need real internet access. Reads the request
until the blank line, then answers with a fixed page and closes (the response
size intentionally exceeds one TCP segment to exercise multi-segment receive).
"""

import sys

req = b""
while b"\r\n\r\n" not in req and b"\n\n" not in req:
    chunk = sys.stdin.buffer.read1(4096)
    if not chunk:
        sys.exit(0)
    req += chunk

body = b"NOS-NET-TEST-BODY-BEGIN\n"
body += b"".join(b"line %04d filler filler filler filler filler filler\n" % i
                 for i in range(80))
body += b"NOS-NET-TEST-BODY-END\n"

resp = (b"HTTP/1.0 200 OK\r\n"
        b"Content-Type: text/plain\r\n"
        b"Content-Length: %d\r\n"
        b"\r\n" % len(body)) + body

sys.stdout.buffer.write(resp)
sys.stdout.buffer.flush()
