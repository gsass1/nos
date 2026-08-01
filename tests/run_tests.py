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
KEYMAP = {" ": "spc", "|": "shift-backslash", "<": "shift-comma"}
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
        """Dump VGA text memory via the monitor; returns the 25 visible rows.

        Reads the monitor's stdout incrementally rather than quitting QEMU to
        collect it, so the call is retryable -- important when a busy guest
        (e.g. an orphaned spin) makes monitor replies slow.
        """
        import select

        fd = self.proc.stdout.fileno()
        # Drain whatever monitor chatter is pending (prompts, sendkey echo).
        while select.select([self.proc.stdout], [], [], 0)[0]:
            if not os.read(fd, 65536):
                break

        self.monitor("xp /4000bx 0xb8000")
        buf = ""
        deadline = time.time() + 10
        while time.time() < deadline:
            if select.select([self.proc.stdout], [], [], 0.2)[0]:
                chunk = os.read(fd, 65536)
                if not chunk:
                    break
                buf += chunk.decode(errors="replace")
            # 4000 dumped bytes -> 4000 0xNN tokens once complete.
            if len(re.findall(r"0x[0-9a-f]{2}", buf)) >= 4000:
                break

        cells = []
        for line in buf.splitlines():
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
        sh.run("cat symtable", ["kernel_base", "stack_top"], "cat symtable")

        # Pipes: hello's stdout is a pipe feeding upper, so only the
        # uppercased text reaches the terminal; the prompt returning proves
        # EOF propagated when the writer exited.
        sh.run("hello | upper", ["HELLO FROM A LOADED ELF PROGRAM"], "pipeline")

        # Three stages, and < redirection reading an initrd file to EOF.
        sh.run("cat symtable | upper | upper", ["KERNEL_BASE"], "3-stage pipeline")
        sh.run("upper < symtable", ["STACK_TOP"], "input redirection")

        # A stage dying mid-pipeline: crash is killed by its page fault, the
        # kill path closes its pipe write end, and upper sees EOF instead of
        # hanging -- the prompt coming back is the real assertion.
        sh.run(
            "crash | upper",
            ["killed: page fault", "[exit status -1]"],
            "pipeline EOF on killed writer",
        )

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

        # Hostile user pointers: every syscall must reject unmapped, kernel,
        # and out-of-window pointers with -1 instead of faulting in ring 0
        # (a panic) or reading/writing kernel memory on the caller's behalf.
        sh.run("badptr", ["badptr: all hostile pointers rejected"], "hostile pointers")

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

        # Window manager: a full scripted desktop session. Initial layout:
        # win1 "welcome" (140,120,400x300) unfocused (gray title), win2
        # "notes" (420,260,360x240) focused (navy title). The cursor position
        # is tracked in `cur` after one corner-clamp reset, giving absolute
        # positioning for every subsequent click.
        sh.type_line("wm")
        if sh.wait_for("wm: started", "wm start"):
            time.sleep(1.0)
            cur = [0, 0]

            def mouse_to(x, y):
                mouse_step(sh, x - cur[0], y - cur[1])
                cur[0], cur[1] = x, y

            def click():
                sh.monitor("mouse_button 1")
                time.sleep(0.4)
                sh.monitor("mouse_button 0")
                time.sleep(0.4)

            def dump(name, checks, desc):
                path = os.path.join(REPO, "tests", name)
                if os.path.exists(path):
                    os.remove(path)
                sh.monitor("screendump " + path)
                time.sleep(1.5)
                sample(path, checks, desc)

            dump("wm1.ppm", [
                (8, 8, (0, 128, 128), "desktop"),
                (512, 760, (192, 192, 192), "taskbar"),
                (340, 132, (128, 128, 128), "win1 title (unfocused)"),
                (340, 200, (255, 255, 255), "win1 body"),
                (600, 272, (0, 0, 128), "win2 title (focused)"),
            ], "wm initial")

            mouse_reset(sh)

            # Drag win2 by its title bar from (600,272) by +120,+60.
            mouse_to(600, 272)
            sh.monitor("mouse_button 1")
            time.sleep(0.4)
            mouse_to(720, 332)
            sh.monitor("mouse_button 0")
            time.sleep(0.6)
            dump("wm2.ppm", [
                (700, 330, (0, 0, 128), "win2 title after drag"),
                (600, 272, (0, 128, 128), "desktop where win2 was"),
            ], "wm after drag")

            # Close win2 via its X button (win2 now at 540,320, 360 wide:
            # close box center is x+w-21+9, y+3+9). Focus falls to win1.
            mouse_to(888, 332)
            click()
            dump("wm3.ppm", [
                (700, 330, (0, 128, 128), "desktop after close"),
                (340, 132, (0, 0, 128), "win1 title focused after close"),
            ], "wm close button")

            # Start menu opens above the taskbar; "new window" spawns into
            # slot 1 (freed by the close) at (180,152), 360x240, focused.
            mouse_to(30, 752)
            click()
            dump("wm4.ppm", [
                (20, 716, (232, 232, 232), "start menu open"),
            ], "wm start menu")
            mouse_to(80, 696)
            click()
            dump("wm5.ppm", [
                (280, 164, (0, 0, 128), "new window title (focused)"),
                (340, 132, (128, 128, 128), "win1 title unfocused again"),
            ], "wm new window")

            # Minimize the new window via its _ button (box center
            # x+w-2*18-5+9, y+3+9 = 508,164); (280,164) then shows win1 body.
            mouse_to(508, 164)
            click()
            dump("wm6.ppm", [
                (280, 164, (255, 255, 255), "win1 body where minimized win was"),
                (340, 132, (0, 0, 128), "win1 focused after minimize"),
            ], "wm minimize")

            # Restore it from its taskbar entry (second entry: 232..384).
            mouse_to(300, 752)
            click()
            dump("wm7.ppm", [
                (280, 164, (0, 0, 128), "restored window title focused"),
            ], "wm restore from taskbar")

            # Terminal: menu item 0 spawns a window running the real sh over
            # a console channel (slot 2 -> 220,184, 360x240, focused). Keys
            # are forwarded to it; output of it AND its children renders in
            # the window (and mirrors to serial, which we assert on).
            mouse_to(30, 752)
            click()
            mouse_to(80, 672)
            click()
            sh.wait_for("nsh - NOS userspace shell", "terminal sh banner")
            time.sleep(0.8)
            dump("wm8.ppm", [
                (300, 300, (0, 0, 0), "terminal body black"),
            ], "wm terminal window")
            for k in "hello":
                sh.monitor("sendkey " + k)
                time.sleep(0.15)
            sh.monitor("sendkey ret")
            sh.wait_for("Hello from a loaded ELF program",
                        "hello ran inside the terminal")
            for k in "exit":
                sh.monitor("sendkey " + k)
                time.sleep(0.15)
            sh.monitor("sendkey ret")
            sh.wait_for("bye!", "terminal shell exited")

            # Close the (now dead) terminal via its X: the clean path, no
            # kill needed. Terminal is slot 2 at (220,184), 360 wide.
            mouse_to(568, 196)
            click()

            # Second terminal (slot 2 reused, same geometry): run `spin`.
            # The shell blocks in SYS_WAIT on a child that never exits, so
            # closing this terminal exercises the SYS_KILL escalation.
            mouse_to(30, 752)
            click()
            mouse_to(80, 672)
            click()
            sh.wait_for("nsh - NOS userspace shell", "second terminal banner")
            for k in "spin":
                sh.monitor("sendkey " + k)
                time.sleep(0.15)
            sh.monitor("sendkey ret")
            sh.wait_for("spin: spinning", "spin runs inside the terminal")
            time.sleep(0.5)
            mouse_to(568, 196)
            click()
            sh.wait_for("wm: killed pid", "unresponsive shell was killed")

            sh.monitor("sendkey esc")
            sh.wait_for("wm: exit", "wm exits to shell")
            sh.wait_for("nsh$", "prompt after wm")

        # The kernel never panicked.
        if "panic:" in sh.serial():
            failures.append("kernel panicked (see serial log)")

        # The prompt must be on the VISIBLE screen, not off in VGA memory.
        # Retried: a heavily loaded guest can lag the monitor's first reply.
        rows = []
        for _ in range(3):
            rows = sh.screen_rows()
            if any("nsh$" in row for row in rows):
                break
            time.sleep(1.0)
        if not any("nsh$" in row for row in rows):
            seen = " | ".join(r.rstrip() for r in rows if r.strip())
            failures.append(
                f"prompt not on the visible VGA screen (rows 0-24); saw: {seen[:400]!r}")
    finally:
        sh.kill()

    if failures:
        print("FAIL")
        for f in failures:
            print("  - " + f)
        return 1
    print("PASS (boot, ls, exec/wait, argv, file io, sbrk, pipes, redirects, "
          "error paths, fault isolation, hostile pointers, shift keys, mouse, "
          "framebuffer, vga screen)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
