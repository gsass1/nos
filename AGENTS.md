# NOS

A unix-like hobby OS for i386 (JamesM-tutorial lineage, heavily reworked). Current state:
preemptive round-robin tasking, per-process address spaces, ELF loading from an initrd
tar, and a ring-3 userland (shell, cat, hello, crash) that talks to the kernel only via
`int 0x80`. See `docs/ARCHITECTURE.md` for diagrams and memory maps — but note it
currently still describes the older all-ring-0 design; the ring-3/TSS/argv/fd work
landed after it was written and it needs a refresh.

## Build & run

Cross toolchain: `i686-elf-gcc` / `i686-elf-as` / `i686-elf-ld` / `i686-elf-nm` (Homebrew).

```sh
make            # kernel.elf
make initrd     # initrd/initrd.tar (symtable + user programs; ustar format required)
make run        # QEMU with -serial stdio and a display window
make test       # boot/integration tests (tests/run_tests.py) -- run before every commit
```

Freestanding C99, `-Wall -Wextra`. The tree is warning-clean and CI builds with
`make WERROR=1`; do not introduce warnings. CI (`.github/workflows/ci.yml`) runs
build + initrd + `make test` on every PR; a PR is not done until `make test` passes.

## Verifying changes (do this before claiming something works)

`make test` covers boot, ls, exec/wait, argv, file io, sbrk, error paths, fault
isolation, and that the prompt is on the visible VGA screen. Extend
`tests/run_tests.py` when adding user-visible behaviour (it polls the serial log
for markers -- no fixed sleeps -- and can dump the screen via the monitor).

For ad-hoc poking beyond the suite -- headless QEMU, driven through the monitor;
serial output goes to a log file:

```sh
(sleep 3; echo "sendkey l"; sleep 0.3; echo "sendkey s"; sleep 0.3; echo "sendkey ret"; \
 sleep 2; echo quit) | \
qemu-system-i386 -kernel kernel.elf -initrd initrd/initrd.tar \
  -serial file:LOG -display none -monitor stdio
```

- Key names: letters as-is, `ret`, `spc`, `bracket_right`, ... The in-kernel keymap is
  QWERTY (letters + digits only; no shift; `.` and `/` fire on key *release*).
- **The serial log is not the screen.** Everything is written to both, but VGA-only bugs
  (cursor, scrolling, wrapping) are invisible in the log. To inspect the actual screen,
  add `echo "xp /4000bx 0xb8000"` before `quit`, capture monitor stdout, and decode:
  even bytes are characters, 160 bytes per row, 25 rows. Anything "printed" beyond row
  24 is invisible to the user.
- Regression programs to run after touching paging/tasking/syscalls: `ls`, `hello`
  (exec+exit+wait), `cat symtable` (argv, open/read loop, sbrk), `crash` (must print a
  page-fault kill report with error `0x7`, exit status -1, and leave the shell alive).

## Layout

- `boot/boot.S` — multiboot entry; `kernel/main.c` — `kmain`, boot order, idle/reaper loop
- `kernel/` — `task.c` (scheduler, exit/reap), `elf.c` (loader + argv), `syscall.c`
  (dispatcher), `paging.c`/`kmalloc.c` (MM), `gdt.c` (GDT+TSS), `idt.c`, `interrupt.S`
  (all entry stubs), `isr.c` (fault handlers), `initrd.c` (tar VFS), `vga.c`
- `include/` — kernel headers; `include/syscall.h` is the user/kernel ABI contract;
  process-layout constants (user window, stack, heap) live in `include/elf.h`
- `user/` — userland programs; `user/ulib.h` is the header-only libc (syscall wrappers)
- Adding a user program touches the Makefile in three places: `USERPROGS`, a build rule,
  and the tar file list under `$(INITRD)`.

## Invariants & gotchas

- Syscall ABI: `eax` = number, `ebx/ecx/edx` = args, result in `eax`. New syscalls:
  number in `syscall.h`, case in `syscall_dispatch`, wrapper in `user/ulib.h`.
- The kernel identity map (0..heap_end) is supervisor-only; user pages are mapped by
  `elf_exec`/`sys_sbrk` with the user bit. Don't hand out kernel memory to ring 3.
- `interrupt.S`: only vectors 8, 10-14, 17 push a CPU error code. `FAULT_STUB` frames
  (`struct fault_frame` in idt.h) normalize this with a dummy push. In GAS, `;` is a
  statement separator, NOT a comment — use `#`.
- The heap is a fixed 2.4MB with no split/coalesce; it starts after the initrd module
  (`heap_init(initrd_end)`), never assume a fixed gap after `kernel_end`.
- `kmalloc`/`kfree` mask interrupts internally; the scheduler can preempt anywhere else.
- User pointers passed to syscalls are only range-checked for exec argv; a bad pointer
  elsewhere still page-faults in ring 0 and panics (copy_from_user is future work).
- Faults from ring 3 kill the task (`exit(-1)`); faults from ring 0 panic. If a "hang"
  is reported, check whether the shell still answers on serial before assuming a lock.
- `wait` returns the child's exit status from a 32-entry ring buffer; statuses of
  long-dead pids read as 0.
- Pipe ends are the only refcounted fd type: duplicate via `file_addref`, drop via
  `file_close`. `exit()`/`task_kill()` close the dying task's whole fd table -- that,
  not the reaper, is what unblocks a peer waiting on the far end (EOF for readers,
  -1 for writers). Never copy a `struct file` without taking the ref.

## Conventions

- Commit messages: imperative summary line, body explains the why (see `git log`).
- Comments state constraints/invariants, not narration; match the file's existing style
  (kernel C is 4-space, some older files use tabs — follow the file).
- Keep `docs/ARCHITECTURE.md` in sync when the boot flow, memory map, or task model
  changes (it has mermaid diagrams and a source map).
