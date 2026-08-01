# Debugging NOS

This is a 32-bit x86 multiboot kernel that runs under QEMU. Because there is
no userspace debugger and a crash can silently reset the machine, debugging
leans on four tools, roughly in order of how often you reach for them:

1. **Serial output** — `kprintf` mirrors everything to COM1. Fastest signal.
2. **In-kernel backtraces** — `panic()` prints a symbol-resolved stack trace.
3. **QEMU CPU logging** — `-d int,cpu_reset` explains resets and faults.
4. **lldb + QEMU's gdb stub** — real breakpoints, single-stepping, registers.

All commands below are run from the repo root. The cross tools
(`i686-elf-gcc`, `i686-elf-nm`, `i686-elf-objdump`, `i686-elf-addr2line`) and
`qemu-system-i386` come from Homebrew; `lldb` ships with macOS.

---

## 0. Build and run

```sh
make          # build kernel.elf
make run      # regenerate the initrd (with a matching symbol table) and boot
```

`make run` opens a QEMU window (VGA output + PS/2 keyboard) and mirrors the
kernel's serial console to your terminal. Quit QEMU with **Ctrl-A** then **X**.

The kernel is booted directly as a multiboot image (`-kernel kernel.elf`) with
the initrd passed as a module (`-initrd initrd/initrd.tar`); no GRUB or ISO is
involved. The `.bochsrc` and `*.bat` files are a legacy Windows/Bochs setup and
are not used here.

---

## 1. Serial output — the fastest loop

`kprintf`, `mprintf` (the `[MODULE]: ...` lines), and `DPRINT` all write to
COM1, so you see them even when VGA is unusable or the machine dies mid-boot.
Serial writes are unbuffered and per-character, so **the last line you see is
the last line that fully executed** — if output stops at `[KBRD]: ...`, the
fault is at or immediately after that point.

Add a probe anywhere with `kprintf("here: x=0x%08x\n", x);` and rebuild.

To capture serial to a file (useful for headless runs and for diffing across
attempts) instead of the terminal:

```sh
qemu-system-i386 -kernel kernel.elf -initrd initrd/initrd.tar \
  -display none -serial file:serial_out.txt -no-reboot
```

> Note: `mprintf` currently ignores its log-level argument, so every
> `LOGLEVEL_DEBUG` line prints too. That is handy for tracing but noisy.

---

## 2. In-kernel backtraces

`panic()` prints the CPU state and a stack trace resolved to function names,
e.g. a page fault triggered from the shell:

```
panic: Page fault (pid:0, eip:0x00102045, cr2:0xb0000000, error:0x00000002)
Stack trace:
    0x00101a97 [panic]
    0x0010157e [isr_exc_PF]
    0x00101300 [_asm_exc_PF]
    0x001020ea [shell]
    0x00102243 [kmain]
    0x00100019 [_start]
```

Read it top-down: innermost frame first. For an exception the `eip=` in the
panic line is the exact faulting instruction (feed it to `addr2line`, see
§5); the trace shows the call chain that led there. The chain crosses the
interrupt stub (`_asm_exc_*`), so the frame just under the handler is where the
fault actually happened.

### How it works

Symbol resolution lives in `kernel/kernel.c` (`sym_init` / `sym_get`). At boot
it reads a file called `symtable` from the initrd — the output of
`i686-elf-nm -n kernel.elf` — into an address-sorted list, and `sym_get(addr)`
returns the nearest symbol at or below `addr`. You'll see
`Sym: loaded N symbols` during boot if it worked.

**The symbol table must match the kernel binary.** The `Makefile` regenerates
`initrd/symtable` and repacks `initrd/initrd.tar` from the *current*
`kernel.elf` every time you `make run` (or `make initrd`). If you ever boot a
hand-built initrd, or your traces resolve to plausible-but-wrong names,
regenerate it:

```sh
make initrd
```

### Forcing a trace

To inspect state at a point of interest, just call `panic("checkpoint\n");`.
To exercise the fault path, dereference an unmapped address, e.g. from a
temporary shell command:

```c
*(volatile int *)0xB0000000 = 1;   // page fault -> isr_exc_PF -> panic -> trace
```

(High addresses like `0xB0000000` are unmapped; note that **low memory,
including address 0, is identity-mapped**, so a NULL dereference reads garbage
instead of faulting — see §7.)

---

## 3. QEMU CPU logging — why did it reset?

A triple fault silently reboots the CPU, which looks like a boot loop. Stop the
reboot and log CPU events:

```sh
qemu-system-i386 -kernel kernel.elf -initrd initrd/initrd.tar \
  -display none -serial file:serial_out.txt \
  -no-reboot -d int,cpu_reset -D qlog.txt
```

- `-no-reboot` makes QEMU **exit** on triple fault instead of looping — if the
  process exits on its own, you triple-faulted.
- `-d int` logs every interrupt/exception as it is *delivered through the IDT*.
- `-d cpu_reset` dumps CPU state on reset.

In `qlog.txt`, each delivered vector looks like:

```
v=21 e=0000 i=0 cpl=0 IP=0008:00101f2e pc=00101f2e SP=0010:0010c93a ...
```

`v=` is the vector in hex (see the table in §6). `pc=` is where the CPU was.
Two facts do most of the work:

- **If a fault vector is logged** (`v=0e` page fault, `v=0d` GP, `v=08` double
  fault), the CPU reached your handler — check the handler and the faulting
  `pc`.
- **If the CPU resets with _no_ fault vector logged**, it couldn't even
  dispatch the fault — that points at a bad IDT/GDT/stack at delivery time, or
  a *commanded* reset (e.g. the 8042 pulse-reset gotcha in §7), not an ordinary
  fault.

### Instruction-level trace (last resort)

When you need the exact instruction stream into a crash, log every instruction:

```sh
qemu-system-i386 -kernel kernel.elf -initrd initrd/initrd.tar \
  -display none -serial file:serial_out.txt -no-reboot \
  -accel tcg,one-insn-per-tb=on -d exec,cpu_reset -D exec.txt
```

`exec.txt` grows fast (hundreds of MB in seconds) — let it run briefly, quit,
then `tail` it or search for the guest PC leaving the kernel's address range
(`0x00100000`–`~0x0011xxxx`). Prefer lldb (§4) unless you specifically need the
full stream.

---

## 4. Source-level debugging with lldb

QEMU exposes a gdb stub; lldb attaches to it. This is the most powerful
option — real breakpoints, stepping, registers, and disassembly against the
kernel's symbols.

**Terminal 1** — start QEMU paused (`-S`) with the stub on a port (`-gdb`):

```sh
qemu-system-i386 -kernel kernel.elf -initrd initrd/initrd.tar \
  -display none -serial file:serial_out.txt -no-reboot \
  -S -gdb tcp::1234
```

**Terminal 2** — attach lldb with the ELF loaded for symbols:

```sh
lldb kernel.elf
(lldb) gdb-remote localhost:1234
(lldb) breakpoint set --name kmain
(lldb) continue
```

Useful commands:

| Goal | lldb |
| --- | --- |
| Break at a function | `b kbd_init` |
| Break at an address | `b -a 0x00101f2e` |
| Continue / step one instruction | `c` / `si` |
| Registers | `register read` (or `register read eip esp ebp`) |
| Disassemble here | `disassemble --pc` / `di -s 0x101f24 -c 12` |
| Read memory | `x/8xw 0x108968` |
| Backtrace | `bt` |

### The bisect-with-breakpoints technique

You don't have to single-step through everything. To localize a crash, drop a
few breakpoints across a suspect region and continue between them; the last one
that *hits* before QEMU exits brackets the faulting instruction. This is how
the boot-loop crash was pinned to a single `outb`:

```
(lldb) b -a 0x100193   # after kmalloc  -> hit
(lldb) b -a 0x1001b0   # after memset   -> hit
(lldb) b -a 0x1001c2   # after outb     -> never hit; process exits here
```

Then single-step that one instruction to see exactly what it does.

---

## 5. Mapping addresses to code

Any address from a panic, a QEMU log, or lldb can be turned back into
source:

```sh
i686-elf-addr2line -f -e kernel.elf 0x00102045    # -> function + file:line
i686-elf-nm -n kernel.elf | less                  # all symbols, address order
i686-elf-objdump -d kernel.elf | less             # full disassembly w/ addresses
```

To see the disassembly of one function (e.g. to find the address of a specific
line to break on):

```sh
i686-elf-objdump -d kernel.elf | awk '/<kbd_init>:/{f=1} f{print} /ret/{if(f)exit}'
```

`i686-elf-nm -n` (numeric sort) is the same data the in-kernel resolver uses, so
if a live backtrace disagrees with `nm`, your initrd symtable is stale (§2).

---

## 6. Reference

### Exception vectors (`v=` in QEMU logs, IDT entry in `kernel/idt.c`)

| Vector | Meaning | Vector | Meaning |
| --- | --- | --- | --- |
| `0x00` | Divide error | `0x0d` | General protection |
| `0x03` | Breakpoint | `0x0e` | Page fault |
| `0x06` | Invalid opcode | `0x20` | IRQ0 — timer (PIT) |
| `0x08` | Double fault | `0x21` | IRQ1 — keyboard |
| `0x0c` | Stack fault | `0x27` | IRQ7 — spurious |

A page fault pushes an error code and puts the faulting address in `CR2`
(the panic message and `isr_exc_PF` already print both).

### Interactive input when running headless

Normal interactive use: just type into the QEMU window `make run` opens (real
keystrokes reach the kernel as PS/2 IRQ1). For a **headless** run
(`-display none`), inject keys through the QEMU monitor instead:

```sh
# add to the qemu command:  -monitor tcp:127.0.0.1:4444,server,nowait
printf 'sendkey l\nsendkey s\nsendkey ret\n' | nc 127.0.0.1 4444
```

The shell reads the PS/2 keyboard (`kbd_getc`), **not** serial, so pasting into
the serial terminal does nothing — use `sendkey` or the QEMU window.

### Handy one-liners

```sh
# Boot, capture serial, stop after it settles, show the tail:
qemu-system-i386 -kernel kernel.elf -initrd initrd/initrd.tar \
  -display none -serial file:serial_out.txt -no-reboot & \
  sleep 2; pkill -f qemu-system-i386; tail -20 serial_out.txt

# Did we triple-fault? (no fault vector before the reset == yes)
grep -E 'v=0e|v=0d|v=08|CPU Reset' qlog.txt
```

macOS has no `timeout`; background QEMU and `pkill -f qemu-system-i386`, or quit
it with Ctrl-A X.

---

## 7. Gotchas specific to this kernel

- **Low memory is identity-mapped, including page 0.** After
  `mm_paging_init`, addresses `0`..`heap_end` are present, so a NULL or
  wild-low-pointer dereference *reads/writes garbage silently* instead of
  page-faulting. Don't expect NULL derefs to trap; they corrupt instead.
- **The 8042 command port can reset the CPU.** Writing a "pulse output lines"
  command (`0xF0`–`0xFF`) to port `0x64` can pulse the CPU reset line — this
  presents as a boot loop with *no* fault vector in the logs. Keyboard *device*
  commands (like `0xF4`, enable scanning) go to the data port `0x60`.
- **Interrupts are enabled early** (`sti` right after PIT setup in `kmain`),
  before tasking exists. Anything an IRQ handler touches must be valid by then;
  `task_switch` guards against a NULL `current_task` for this reason.
- **Tasking is currently disabled.** `tasking_init()` is commented out in
  `kmain` because `move_stack()` captures `esp`/`ebp` that modern GCC
  invalidates around an intervening `memcpy`, so it returns to a garbage
  address. Consequently `current_task` is NULL and panic's `PID`/`Name` show
  `0`/empty — expected until `move_stack` is rewritten in assembly.
- **The build is stock i686-elf-gcc with no `-fpermissive`.** If a change
  reintroduces an implicit declaration or incompatible-pointer error, fix the
  types/includes rather than re-adding `-fpermissive` — it hides real bugs.
