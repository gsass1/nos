#!/usr/bin/env python3
"""NOS boot/integration tests.

Boots the kernel headless in QEMU, types shell commands through the monitor
(sendkey), and asserts on the serial log. Instead of fixed sleeps it polls the
log for expected markers, so it tolerates slow CI runners. At the end it dumps
VGA text memory through the monitor and asserts the prompt is actually on the
visible screen -- serial output alone cannot catch display regressions (a VGA
wrap bug once made the console look frozen while serial showed a live shell).

Usage: python3 tests/run_tests.py   (from the repo root, after make && make initrd)
"""

import os
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_LOG = os.path.join(REPO, "tests", "serial.log")
KERNEL = os.path.join(REPO, "kernel.elf")
INITRD = os.path.join(REPO, "initrd", "initrd.tar")

WAIT_TIMEOUT = 30  # seconds per marker; CI runners can be slow

# QEMU sendkey names for the characters the tests type.
KEYMAP = {" ": "spc"}
for _c in "abcdefghijklmnopqrstuvwxyz0123456789":
    KEYMAP[_c] = _c

failures = []


class Shell:
    def __init__(self):
        self.proc = subprocess.Popen(
            [
                "qemu-system-i386",
                "-kernel", KERNEL,
                "-initrd", INITRD,
                "-serial", "file:" + SERIAL_LOG,
                "-display", "none",
                "-monitor", "stdio",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
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
        """Block until `marker` appears in serial output we haven't consumed yet."""
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
        failures.append(f"{desc}: never saw {marker!r} (serial tail: {self.serial()[-300:]!r})")
        return False

    def type_line(self, text):
        for ch in text:
            self.monitor("sendkey " + KEYMAP[ch])
            time.sleep(0.06)
        self.monitor("sendkey ret")

    def run(self, command, markers, desc):
        """Type a command, wait for each marker, then wait for the next prompt."""
        self.type_line(command)
        for m in markers:
            if not self.wait_for(m, desc):
                return False
        return self.wait_for("nsh$", desc + " (prompt back)")

    def screen_rows(self):
        """Dump VGA text memory via the monitor; returns the 25 visible rows."""
        self.monitor("xp /4000bx 0xb8000")
        time.sleep(1.0)
        self.monitor("quit")
        out, _ = self.proc.communicate(timeout=15)
        cells = []
        for line in out.splitlines():
            if ": 0x" not in line:
                continue
            cells += [int(tok, 16) for tok in re.findall(r"0x[0-9a-f]{2}", line)]
        chars = cells[0::2]  # even bytes are characters, odd are attributes
        text = "".join(chr(c) if 32 <= c < 127 else " " for c in chars)
        return [text[i : i + 80] for i in range(0, 2000, 80)]

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()


def main():
    if not (os.path.exists(KERNEL) and os.path.exists(INITRD)):
        print("Build first: make && make initrd", file=sys.stderr)
        return 2
    if os.path.exists(SERIAL_LOG):
        os.remove(SERIAL_LOG)

    sh = Shell()
    try:
        # Boot: kernel comes up and the ring-3 shell prints its banner + prompt.
        sh.wait_for("Welcome to NOS!", "boot")
        sh.wait_for("nsh - NOS userspace shell", "shell banner")
        sh.wait_for("nsh$", "first prompt")

        # readdir: all initrd files are listed.
        sh.run("ls", ["symtable", "hello", "crash", "cat"], "ls")

        # exec + exit + wait round trip.
        sh.run("hello", ["Hello from a loaded ELF program"], "hello")

        # argv + open/read loop + sbrk: cat streams a >4KB file; check content
        # from both the first and a later read() chunk.
        sh.run("cat symtable", ["kernel_base", "kernel_end"], "cat symtable")

        # Error paths report distinct messages and nonzero exit statuses.
        sh.run("cat nope", ["not found", "[exit status 1]"], "cat missing file")
        sh.run("cat", ["usage: cat", "[exit status 1]"], "cat usage")
        sh.run("nosuchthing", ["unknown command"], "unknown command")

        # Fault isolation: crash dies to a ring-3 page fault (error bit 2 set =
        # user-mode access) and the shell survives to run another command.
        sh.run(
            "crash",
            ["killed: page fault", "error 0x00000007", "[exit status -1]"],
            "crash isolation",
        )
        sh.run("hello", ["Hello from a loaded ELF program"], "shell alive after crash")

        # The kernel never panicked.
        if "panic:" in sh.serial():
            failures.append("kernel panicked (see serial log)")

        # The prompt must be on the VISIBLE screen, not off in VGA memory.
        rows = sh.screen_rows()
        if not any("nsh$" in row for row in rows):
            failures.append("prompt not on the visible VGA screen (rows 0-24)")
    finally:
        sh.kill()

    if failures:
        print("FAIL")
        for f in failures:
            print("  - " + f)
        return 1
    print("PASS (boot, ls, exec/wait, argv, file io, sbrk, error paths, "
          "fault isolation, vga screen)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
