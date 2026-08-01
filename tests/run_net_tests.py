#!/usr/bin/env python3
"""NOS networking integration tests.

Boots the kernel headless with an RTL8139 on QEMU user-mode networking and
drives `wget` through the shell. The HTTP endpoint is a slirp guestfwd to
tests/http_stub.py speaking a canned response on the connection's stdio, so
the whole exchange -- ARP, TCP handshake, multi-segment receive, teardown --
runs hermetically with no real network access.

Usage: python3 tests/run_net_tests.py   (from the repo root, after make && make initrd)
"""

import os
import subprocess
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_LOG = os.path.join(REPO, "tests", "serial_net.log")
KERNEL = os.path.join(REPO, "kernel.elf")
INITRD = os.path.join(REPO, "initrd", "initrd.tar")

WAIT_TIMEOUT = 30  # seconds per marker; CI runners can be slow

KEYMAP = {" ": "spc", ".": "dot", "/": "slash", "-": "minus", "|": "shift-backslash"}
for _c in "abcdefghijklmnopqrstuvwxyz0123456789":
    KEYMAP[_c] = _c
for _c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
    KEYMAP[_c] = "shift-" + _c.lower()

failures = []


class Shell:
    def __init__(self):
        stub = "python3 " + os.path.join(REPO, "tests", "http_stub.py")
        self.proc = subprocess.Popen(
            [
                "qemu-system-i386",
                "-kernel", KERNEL,
                "-initrd", INITRD,
                "-netdev", "user,id=n0,guestfwd=tcp:10.0.2.100:80-cmd:" + stub,
                "-device", "rtl8139,netdev=n0",
                "-serial", "file:" + SERIAL_LOG,
                "-display", "none",
                "-monitor", "stdio",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.pos = 0  # how much of the serial log we've already consumed

    def monitor(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()

    def serial(self):
        try:
            with open(SERIAL_LOG, "r", errors="replace") as f:
                return f.read()
        except FileNotFoundError:
            return ""

    def wait_for(self, marker, desc):
        deadline = time.time() + WAIT_TIMEOUT
        while time.time() < deadline:
            data = self.serial()
            idx = data.find(marker, self.pos)
            if idx != -1:
                self.pos = idx + len(marker)
                return True
            if self.proc.poll() is not None:
                break
            time.sleep(0.1)
        failures.append(
            f"{desc}: never saw {marker!r} (serial tail: {self.serial()[-300:]!r})")
        return False

    def type_line(self, text):
        for ch in text:
            self.monitor("sendkey " + KEYMAP[ch])
            time.sleep(0.06)
        self.monitor("sendkey ret")

    def run(self, command, markers, desc):
        self.type_line(command)
        for m in markers:
            if not self.wait_for(m, desc):
                return False
        return self.wait_for("nsh$", desc + " (prompt back)")

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()


def main():
    if not (os.path.exists(KERNEL) and os.path.exists(INITRD)):
        print("Build first: make && make initrd")
        return 2
    if os.path.exists(SERIAL_LOG):
        os.remove(SERIAL_LOG)

    sh = Shell()
    try:
        # Boot: the NIC probes and the stack comes up before the shell.
        sh.wait_for("rtl8139: io", "rtl8139 probe")
        sh.wait_for("net: ip 10.0.2.15", "net config")
        sh.wait_for("nsh$", "first prompt")

        # Argument validation before any network activity.
        sh.run("wget", ["usage: wget", "[exit status 1]"], "wget usage")

        # The full exchange against the guestfwd stub: ARP to the gateway,
        # TCP handshake, request, a >4KB response spanning several segments
        # (exercising in-order receive + window updates), clean EOF.
        sh.run(
            "wget 10.0.2.100 /",
            ["HTTP/1.0 200 OK", "NOS-NET-TEST-BODY-BEGIN",
             "line 0079", "NOS-NET-TEST-BODY-END"],
            "wget against http stub",
        )

        # A socket fd behaves like any other fd: wget's stdout into a pipe.
        sh.run(
            "wget 10.0.2.100 / | grep END",
            ["NOS-NET-TEST-BODY-END"],
            "wget through a pipeline",
        )

        # A second connection reuses socket resources (the first one's slot
        # must have been reclaimed after teardown).
        sh.run(
            "wget 10.0.2.100 /again",
            ["HTTP/1.0 200 OK", "NOS-NET-TEST-BODY-END"],
            "second connection",
        )

        if "panic:" in sh.serial():
            failures.append("kernel panicked (see serial log)")
    finally:
        sh.kill()

    if failures:
        print("FAIL")
        for f in failures:
            print("  - " + f)
        return 1
    print("PASS (rtl8139 probe, ARP, TCP handshake, HTTP GET, multi-segment "
          "receive, socket-in-pipeline, reconnect)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
