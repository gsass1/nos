#!/usr/bin/env python3
"""QEMU acceptance test for the polling ATA PIO driver."""

import os
import subprocess
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(REPO, "kernel.elf")
INITRD = os.path.join(REPO, "initrd", "initrd.tar")
TIMEOUT = 20


def boot(images, mode, markers, serial_log):
    if os.path.exists(serial_log):
        os.remove(serial_log)
    args = [
        "qemu-system-i386",
        "-kernel", KERNEL,
        "-initrd", INITRD,
        "-append", "ata-test=" + mode,
        "-serial", "file:" + serial_log,
        "-display", "none",
        "-monitor", "none",
    ]
    for index, image in enumerate(images):
        args += [
            "-drive",
            f"file={image},format=raw,if=ide,index={index},media=disk",
        ]
    proc = subprocess.Popen(
        args,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        deadline = time.time() + TIMEOUT
        while time.time() < deadline:
            try:
                with open(serial_log, "r", errors="replace") as f:
                    output = f.read()
            except FileNotFoundError:
                output = ""
            if "panic:" in output:
                raise RuntimeError("kernel panic:\n" + output[-1000:])
            if all(marker in output for marker in markers):
                return
            if proc.poll() is not None:
                raise RuntimeError(
                    f"QEMU exited {proc.returncode}: {proc.stderr.read()}"
                )
            time.sleep(0.1)
        raise RuntimeError(
            f"timed out waiting for {markers!r}; serial tail: {output[-1000:]!r}"
        )
    finally:
        proc.kill()
        proc.wait()


def main():
    with tempfile.TemporaryDirectory(prefix="nos-ata-") as tmp:
        images = [os.path.join(tmp, f"disk{i}.img") for i in range(4)]
        serial = os.path.join(tmp, "serial.log")
        for image in images:
            with open(image, "wb") as f:
                f.truncate(2 * 1024 * 1024)
        with open(images[0], "r+b") as f:
            f.seek(7 * 512)
            f.write(b"NOS ATA SEEDED SECTOR")

        boot(
            images,
            "write",
            ["ata: hda:", "ata: hdb:", "ata: hdc:", "ata: hdd:",
             "ATA TEST: seeded read ok",
             "ATA TEST: write and readback ok"],
            serial,
        )
        with open(images[0], "rb") as f:
            f.seek(8 * 512)
            persisted = f.read(len(b"NOS ATA PERSISTED SECTOR"))
        if persisted != b"NOS ATA PERSISTED SECTOR":
            raise RuntimeError(f"host persistence check failed: {persisted!r}")

        boot(
            images,
            "verify",
            ["ata: hda:", "ATA TEST: persistence ok"],
            serial,
        )

    print("PASS (ATA identify, seeded read, write, flush, reboot persistence)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
