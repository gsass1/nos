#!/usr/bin/env python3
"""Looping TLS HTTP responder for the net tests.

Usage: https_stub.py <port> <certfile> <keyfile> <body-marker>

Listens on 127.0.0.1:<port> -- which the guest reaches as 10.0.2.2:<port>
through slirp's host-loopback mapping -- and answers every connection with a
canned HTTP response containing <body-marker>. Runs until killed by the test
harness. Handshake failures (e.g. the guest aborting on an untrusted
certificate, which one test deliberately provokes) are ignored.
"""

import socket
import ssl
import sys

port = int(sys.argv[1])
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(sys.argv[2], sys.argv[3])
marker = sys.argv[4].encode()

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(5)
print("ready", flush=True)

while True:
    conn, _ = srv.accept()
    try:
        conn.settimeout(20)
        tls = ctx.wrap_socket(conn, server_side=True)
        req = b""
        while b"\r\n\r\n" not in req:
            chunk = tls.recv(4096)
            if not chunk:
                break
            req += chunk
        body = marker + b"\n"
        tls.sendall(b"HTTP/1.0 200 OK\r\n"
                    b"Content-Type: text/plain\r\n"
                    b"Content-Length: %d\r\n\r\n" % len(body) + body)
        tls.unwrap()  # send close_notify so the client sees a clean TLS EOF
    except (ssl.SSLError, OSError):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass
