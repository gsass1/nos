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
KEYMAP = {
    " ": "spc",
    "|": "shift-backslash",
    "<": "shift-comma",
    "^": "shift-6",
    "*": "shift-8",
    "$": "shift-4",
}
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


def check_surface_window(path):
    """The browser's surface window sits at (60,40), body at (61,64) sized
    780x540: mostly white page with dark rendered text, on the teal desktop."""
    try:
        w, h, px = read_ppm(path)
    except (OSError, ValueError, IndexError) as e:
        failures.append(f"surface screendump unreadable: {e}")
        return
    if (w, h) != (1024, 768):
        failures.append(f"surface screendump is {w}x{h}, expected 1024x768")
        return
    white = dark = 0
    for y in range(80, 300, 4):
        for x in range(70, 830, 4):
            off = (y * w + x) * 3
            r, g, b = px[off], px[off + 1], px[off + 2]
            if r > 200 and g > 200 and b > 200:
                white += 1
            elif r < 100 and g < 100 and b < 100:
                dark += 1
    if white < 1000:
        failures.append(f"surface window body not composited ({white} white samples)")
    if dark < 20:
        failures.append(f"no client text visible in the surface window ({dark} dark samples)")


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

        # xv6-style filters: cat defaults to stdin, echo produces a line, wc
        # counts it, and grep supports the ^, ., *, and $ matcher operators.
        sh.run("echo one two | cat | wc", ["1 2 8"], "cat stdin and wc")
        sh.run("echo abbbc | grep ^ab*c$ | wc", ["1 1 6"], "grep regexp")

        # A stage dying mid-pipeline: crash is killed by its page fault, the
        # kill path closes its pipe write end, and upper sees EOF instead of
        # hanging -- the prompt coming back is the real assertion.
        sh.run(
            "crash | upper",
            ["killed: page fault"],
            "pipeline EOF on killed writer",
        )

        # Error paths report distinct messages and nonzero exit statuses.
        sh.run("cat nope", ["not found", "[exit status 1]"], "cat missing file")
        sh.run("grep", ["usage: grep", "[exit status 1]"], "grep usage")
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

        # Display ownership: the two pipeline stages run concurrently and
        # both want the framebuffer; exactly one may get it. The loser's
        # stdout may be swallowed by the pipe (stage 1 writes into it), so
        # assert on the shell's exit-status report instead: the winner exits
        # 0 silently, the loser's failed fbmap exits 1 -- exactly once.
        before = sh.serial().count("[exit status 1]")
        sh.run("fbtest | fbtest", ["[exit status 1]"], "fb single-owner")
        delta = sh.serial().count("[exit status 1]") - before
        if delta != 1:
            failures.append(
                f"fb single-owner: expected exactly one denied fbmap, got {delta}")

        # Dying while owning the display: the kernel must give the fb back
        # and restore text mode even though the task never called fboff --
        # otherwise a crashed graphics app leaves the console invisible.
        sh.run("fbtest crash",
               ["fbtest: pattern drawn", "killed: page fault", "[exit status -1]"],
               "fb restore on crash")
        rows = sh.screen_rows()
        if not any("nsh$" in row for row in rows):
            failures.append("text mode not restored after a crash in graphics mode")

        # Window manager: a full scripted desktop session. Initial layout:
        # a single terminal window in slot 0 (140,120, 360x240) running sh,
        # focused (navy title). The cursor position is tracked in `cur` after
        # one corner-clamp reset, giving absolute positioning for every
        # subsequent click.
        sh.type_line("wm")
        if sh.wait_for("wm: started", "wm start"):
            sh.wait_for("nsh - NOS userspace shell", "startup terminal banner")
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
                (300, 132, (0, 0, 128), "terminal title (focused)"),
                (300, 300, (0, 0, 0), "terminal body black"),
            ], "wm initial")

            mouse_reset(sh)

            # The startup terminal has focus: keys reach the sh inside it;
            # output of it AND its children renders in the window (and
            # mirrors to serial, which we assert on).
            for k in "hello":
                sh.monitor("sendkey " + k)
                time.sleep(0.15)
            sh.monitor("sendkey ret")
            sh.wait_for("Hello from a loaded ELF program",
                        "hello ran inside the startup terminal")

            # Start menu opens above the taskbar (4 items: terminal,
            # browser, new window, about -> top edge at y=632).
            mouse_to(30, 752)
            click()
            dump("wm2.ppm", [
                (150, 640, (232, 232, 232), "start menu top (4 items)"),
                (20, 716, (232, 232, 232), "start menu bottom"),
            ], "wm start menu")

            # "new window" (item 2) spawns into slot 1 at (180,152),
            # 360x240, focused; the terminal drops to unfocused gray.
            mouse_to(80, 696)
            click()
            dump("wm3.ppm", [
                (350, 164, (0, 0, 128), "new window title (focused)"),
                (300, 132, (128, 128, 128), "terminal title unfocused"),
            ], "wm new window")

            # Drag the new window by its title bar from (350,164) by
            # +200,+100: it lands at (380,252).
            mouse_to(350, 164)
            sh.monitor("mouse_button 1")
            time.sleep(0.4)
            mouse_to(550, 264)
            sh.monitor("mouse_button 0")
            time.sleep(0.6)
            dump("wm4.ppm", [
                (650, 264, (0, 0, 128), "new window title after drag"),
                (350, 164, (0, 0, 0), "terminal body where the window was"),
            ], "wm after drag")

            # Close it via its X button (window at 380,252, 360 wide: close
            # box center is x+w-21+9, y+3+9). Focus falls to the terminal.
            mouse_to(728, 264)
            click()
            dump("wm5.ppm", [
                (650, 264, (0, 128, 128), "desktop after close"),
                (300, 132, (0, 0, 128), "terminal title focused after close"),
            ], "wm close button")

            # Minimize the terminal via its _ button (box center
            # x+w-2*18-5+9, y+3+9 = 468,132); its taskbar entry grays out.
            mouse_to(468, 132)
            click()
            dump("wm6.ppm", [
                (300, 300, (0, 128, 128), "desktop where minimized terminal was"),
                (200, 750, (144, 144, 144), "taskbar entry minimized"),
            ], "wm minimize")

            # Restore it from its taskbar entry (first entry: 72..224).
            mouse_to(150, 752)
            click()
            dump("wm7.ppm", [
                (300, 132, (0, 0, 128), "restored terminal title focused"),
                (300, 300, (0, 0, 0), "restored terminal body"),
            ], "wm restore from taskbar")

            # Menu item 0 spawns a second terminal (slot 1 -> 180,152,
            # 360x240, focused) over a console channel. `exit` inside it,
            # then its X: the clean close path, no kill needed.
            mouse_to(30, 752)
            click()
            mouse_to(80, 648)
            click()
            sh.wait_for("nsh - NOS userspace shell", "second terminal banner")
            for k in "exit":
                sh.monitor("sendkey " + k)
                time.sleep(0.15)
            sh.monitor("sendkey ret")
            sh.wait_for("bye!", "terminal shell exited")
            mouse_to(528, 164)
            click()

            # Third terminal (slot 1 reused, same geometry): run `spin`.
            # The shell blocks in SYS_WAIT on a child that never exits, so
            # closing this terminal exercises the SYS_KILL escalation.
            mouse_to(30, 752)
            click()
            mouse_to(80, 648)
            click()
            sh.wait_for("nsh - NOS userspace shell", "third terminal banner")
            for k in "spin":
                sh.monitor("sendkey " + k)
                time.sleep(0.15)
            sh.monitor("sendkey ret")
            sh.wait_for("spin: spinning", "spin runs inside the terminal")
            time.sleep(0.5)
            mouse_to(528, 164)
            click()
            sh.wait_for("wm: killed pid", "unresponsive shell was killed")

            # Window surfaces: "browser" in the start menu (item 1) execs
            # the graphics client. wm holds the display, so it must render
            # into its own offscreen buffer (SYS_WCREATE) which wm
            # composites as a window and routes input to.
            mouse_to(30, 752)
            click()
            mouse_to(80, 672)
            click()
            if sh.wait_for("wm: surface 'browser' 780x540", "browser surface"):
                time.sleep(1.5)  # first composited frame
                sppm = os.path.join(REPO, "tests", "surface.ppm")
                if os.path.exists(sppm):
                    os.remove(sppm)
                sh.monitor("screendump " + sppm)
                deadline = time.time() + 10
                while time.time() < deadline and not (
                    os.path.exists(sppm) and os.path.getsize(sppm) > 0
                ):
                    time.sleep(0.2)
                check_surface_window(sppm)

                # The surface window has focus: 'q' quits the browser, wm
                # notices the client died and removes the window.
                sh.monitor("sendkey q")
                sh.wait_for("wm: surface closed", "browser window closed")

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
          "framebuffer, fb ownership, window surfaces, vga screen)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
