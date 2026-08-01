#!/usr/bin/env python3
"""Canned HTTP responder for the net tests.

QEMU's slirp guestfwd runs this once per connection with the TCP stream on
stdin/stdout, so the tests never need real internet access. Reads the request
until the blank line, then answers by path and closes. The default response
size intentionally exceeds one TCP segment to exercise multi-segment receive;
the /index.html, /page2.html, and /redir routes exist for the browser tests
(two HTML pages linked to each other, plus a 302).
"""

import sys

req = b""
while b"\r\n\r\n" not in req and b"\n\n" not in req:
    chunk = sys.stdin.buffer.read1(4096)
    if not chunk:
        sys.exit(0)
    req += chunk

first = req.split(b"\r\n", 1)[0].split()
path = first[1] if len(first) > 1 else b"/"

INDEX_HTML = b"""<html><head><title>NOS Browser Test</title></head><body>
<h1>Browser test page</h1>
<p>Hello from the <b>NOS</b> test stub &amp; friends.</p>
<ul><li>first item</li><li>second item</li></ul>
<p><a href="/page2.html">go to page two</a></p>
</body></html>
"""

PAGE2_HTML = b"""<html><head><title>Page Two</title></head><body>
<h1>Page two</h1>
<p>You followed a link. <a href="index.html">back to the index</a></p>
</body></html>
"""


def respond(status, body, ctype=b"text/plain", extra=b""):
    return (b"HTTP/1.0 %s\r\n"
            b"Content-Type: %s\r\n"
            b"Content-Length: %d\r\n"
            b"%s\r\n" % (status, ctype, len(body), extra)) + body


if path == b"/index.html":
    resp = respond(b"200 OK", INDEX_HTML, b"text/html")
elif path == b"/page2.html":
    resp = respond(b"200 OK", PAGE2_HTML, b"text/html")
elif path == b"/redir":
    resp = respond(b"302 Found", b"", b"text/html",
                   b"Location: /page2.html\r\n")
else:
    body = b"NOS-NET-TEST-BODY-BEGIN\n"
    body += b"".join(b"line %04d filler filler filler filler filler filler\n" % i
                     for i in range(80))
    body += b"NOS-NET-TEST-BODY-END\n"
    resp = respond(b"200 OK", body)

sys.stdout.buffer.write(resp)
sys.stdout.buffer.flush()
