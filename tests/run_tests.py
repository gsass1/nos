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
for _c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
    KEYMAP[_c] = "shift-" + _c.lower()

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


def read_ppm(path):
    """Parse a binary (P6) PPM into (width, height, pixel_bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    tokens = []
    i = 0
    while len(tokens) < 4:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        start = i
        while i < len(data) and not data[i : i + 1].isspace():
            i += 1
        tokens.append(data[start:i])
    i += 1  # single whitespace after maxval
    w, h = int(tokens[1]), int(tokens[2])
    return w, h, data[i:]


def check_screendump(path):
    """fbtest draws red/green/blue/white quadrants; sample each center."""
    try:
        w, h, px = read_ppm(path)
    except (OSError, ValueError, IndexError) as e:
        failures.append(f"screendump unreadable: {e}")
        return
    if (w, h) != (1024, 768):
        failures.append(f"screendump is {w}x{h}, expected 1024x768 (graphics mode not active?)")
        return
    expected = {
        (w // 4, h // 4): (255, 0, 0),
        (3 * w // 4, h // 4): (0, 255, 0),
        (w // 4, 3 * h // 4): (0, 0, 255),
        (3 * w // 4, 3 * h // 4): (255, 255, 255),
    }
    for (x, y), rgb in expected.items():
        off = (y * w + x) * 3
        got = tuple(px[off : off + 3])
        if got != rgb:
            failures.append(f"screendump pixel ({x},{y}) = {got}, expected {rgb}")


def mouse_reset(sh):
    """Drive the cursor into the top-left corner: position clamping turns
    repeated large negative moves into an absolute (0,0)."""
    for _ in range(8):
        sh.monitor("mouse_move -300 -300")
        time.sleep(0.1)


def mouse_step(sh, dx, dy):
    """Move in <=120px chunks so PS/2 9-bit deltas never clamp."""
    while dx or dy:
        sx = max(-120, min(120, dx))
        sy = max(-120, min(120, dy))
        sh.monitor(f"mouse_move {sx} {sy}")
        dx -= sx
        dy -= sy
        time.sleep(0.15)


def sample(path, checks, desc):
    """checks: list of (x, y, (r,g,b), what)."""
    try:
        w, h, px = read_ppm(path)
    except (OSError, ValueError, IndexError) as e:
        failures.append(f"{desc}: screendump unreadable: {e}")
        return
    if (w, h) != (1024, 768):
        failures.append(f"{desc}: screendump is {w}x{h}, expected 1024x768")
        return
    for x, y, rgb, what in checks:
        off = (y * w + x) * 3
        got = tuple(px[off : off + 3])
        if got != rgb:
            failures.append(f"{desc}: ({x},{y}) [{what}] = {got}, expected {rgb}")


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

        # Shift handling in the keyboard driver: typed capitals echo back.
        sh.run("Hello World", ["unknown command: Hello"], "shift typing")

        # Mouse: inject movement and a left click through the monitor while
        # mtest polls SYS_MOUSE; it reports each state change.
        sh.type_line("mtest")
        if sh.wait_for("mtest: start", "mtest start"):
            time.sleep(0.3)
            sh.monitor("mouse_move 100 50")
            time.sleep(0.3)
            sh.monitor("mouse_button 1")
            time.sleep(0.3)
            sh.monitor("mouse_button 0")
            sh.wait_for("mtest: x=", "mouse movement reported")
            sh.wait_for("b=1", "mouse left button reported")
            sh.wait_for("mtest: done", "mtest done")
            sh.wait_for("nsh$", "prompt after mtest")

        # Fault isolation: crash dies to a ring-3 page fault (error bit 2 set =
        # user-mode access) and the shell survives to run another command.
        sh.run(
            "crash",
            ["killed: page fault", "error 0x00000007", "[exit status -1]"],
            "crash isolation",
        )
        sh.run("hello", ["Hello from a loaded ELF program"], "shell alive after crash")

        # Framebuffer: fbtest switches to 1024x768x32 graphics, draws four
        # colored quadrants and holds them; screendump the emulated display
        # and check actual pixel colors, then confirm text mode comes back.
        ppm = os.path.join(REPO, "tests", "screen.ppm")
        if os.path.exists(ppm):
            os.remove(ppm)
        sh.type_line("fbtest")
        if sh.wait_for("fbtest: pattern drawn", "fbtest draw"):
            sh.monitor("screendump " + ppm)
            deadline = time.time() + 10
            while time.time() < deadline and not (
                os.path.exists(ppm) and os.path.getsize(ppm) > 0
            ):
                time.sleep(0.2)
            check_screendump(ppm)
            sh.wait_for("fbtest: done", "fbtest back to text mode")
            sh.wait_for("nsh$", "prompt after fbtest")

        # Window manager: desktop + taskbar render, and dragging a window by
        # its title bar moves it. Initial layout: win1 (140,120,400x300)
        # unfocused (gray title), win2 (420,260,360x240) focused (navy title).
        sh.type_line("wm")
        if sh.wait_for("wm: started", "wm start"):
            time.sleep(1.0)
            wm1 = os.path.join(REPO, "tests", "wm1.ppm")
            for f in (wm1,):
                if os.path.exists(f):
                    os.remove(f)
            sh.monitor("screendump " + wm1)
            time.sleep(1.5)
            sample(wm1, [
                (8, 8, (0, 128, 128), "desktop"),
                (512, 760, (192, 192, 192), "taskbar"),
                (340, 132, (128, 128, 128), "win1 title (unfocused)"),
                (340, 200, (255, 255, 255), "win1 body"),
                (600, 272, (0, 0, 128), "win2 title (focused)"),
            ], "wm initial")

            # Drag win2's title bar (grab at 600,272) by +120,+60.
            mouse_reset(sh)
            mouse_step(sh, 600, 272)
            sh.monitor("mouse_button 1")
            time.sleep(0.4)
            mouse_step(sh, 120, 60)
            sh.monitor("mouse_button 0")
            time.sleep(0.8)
            wm2 = os.path.join(REPO, "tests", "wm2.ppm")
            if os.path.exists(wm2):
                os.remove(wm2)
            sh.monitor("screendump " + wm2)
            time.sleep(1.5)
            sample(wm2, [
                (700, 330, (0, 0, 128), "win2 title after drag"),
                (600, 272, (0, 128, 128), "desktop where win2 was"),
            ], "wm after drag")

            sh.monitor("sendkey q")
            sh.wait_for("wm: exit", "wm exits to shell")
            sh.wait_for("nsh$", "prompt after wm")

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
          "fault isolation, shift keys, mouse, framebuffer, vga screen)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
