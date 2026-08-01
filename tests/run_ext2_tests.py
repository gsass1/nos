#!/usr/bin/env python3
"""QEMU acceptance test for ext2 read/write support.

Creates a host ext2 image with mke2fs, boots NOS with it as the first ATA
disk, runs the `disktest` user utility from the shell to exercise nested
listing/reads and guest file creation/write, then reboots the same image to
verify persistence. After each guest session, e2fsck -fn validates the
on-disk filesystem and debugfs confirms the written content.

Requires: mke2fs, e2fsck, debugfs (e2fsprogs), qemu-system-i386.
Usage: python3 tests/run_ext2_tests.py  (from the repo root, after make && make initrd)
"""

import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "kernel.elf")
INITRD = os.path.join(REPO, "initrd", "initrd.tar")
SERIAL_LOG = os.path.join(REPO, "tests", "ext2_serial.log")
TIMEOUT = 45

KEYMAP = {
    " ": "spc",
    "|": "shift-backslash",
    ">": "shift-dot",
    "/": "slash",
    ".": "dot",
}
for _c in "abcdefghijklmnopqrstuvwxyz0123456789":
    KEYMAP[_c] = _c
for _c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
    KEYMAP[_c] = "shift-" + _c.lower()


def keyname(ch):
    """Map a command character to a QEMU sendkey name. An unmapped character
    would be rejected by the monitor and surface much later as a confusing
    missing-marker timeout, so fail loudly here instead."""
    if ch not in KEYMAP:
        raise KeyError(f"no sendkey mapping for character {ch!r}; extend KEYMAP")
    return KEYMAP[ch]


failures = []


def check_tools():
    """The suite skips when the ext2 host tools are missing (fine locally).
    Set NOS_REQUIRE_EXT2_TOOLS=1 in CI so a runner image losing e2fsprogs
    fails the build instead of silently dropping all ext2 coverage."""
    for tool in ("mke2fs", "e2fsck", "debugfs", "qemu-system-i386"):
        if not shutil.which(tool):
            if os.environ.get("NOS_REQUIRE_EXT2_TOOLS"):
                print(f"FAIL: required tool '{tool}' not found", file=sys.stderr)
                sys.exit(1)
            print(f"SKIP: required tool '{tool}' not found", file=sys.stderr)
            return False
    return True


def create_image(path, srcdir):
    """Create a feature-free ext2 rev-0 image with 1 KiB blocks, one group."""
    img = path
    with open(img, "wb") as f:
        f.truncate(4 * 1024 * 1024)  # 4 MB
    common = [
        "mke2fs", "-t", "ext2", "-b", "1024",
        "-O", "none", "-I", "128", "-N", "128",
        "-d", srcdir, "-F", "-q",
    ]
    # e2fsprogs >= 1.47.3 removed "-r 0" (and refuses -E revision=0 with any
    # invocation we found). Older releases -- current CI runners -- accept
    # "-r 0" directly; on newer ones, build the same feature-free filesystem
    # as revision 1 and stamp the superblock back to revision 0 with debugfs
    # (verified: e2fsck -fn is clean and all feature masks stay zero).
    r = subprocess.run(common + ["-r", "0", img], capture_output=True)
    if r.returncode != 0:
        subprocess.run(common + [img], check=True, capture_output=True)
        subprocess.run(["debugfs", "-w", "-R", "ssv rev_level 0", img],
                       check=True, capture_output=True)


def make_srcdir(path):
    """Populate the host source directory with test files."""
    os.makedirs(os.path.join(path, "subdir"))
    with open(os.path.join(path, "hello.txt"), "w") as f:
        f.write("Hello from ext2\n")
    with open(os.path.join(path, "subdir", "README"), "w") as f:
        f.write("Nested README content\n")


def boot_and_run(image, command, markers, desc):
    """Boot NOS with the ext2 image, type a shell command, check serial."""
    if os.path.exists(SERIAL_LOG):
        os.remove(SERIAL_LOG)

    proc = subprocess.Popen(
        [
            "qemu-system-i386",
            "-kernel", KERNEL,
            "-initrd", INITRD,
            "-serial", "file:" + SERIAL_LOG,
            "-display", "none",
            "-monitor", "stdio",
            "-drive", f"file={image},format=raw,if=ide,index=0,media=disk",
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
        text=True,
    )

    pos = 0
    try:
        deadline = time.time() + 15
        # Wait for shell prompt.
        while time.time() < deadline:
            try:
                with open(SERIAL_LOG, "r", errors="replace") as f:
                    data = f.read()
            except FileNotFoundError:
                data = ""
            if "nsh$" in data:
                pos = data.index("nsh$") + 4
                break
            if "panic:" in data:
                failures.append(f"{desc}: kernel panic during boot")
                return False
            if proc.poll() is not None:
                failures.append(f"{desc}: QEMU exited early")
                return False
            time.sleep(0.2)
        else:
            failures.append(f"{desc}: no shell prompt")
            return False

        # Type the command.
        for ch in command:
            proc.stdin.write("sendkey " + keyname(ch) + "\n")
            proc.stdin.flush()
            time.sleep(0.06)
        proc.stdin.write("sendkey ret\n")
        proc.stdin.flush()

        # Wait for markers.
        deadline = time.time() + TIMEOUT
        while time.time() < deadline:
            try:
                with open(SERIAL_LOG, "r", errors="replace") as f:
                    data = f.read()
            except FileNotFoundError:
                data = ""
            if "panic:" in data:
                failures.append(f"{desc}: kernel panic during test")
                return False
            idx = data.find("nsh$", pos)
            all_found = all(m in data for m in markers)
            if all_found and idx != -1:
                return True
            if proc.poll() is not None:
                break
            time.sleep(0.2)

        # Timed out: the marker+prompt condition never became true, so this
        # is always a failure. Diagnose against the FULL log -- a short tail
        # can both hide markers that did arrive earlier and, if every marker
        # happens to fall inside it, make a timeout look like a pass.
        try:
            with open(SERIAL_LOG, "r", errors="replace") as f:
                data = f.read()
        except FileNotFoundError:
            data = ""
        tail = data[-600:]
        missing = [m for m in markers if m not in data]
        if missing:
            failures.append(f"{desc}: missing {missing!r} (serial tail: {tail!r})")
        else:
            failures.append(
                f"{desc}: markers seen but no prompt returned (serial tail: {tail!r})")
        return False
    finally:
        proc.kill()
        proc.wait()


def boot_and_run_seq(image, commands, desc):
    """Boot NOS with the ext2 image, type multiple shell commands in sequence,
    waiting for the prompt after each. Returns True if all commands completed
    without a panic or timeout."""
    if os.path.exists(SERIAL_LOG):
        os.remove(SERIAL_LOG)

    proc = subprocess.Popen(
        [
            "qemu-system-i386",
            "-kernel", KERNEL,
            "-initrd", INITRD,
            "-serial", "file:" + SERIAL_LOG,
            "-display", "none",
            "-monitor", "stdio",
            "-drive", f"file={image},format=raw,if=ide,index=0,media=disk",
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
        text=True,
    )

    try:
        # Wait for the first shell prompt.
        pos = 0
        deadline = time.time() + 15
        while time.time() < deadline:
            try:
                with open(SERIAL_LOG, "r", errors="replace") as f:
                    data = f.read()
            except FileNotFoundError:
                data = ""
            if "panic:" in data:
                failures.append(f"{desc}: kernel panic during boot")
                return False
            if "nsh$" in data:
                pos = data.index("nsh$") + 4
                break
            if proc.poll() is not None:
                failures.append(f"{desc}: QEMU exited early")
                return False
            time.sleep(0.2)
        else:
            failures.append(f"{desc}: no shell prompt")
            return False

        # Type each command, waiting for the next prompt.
        for cmd in commands:
            for ch in cmd:
                proc.stdin.write("sendkey " + keyname(ch) + "\n")
                proc.stdin.flush()
                time.sleep(0.06)
            proc.stdin.write("sendkey ret\n")
            proc.stdin.flush()

            deadline = time.time() + TIMEOUT
            while time.time() < deadline:
                try:
                    with open(SERIAL_LOG, "r", errors="replace") as f:
                        data = f.read()
                except FileNotFoundError:
                    data = ""
                if "panic:" in data:
                    failures.append(f"{desc}: kernel panic during '{cmd}'")
                    return False
                idx = data.find("nsh$", pos)
                if idx != -1:
                    pos = idx + 4
                    break
                if proc.poll() is not None:
                    failures.append(f"{desc}: QEMU exited during '{cmd}'")
                    return False
                time.sleep(0.2)
            else:
                failures.append(f"{desc}: no prompt after '{cmd}'")
                return False

        return True
    finally:
        proc.kill()
        proc.wait()


def run_e2fsck(image, desc):
    """Run e2fsck -fn (read-only, force) and check for a clean result."""
    r = subprocess.run(
        ["e2fsck", "-fn", image],
        capture_output=True, text=True,
    )
    # e2fsck returns 0 for clean, 1 for errors found and corrected,
    # 2 for errors found and not corrected, etc.
    if r.returncode != 0:
        failures.append(
            f"{desc}: e2fsck failed (rc={r.returncode}):\n{r.stdout}\n{r.stderr}"
        )
        return False
    return True


def run_debugfs_cat(image, path, expected, desc):
    """Use debugfs to read a file from the image and compare content."""
    r = subprocess.run(
        ["debugfs", "-R", f"cat {path}", image],
        capture_output=True, text=True,
    )
    # debugfs prints file content to stdout, status messages to stderr.
    # The content may have a trailing newline added by debugfs.
    content = r.stdout
    if expected not in content:
        failures.append(
            f"{desc}: debugfs cat {path}: expected {expected!r}, got {content!r} "
            f"(rc={r.returncode}, stderr={r.stderr!r})"
        )
        return False
    return True


def run_debugfs_ls(image, path, expected_entry, desc):
    """Use debugfs to list a directory and verify an entry is present."""
    r = subprocess.run(
        ["debugfs", "-R", f"ls {path}", image],
        capture_output=True, text=True,
    )
    if expected_entry not in r.stdout:
        failures.append(
            f"{desc}: debugfs ls {path}: expected entry {expected_entry!r}, "
            f"got {r.stdout!r} (rc={r.returncode}, stderr={r.stderr!r})"
        )
        return False
    return True


def run_debugfs_stat(image, path, expected_fields, desc):
    """Use debugfs to stat an inode and verify expected fields are present."""
    r = subprocess.run(
        ["debugfs", "-R", f"stat {path}", image],
        capture_output=True, text=True,
    )
    for field in expected_fields:
        if field not in r.stdout:
            failures.append(
                f"{desc}: debugfs stat {path}: expected {field!r}, "
                f"got {r.stdout!r} (rc={r.returncode}, stderr={r.stderr!r})"
            )
            return False
    return True


def run_debugfs_link_count(image, path, desc):
    """Use debugfs to stat an inode and return its link count as an int,
    or -1 if it cannot be parsed."""
    r = subprocess.run(
        ["debugfs", "-R", f"stat {path}", image],
        capture_output=True, text=True,
    )
    for line in r.stdout.splitlines():
        line = line.strip()
        if line.startswith("Links:"):
            # debugfs prints "Links: N" (or "Links: N   ....")
            parts = line.split()
            if len(parts) >= 2:
                try:
                    return int(parts[1])
                except ValueError:
                    pass
    failures.append(f"{desc}: could not parse link count from {r.stdout!r} "
                     f"(rc={r.returncode}, stderr={r.stderr!r})")
    return -1


def boot_no_disk(markers, desc):
    """Boot NOS with no disk attached; verify it comes up normally."""
    if os.path.exists(SERIAL_LOG):
        os.remove(SERIAL_LOG)

    proc = subprocess.Popen(
        [
            "qemu-system-i386",
            "-kernel", KERNEL,
            "-initrd", INITRD,
            "-serial", "file:" + SERIAL_LOG,
            "-display", "none",
            "-monitor", "none",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                with open(SERIAL_LOG, "r", errors="replace") as f:
                    data = f.read()
            except FileNotFoundError:
                data = ""
            if "panic:" in data:
                failures.append(f"{desc}: kernel panicked")
                return False
            if all(m in data for m in markers):
                return True
            if proc.poll() is not None:
                break
            time.sleep(0.2)
        tail = data[-400:] if data else ""
        failures.append(f"{desc}: missing markers (serial tail: {tail!r})")
        return False
    finally:
        proc.kill()
        proc.wait()


def boot_bad_disk(image, desc):
    """Boot NOS with a malformed disk image; verify it boots and does NOT
    mount ext2 (no 'ext2: mounted' in serial) and does not panic."""
    if os.path.exists(SERIAL_LOG):
        os.remove(SERIAL_LOG)

    proc = subprocess.Popen(
        [
            "qemu-system-i386",
            "-kernel", KERNEL,
            "-initrd", INITRD,
            "-serial", "file:" + SERIAL_LOG,
            "-display", "none",
            "-monitor", "none",
            "-drive", f"file={image},format=raw,if=ide,index=0,media=disk",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                with open(SERIAL_LOG, "r", errors="replace") as f:
                    data = f.read()
            except FileNotFoundError:
                data = ""
            if "panic:" in data:
                failures.append(f"{desc}: kernel panicked on bad disk")
                return False
            if "nsh$" in data:
                # Check that ext2 was NOT mounted.
                if "ext2: mounted" in data:
                    failures.append(
                        f"{desc}: mounted an invalid filesystem"
                    )
                    return False
                return True
            if proc.poll() is not None:
                break
            time.sleep(0.2)
        failures.append(f"{desc}: no shell prompt (serial tail: {data[-400:]!r})")
        return False
    finally:
        proc.kill()
        proc.wait()


def main():
    if not (os.path.exists(KERNEL) and os.path.exists(INITRD)):
        print("Build first: make && make initrd", file=sys.stderr)
        return 2
    if not check_tools():
        return 0  # Skip, not fail, when tools are absent.

    import tempfile

    with tempfile.TemporaryDirectory(prefix="nos-ext2-") as tmp:
        srcdir = os.path.join(tmp, "src")
        make_srcdir(srcdir)

        image = os.path.join(tmp, "disk.img")
        create_image(image, srcdir)

        # --- Phase 0: normal boot without a disk must still work ---
        boot_no_disk(
            ["Welcome to NOS!", "nsh - NOS userspace shell"],
            "boot without disk",
        )

        # --- Phase 1: guest reads host-created files, then writes a new file ---
        ok = boot_and_run(
            image, "disktest",
            [
                "ext2: mounted",          # kernel mounted the filesystem
                "disktest: read ok",       # read /disk/hello.txt
                "disktest: nested read ok",  # read /disk/subdir/README
                "disktest: write-only read rejected ok",  # access-mode regression
                "disktest: write ok",      # created and wrote /disk/written.txt
                "disktest: readback ok",   # read it back in the same session
                "disktest: PASS",
            ],
            "phase 1 (read + write)",
        )
        if not ok:
            # Fall through to e2fsck even on failure to diagnose corruption.
            pass

        # --- e2fsck after guest write ---
        run_e2fsck(image, "e2fsck after write")

        # --- debugfs: verify the guest-written file content ---
        run_debugfs_cat(
            image, "/written.txt",
            "NOS ext2 persistent guest write",
            "debugfs after write",
        )

        # --- Phase 2: reboot same image, verify persistence ---
        ok = boot_and_run(
            image, "disktest verify",
            [
                "ext2: mounted",
                "disktest: read ok",
                "disktest: nested read ok",
                "disktest: verify ok",  # read /disk/written.txt after reboot
                "disktest: PASS",
            ],
            "phase 2 (persistence verify)",
        )

        # --- e2fsck after reboot read ---
        run_e2fsck(image, "e2fsck after reboot")

        # --- debugfs: final check ---
        run_debugfs_cat(
            image, "/written.txt",
            "NOS ext2 persistent guest write",
            "debugfs after reboot",
        )

        # --- Phase 3: repeat opens exercise the inode cache ---
        # disktest opens hello.txt, subdir/README, and written.txt — each
        # resolve hits the cache after the first boot. Run it twice on the
        # same image to exercise cache lookups across multiple sessions.
        boot_and_run(
            image, "disktest",
            ["disktest: read ok", "disktest: PASS"],
            "phase 3a (cache exercise)",
        )
        boot_and_run(
            image, "disktest verify",
            ["disktest: read ok", "disktest: PASS"],
            "phase 3b (cache exercise)",
        )

        # Capture the root directory's link count before mkdir so we can
        # verify the parent link count increased after the guest creates a
        # subdirectory.
        root_links_before = run_debugfs_link_count(
            image, "/", "root link count before mkdir",
        )

        # --- Phase 5: mkdir, output redirection, pipeline+redirect ---
        # Create a directory, redirect echo into a nested file, and exercise
        # pipeline+redirect interaction — all from the shell.
        ok = boot_and_run_seq(
            image,
            [
                "mkdir disk/mydir",
                "echo redirtest > disk/mydir/file.txt",
                "echo hello | upper > disk/pipe.txt",
            ],
            "phase 5 (mkdir + redirect)",
        )
        if not ok:
            pass  # fall through to e2fsck for diagnostics

        # The non-final-stage redirect: wc saw EOF (0 0 0) and side.txt
        # holds the redirected data, proving the pipe was still created and
        # closed correctly even though stdout went to a file.
        boot_and_run(
            image, "echo side > disk/side.txt | wc",
            ["0 0 0"],
            "phase 5 (non-final stage redirect: downstream sees EOF)",
        )

        # Read back the redirected files in fresh boots.
        boot_and_run(
            image, "cat disk/mydir/file.txt",
            ["redirtest"],
            "phase 5 readback (cat redirected file)",
        )
        boot_and_run(
            image, "cat disk/pipe.txt",
            ["HELLO"],
            "phase 5 readback (cat pipeline-redirected file)",
        )
        boot_and_run(
            image, "cat disk/side.txt",
            ["side"],
            "phase 5 readback (cat non-final-stage redirect file)",
        )

        # --- e2fsck after mkdir + redirect ---
        run_e2fsck(image, "e2fsck after mkdir + redirect")

        # --- debugfs: verify directory entries, content, and link counts ---
        run_debugfs_ls(image, "/mydir", "file.txt", "debugfs ls /mydir")
        run_debugfs_stat(
            image, "/mydir", ["directory", "Links: 2"],
            "debugfs stat /mydir",
        )
        run_debugfs_cat(
            image, "/mydir/file.txt", "redirtest",
            "debugfs cat /mydir/file.txt",
        )
        run_debugfs_cat(
            image, "/pipe.txt", "HELLO",
            "debugfs cat /pipe.txt",
        )
        run_debugfs_cat(
            image, "/side.txt", "side",
            "debugfs cat /side.txt",
        )
        # Verify the root (parent) link count increased by 1 after mkdir.
        if root_links_before >= 0:
            root_links_after = run_debugfs_link_count(
                image, "/", "root link count after mkdir",
            )
            if root_links_after >= 0:
                if root_links_after != root_links_before + 1:
                    failures.append(
                        f"root link count: expected {root_links_before + 1}, "
                        f"got {root_links_after}"
                    )

        # --- Phase 6: reboot, verify mkdir + redirect persistence ---
        boot_and_run(
            image, "cat disk/mydir/file.txt",
            ["redirtest"],
            "phase 6 (reboot: redirected file persists)",
        )
        boot_and_run(
            image, "cat disk/pipe.txt",
            ["HELLO"],
            "phase 6 (reboot: pipeline-redirected file persists)",
        )
        boot_and_run(
            image, "cat disk/side.txt",
            ["side"],
            "phase 6 (reboot: non-final-stage redirect persists)",
        )

        # --- e2fsck + debugfs after reboot ---
        run_e2fsck(image, "e2fsck after reboot (mkdir)")
        run_debugfs_ls(image, "/mydir", "file.txt",
                        "debugfs after reboot (dir listing)")
        run_debugfs_stat(
            image, "/mydir", ["directory", "Links: 2"],
            "debugfs after reboot (dir stat)",
        )
        run_debugfs_cat(
            image, "/mydir/file.txt", "redirtest",
            "debugfs after reboot (file content)",
        )

        # --- Phase 7: shell redirection error cases ---
        boot_and_run(
            image, "echo hello >",
            ["needs a filename"],
            "error: missing redirect filename",
        )
        boot_and_run(
            image, "echo hi > disk/a > disk/b",
            ["duplicate"],
            "error: duplicate output redirection",
        )
        # A redirection operator as the filename is a missing-filename error,
        # not a file literally named '>'.
        boot_and_run(
            image, "echo hi > >",
            ["needs a filename"],
            "error: operator as redirect filename",
        )

        # e2fsck should still be clean (error cases create no files).
        run_e2fsck(image, "e2fsck after error cases")

        # --- Phase 8: malformed disk images must not mount or panic ---

        # 8a: wrong magic (not ext2 at all)
        bad_magic = os.path.join(tmp, "bad_magic.img")
        with open(bad_magic, "wb") as f:
            f.truncate(4 * 1024 * 1024)
        with open(bad_magic, "r+b") as f:
            f.seek(1024 + 56)  # s_magic offset
            f.write(b"\x00\x00")  # wrong magic
        boot_bad_disk(bad_magic, "bad magic")

        # 8b: unsupported features (set incompat feature bit)
        bad_feat = os.path.join(tmp, "bad_feat.img")
        create_image(bad_feat, srcdir)
        with open(bad_feat, "r+b") as f:
            f.seek(1024 + 96)  # s_feature_incompat offset
            f.write(b"\x01\x00\x00\x00")  # set bit 0 (journal)
        boot_bad_disk(bad_feat, "unsupported incompat feature")

        # 8c: too-large geometry (blocks count exceeds device)
        bad_geom = os.path.join(tmp, "bad_geom.img")
        with open(bad_geom, "wb") as f:
            f.truncate(512 * 1024)  # 512 KB — much smaller than 4 MB fs
        create_image(bad_geom, srcdir)
        # The mke2fs-created image thinks it has 4 MB but the QEMU drive is
        # only 512 KB. Our validation should reject it.
        # Actually mke2fs will size the FS to the file, so we need a different
        # approach: create a 4 MB image but lie about the block device size
        # by truncating the QEMU drive. Instead, corrupt the block count.
        with open(bad_geom, "r+b") as f:
            f.seek(1024 + 4)  # s_blocks_count
            import struct
            f.write(struct.pack("<I", 0x00100000))  # claim 1M blocks
        boot_bad_disk(bad_geom, "out-of-device geometry")

        # 8d: data pointer into metadata (root inode i_block[0] points at
        # the superblock block). The mount must reject this because the
        # pointer is below first_data_block.
        bad_ptr = os.path.join(tmp, "bad_ptr.img")
        create_image(bad_ptr, srcdir)
        with open(bad_ptr, "r+b") as f:
            import struct
            # Root inode is inode 2, at inode_table_block * 1024 + (2-1)*128.
            # Read the group descriptor to find the inode table block.
            f.seek(2048)  # GD at block 2
            gd = f.read(32)
            it_block = struct.unpack_from("<I", gd, 8)[0]
            # Root inode offset: it_block * 1024 + 128 (inode 2, 128-byte inodes)
            root_off = it_block * 1024 + 128
            # i_block[0] is at offset 40 within the inode
            f.seek(root_off + 40)
            f.write(struct.pack("<I", 1))  # point at the superblock block

        boot_bad_disk(bad_ptr, "data pointer into metadata")

    if failures:
        print("FAIL")
        for f in failures:
            print("  - " + f)
        return 1
    print("PASS (ext2 mount, nested listing/reads, guest write, "
          "reboot persistence, repeat-open cache, mkdir + redirect, "
          "non-final-stage redirect, redirect error cases, "
          "malformed-mount rejection, no-disk boot, "
          "e2fsck clean, debugfs content/link counts verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
