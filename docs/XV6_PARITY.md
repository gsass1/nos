# xv6 x86 feature-parity gaps

This audit compares NOS at `335a84c` with the final MIT `xv6-public` x86 tree at
`eeb7b415dbcb12cc362d0783e41c3d1f44066b17`. It treats a feature as present only
when the current source implements it; comments, unused interfaces, and the
outdated parts of `ARCHITECTURE.md` are not evidence.

“Parity” here means the observable kernel, syscall, filesystem, process,
userland, and machine capabilities shipped by xv6. It does not require NOS to
copy xv6's internal structure when NOS already provides equivalent behavior.
NOS-only facilities such as the framebuffer, mouse, desktop, console channels,
exit statuses, and hostile-pointer checks are outside the comparison.

NOS already has the following xv6-level foundations: ring-3 ELF programs,
per-process page tables and kernel stacks, timer preemption, fault isolation,
arguments at process startup, file descriptors for console/input/initrd files,
blocking pipes with EOF, `read`/`write`/`close`, heap growth, sleep and uptime,
and process termination. The remaining gaps are below.

The xv6 side was inventoried from `syscall.h`, `sysproc.c`, `sysfile.c`,
`proc.c`, `exec.c`, `file.c`, `fs.c`, `bio.c`, `log.c`, `ide.c`, `pipe.c`,
`console.c`, `main.c`, `trap.c`, `Makefile`, and the shipped user programs. The
NOS side was checked against `include/syscall.h`, `kernel/syscall.c`,
`kernel/task.c`, `kernel/elf.c`, `kernel/{vfs,initrd,pipe,paging}.c`,
`kernel/main.c`, the drivers, `user/`, `tests/run_tests.py`, and `Makefile`.

## Syscall compatibility summary

| xv6 syscall | NOS status | Missing behavior |
| --- | --- | --- |
| `fork` | Missing | No process can clone its address space, registers, descriptors, or current execution point. |
| `exit` | Present, different | NOS adds a status, but has no parent-owned zombie lifecycle. |
| `wait` | Partial, incompatible | NOS waits for a caller-selected PID and returns its status; xv6 waits for any direct child, returns that child's PID, rejects callers with no children, and performs reaping. |
| `pipe` | Partial | Basic data/EOF behavior exists, but descriptors are not generally inherited or duplicable and blocked operations use busy-yielding rather than sleep/wakeup. |
| `read` | Partial | Files, pipes, and console work; directory streams, canonical console reads, serial input, kill-interruptible blocking, and shared open-file offsets do not. |
| `kill` | Partial, incompatible | NOS removes another runnable task immediately; xv6 marks it killed, wakes it if sleeping, and exits it at a safe user/kernel boundary. |
| `exec` | Partial, incompatible | NOS spawns a new task and returns its PID; xv6 replaces the calling process and preserves its PID, parent, cwd, and all open descriptors. |
| `fstat` | Missing | No user-visible file type, inode number, link count, or size metadata API. |
| `chdir` | Missing | No current working directory or relative hierarchical path resolution. |
| `dup` | Missing | No general descriptor duplication or shared open-file description. |
| `getpid` | Missing from ABI | The kernel has an internal helper, but userspace cannot call it. |
| `sbrk` | Partial | Positive growth works; shrinking is ignored, and physical-memory exhaustion panics instead of returning `-1`. |
| `sleep` | Present, different | NOS takes milliseconds and repeatedly yields; xv6 takes clock ticks and blocks on a wait channel. |
| `uptime` | Present, different | NOS returns milliseconds; xv6 returns clock ticks. |
| `open` | Partial, incompatible | NOS accepts no mode, opens existing flat initrd entries read-only, and cannot create files. |
| `write` | Partial | Console and pipes work; regular-file and device-file writes do not. |
| `mknod` | Missing | No filesystem device nodes or major/minor dispatch. |
| `unlink` | Missing | Files and directories cannot be removed. |
| `link` | Missing | No hard links or inode link counts. |
| `mkdir` | Missing | Directories cannot be created. |
| `close` | Present, partial model | Closing works, but only pipe objects are reference-counted and shared. |

## Process and execution model

1. **`fork` semantics are absent.** NOS can only create a fresh program with
   `exec`/`exec2`; it cannot run a child from the parent's next instruction or
   copy the parent's memory and register state. This prevents xv6's normal
   fork/exec programming model and programs such as `forktest` and `zombie`.

2. **`exec` does not replace the caller.** `elf_exec` always allocates a new
   task and returns a PID, so successful execution returns to the old image.
   xv6 keeps the same process identity and commits a new image only after the
   load succeeds.

3. **There is no parent/child relationship.** `struct task` has no parent
   pointer, so the kernel cannot constrain waiting to children, notify a parent
   on exit, or represent a process tree.

4. **There is no user-space `init` process.** The kernel directly launches `sh`;
   there is no PID 1 that creates the console, restarts the shell, adopts
   orphans, and reaps abandoned children.

5. **Orphan reparenting is absent.** Because there are no parents, children of
   a terminated task are not adopted by `init`.

6. **Zombie ownership differs.** NOS frees dead tasks from the kernel idle loop
   regardless of whether a parent has observed them. xv6 retains a zombie until
   its parent calls `wait`.

7. **Exit records are lossy and globally readable.** NOS keeps only 32 recent
   `(pid, status)` records, returns `0` once one is overwritten, and allows any
   process to wait on any PID. xv6 keeps each child zombie until its own parent
   reaps it.

8. **`wait` has incompatible results and errors.** NOS takes a PID, can return
   `0` for a nonexistent or forgotten PID, and returns an exit status. xv6 takes
   no PID, returns a reaped child PID, and returns `-1` when there are no
   children or the caller is killed.

9. **The complete descriptor table is not inherited.** `spawn_task` copies only
   descriptors 0–2. xv6 `fork` duplicates every open descriptor and `exec`
   preserves the resulting table.

10. **`getpid` is not exposed to user programs.** The internal `getpid()` used
    for diagnostics has no syscall number or `ulib` wrapper.

11. **Argument validation is not xv6-equivalent.** NOS silently truncates at 16
    arguments, at 1024 bytes, or at the first invalid pointer. xv6 accepts up to
    its 32-entry `MAXARG` vector and fails the whole `exec` for a malformed or
    oversized vector.

12. **Resource exhaustion is not recoverable in all process paths.** Frame
    allocation panics when RAM is exhausted, including during `sbrk` and program
    loading, where xv6 propagates an allocation failure to the syscall.

## Scheduling, blocking, and concurrency

13. **There are no sleeping process states or wait channels.** NOS keeps every
    waiting task on the ready queue and calls `task_switch` in loops for console
    input, pipes, sleep, and wait. xv6 removes blocked processes from runnable
    scheduling and wakes them on a channel.

14. **Blocking calls are not kill-interruptible in the xv6 sense.** xv6 checks
    `killed` while waiting in pipes, console reads, sleep, and wait. NOS has no
    persistent killed flag because `task_kill` tears a target down immediately.

15. **Deferred safe-point killing is absent.** xv6 lets a process finish the
    current protected kernel section before exiting at a trap/syscall boundary.
    NOS unlinks a non-current target and closes its descriptors synchronously,
    then queues its stack and address space for the kernel reaper.

16. **SMP is absent.** NOS runs one processor; xv6 discovers up to eight CPUs,
    starts application processors, and runs one scheduler per CPU.

17. **SMP-safe spinlocks are absent.** NOS mutexes and pipe/console critical
    sections mask local interrupts, which is sufficient only on one CPU. xv6
    provides atomic spinlocks with interrupt nesting and ownership checks.

18. **Sleep locks are absent.** xv6 has locks that block a process while inode
    or disk work is in progress; NOS has no equivalent blocking lock.

19. **Per-CPU kernel state is absent.** NOS has no per-CPU scheduler context,
    GDT/TSS, current-process pointer, interrupt-disable nesting, or panic
    coordination.

20. **Interactive process diagnostics are absent.** xv6's `Ctrl-P` prints every
    process with PID, state, name, and sleeping stack PCs. NOS only has an
    internal current-task diagnostic helper.

## File descriptors and filesystem

21. **There is no writable persistent filesystem.** NOS exposes the boot-time
    tar archive directly from RAM and never writes it back. xv6 uses an IDE disk
    filesystem whose changes survive process exit and normal reboot of the same
    image.

22. **The namespace is flat.** NOS looks up an entire string among root initrd
    entries. It has no slash traversal, nested directories, absolute versus
    relative paths, `.`/`..`, or per-process current directory.

23. **Runtime file creation is absent.** `open` has no `O_CREATE`, `O_WRONLY`,
    or `O_RDWR`, and the VFS has no implemented create operation.

24. **Regular files cannot be modified.** `FD_FILE` is read-only and
    `sys_write` rejects it; there is no extension, block allocation, partial
    overwrite, or truncation behavior.

25. **Directories cannot be modified.** There is no `mkdir`, directory-link
    insertion, empty-directory check, or directory removal.

26. **Hard links are absent.** NOS has no `link`, persistent inode identity, or
    link-count lifecycle.

27. **Unlink-while-open semantics are absent.** xv6 can remove a directory entry
    while an open file remains usable until its last reference closes. NOS has
    neither unlink nor reference-counted regular open files.

28. **Filesystem metadata is not exposed.** There is no `struct stat`, `fstat`,
    or `stat` helper reporting file/device/directory type, inode, link count, or
    size.

29. **Directories are not readable through ordinary descriptors.** xv6
    represents a directory as an inode stream of `dirent` records. NOS uses a
    root-only indexed `readdir` syscall that returns names without metadata.

30. **Device nodes and the device switch are absent.** The synthetic `dev`
    node has no children or operations. There is no `mknod`, major/minor number,
    or inode-backed dispatch to console and other devices.

31. **There is no global open-file table.** xv6 uses reference-counted open-file
    descriptions shared by descriptors and processes. NOS stores file state
    inline in each task and reference-counts only pipe ends.

32. **Shared file offsets are absent.** A duplicated or inherited xv6 descriptor
    refers to the same open-file offset. NOS cannot duplicate regular-file
    descriptors, and the copies used for stdio contain independent offset
    values.

33. **`dup` and descriptor-based redirection are absent.** Closed descriptors
    0–2 are not reused by `open` or `pipe`, which only search slots 3 and above.
    NOS requires the special `exec2` mapping and cannot express xv6's general
    close/open/dup pattern.

34. **Descriptor capacity is lower.** NOS has eight slots per task, with only
    five available to `open` and `pipe`; xv6 has 16 generally allocatable slots.

35. **Open access modes are absent.** NOS file objects do not enforce general
    readable/writable flags, so it cannot represent xv6's read-only,
    write-only, and read/write opens.

36. **The inode layer is absent.** There is no on-disk inode allocation/cache,
    locking, direct and indirect block mapping, device identity, or delayed
    deletion.

37. **The free-block bitmap and superblock are absent.** NOS has no on-disk
    space allocator or filesystem geometry.

38. **The disk block cache is absent.** xv6's shared, locked buffer cache and
    buffer replacement have no NOS equivalent.

39. **Filesystem transactions and crash recovery are absent.** xv6 wraps
    mutating operations in a write-ahead log and recovers committed operations
    on boot; NOS has no mutable storage to journal.

40. **The IDE disk driver is absent.** NOS does not probe, queue, interrupt, read,
    or write an ATA disk.

41. **The `mkfs` host tool and filesystem image are absent.** NOS packs a ustar
    initrd and cannot construct or inspect an xv6-style mutable disk image.

## Console and device behavior

42. **Canonical console input is absent.** xv6 buffers and echoes complete
    lines, supports backspace/Delete, `Ctrl-U` line kill, and `Ctrl-D` EOF in
    the console device. NOS gives the shell raw characters and implements only
    shell-local backspace.

43. **Console reads cannot return EOF.** NOS `read`/`getc` waits for a character;
    there is no `Ctrl-D` path that produces a zero-byte read.

44. **Serial console input is not connected.** NOS initializes COM1 and has a
    polling `serial_read` helper, but no IRQ handler feeds serial bytes into
    stdin. xv6 accepts both keyboard and UART input through the console line
    discipline.

45. **The console is not an openable filesystem object.** xv6 `init` creates a
    `console` device inode and opens/duplicates it as descriptors 0–2. NOS
    installs special descriptor types directly when spawning a task.

## User API and programs

46. **The usual xv6 user C support library is incomplete.** NOS lacks compatible
    `printf`, `gets`, `stat`, `strcpy`, `strcmp`, `strlen`, `strchr`, `memset`,
    `memmove`, `atoi`, `malloc`, and `free` interfaces. Its small header offers
    narrower helpers under different names.

47. **There is no reusable user heap allocator.** Programs can call `sbrk`
    directly, but there is no `malloc`/`free` implementation with free-list
    reuse and coalescing.

48. **The user-program entry convention differs.** xv6 programs expose
    `main(argc, argv)` and link with the common user library; NOS programs expose
    `_start` and use a header-only ABI. xv6 user sources therefore cannot be
    rebuilt unchanged.

49. **The shell lacks output redirection.** `<` is supported, but `>` and `>>`
    cannot work because regular files cannot be created or written.

50. **The shell lacks command lists.** There is no `;` sequencing operator.

51. **The shell lacks background jobs.** There is no `&` operator; the
    spawn-based shell always waits for every foreground pipeline PID.

52. **The shell lacks grouped commands.** Parenthesized command trees and
    redirection of a group are not parsed.

53. **The shell has a fixed four-stage pipeline limit.** xv6's recursive parser
    is limited by processes, descriptors, and memory rather than an explicit
    four-stage array.

54. **The shell has no `cd`.** This follows from the missing cwd and `chdir`
    syscall, and prevents persistent directory changes in the shell process.

55. **Shell tokenization is narrower.** NOS splits only on spaces and requires
    `<` to be a separate word; xv6 recognizes tabs and other whitespace and
    tokenizes operators without surrounding spaces.

56. **`cat` lacks xv6's stdin/filter mode.** With no filename NOS prints usage;
    xv6 reads descriptor 0, which makes `producer | cat` and interactive input
    work.

57. **The standalone `ls` behavior is absent.** NOS has a root-only shell
    builtin that prints names; xv6 `ls` accepts paths and reports type, inode,
    and size for files and directory entries.

58. **The standard xv6 utilities are missing.** NOS does not ship `echo`, `grep`
    (including xv6's `^`, `.`, `*`, and `$` matcher), `kill`, `ln`, `mkdir`,
    `rm`, or `wc`.

59. **The xv6 process/filesystem exercisers are missing.** NOS does not ship
    `forktest`, `stressfs`, `usertests`, or `zombie`. NOS has its own integration
    tests, but they do not provide these user-visible stress programs or their
    coverage of fork, inode, link, directory, shared-fd, and disk behavior.

## Boot, interrupt, and machine support

60. **NOS is not a self-contained BIOS disk boot.** xv6 builds a signed
    512-byte boot sector that loads the kernel from its own disk image. NOS
    requires a Multiboot-aware loader or QEMU's multiboot `-kernel` path plus a
    separately supplied initrd.

61. **MP-table discovery is absent.** NOS does not discover processor and I/O
    topology from Intel MP tables.

62. **Local APIC support is absent.** There is no LAPIC initialization, timer,
    end-of-interrupt handling, CPU identification, or application-processor
    startup.

63. **I/O APIC routing is absent.** NOS uses the legacy 8259 PIC and cannot route
    device interrupts to CPUs as xv6 does.

64. **The complete xv6 IRQ set is absent.** NOS handles its PIT, keyboard,
    mouse, and legacy spurious IRQs, but lacks xv6's IDE, UART receive, LAPIC
    timer, and APIC spurious interrupt paths.

65. **There is no in-memory-disk kernel variant.** xv6 can build
    `kernelmemfs`/`xv6memfs.img` to run the filesystem without IDE hardware; NOS
    has only its immutable initrd model.

66. **Equivalent build targets are missing.** NOS documents manual QEMU debugger
    commands, but has no xv6-style `qemu-nox`, `qemu-gdb`, `qemu-nox-gdb`, or
    generated debugger-init targets.

## What would establish parity

The dependencies make the order fairly strict:

1. Add parented processes, `fork`, replace-in-place `exec`, child-owned zombies,
   xv6-style `wait`, all-fd inheritance, and a user `init`.
2. Replace inline descriptor state with reference-counted open-file
   descriptions, then add `dup`, `getpid`, and `fstat`.
3. Add a writable hierarchical inode filesystem, block cache, IDE path, and
   write-ahead log; expose the remaining pathname and mutation syscalls.
4. Add real sleep/wakeup states and interruptible blocking before attempting
   SMP, then add MP/APIC discovery, per-CPU state, and SMP locks.
5. Fill out the user library, shell grammar, utilities, and xv6 user tests.

Completing only the syscall names would not provide parity: xv6's process model,
shared open-file objects, persistent inode namespace, sleep/wakeup discipline,
and SMP-safe locking are the behaviors those calls depend on.
