#include <elf.h>
#include <kernel.h>
#include <mm.h>
#include <string.h>
#include <task.h>
#include <vfs.h>

MODULE("ELF ");

extern struct page_directory *kernel_directory;

static int elf_is_valid(struct elf32_ehdr *eh)
{
    return eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' &&
           eh->e_ident[2] == 'L'  && eh->e_ident[3] == 'F';
}

// True if p can be a pointer into the calling process's user memory. Kernel
// callers (kmain) pass argv = NULL, so argv entries must always be user
// pointers; rejecting everything else keeps a bogus argv array from making
// the kernel dereference wild addresses on the caller's behalf.
static int is_user_ptr(const void *p)
{
    return (uint32_t)p >= USER_VADDR_MIN && (uint32_t)p < USER_VADDR_MAX;
}

int elf_exec(const char *path, const char *const *argv,
             const struct file *stdio)
{
    // Copy the path out of the caller's address space now: once we activate the
    // new page directory below, the caller's user memory is no longer mapped.
    char kpath[128];
    strncpy(kpath, path, sizeof(kpath) - 1);
    kpath[sizeof(kpath) - 1] = '\0';

    // Same for the argument strings. kargv[] points into argbuf.
    char argbuf[EXEC_ARG_BYTES];
    const char *kargv[EXEC_MAX_ARGS];
    uint32_t argc = 0, argused = 0;
    if (argv && is_user_ptr(argv)) {
        while (argc < EXEC_MAX_ARGS && is_user_ptr(argv[argc])) {
            uint32_t len = strlen(argv[argc]) + 1;
            if (argused + len > sizeof(argbuf)) {
                break;
            }
            memcpy(argbuf + argused, argv[argc], len);
            kargv[argc] = argbuf + argused;
            argused += len;
            argc++;
        }
    }

    struct fs_node *node = vfs_finddir(fs_root, kpath);
    if (!node) {
        mprintf(LOGLEVEL_DEFAULT, "elf_exec: '%s' not found\n", kpath);
        return -1;
    }

    uint8_t *buf = kmalloc(node->length);
    uint32_t got = vfs_read(node, 0, node->length, buf);
    if (got < sizeof(struct elf32_ehdr)) {
        mprintf(LOGLEVEL_DEFAULT, "elf_exec: '%s' too small to be an ELF\n", kpath);
        kfree(buf);
        return -1;
    }

    struct elf32_ehdr *eh = (struct elf32_ehdr *)buf;
    if (!elf_is_valid(eh)) {
        mprintf(LOGLEVEL_DEFAULT, "elf_exec: '%s' is not an ELF\n", kpath);
        kfree(buf);
        return -1;
    }

    // Sanity-check the headers before touching any address space: the program
    // header table must lie inside the file, every PT_LOAD's file data must
    // too, and segments may only target the user region -- otherwise a
    // truncated or malicious binary reads past the buffer or memcpy()s over
    // kernel memory (alloc_frame silently keeps existing kernel mappings).
    if (eh->e_phoff > got ||
        (uint32_t)eh->e_phnum * eh->e_phentsize > got - eh->e_phoff) {
        mprintf(LOGLEVEL_DEFAULT, "elf_exec: '%s' program headers out of bounds\n", kpath);
        kfree(buf);
        return -1;
    }
    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        struct elf32_phdr *ph =
            (struct elf32_phdr *)(buf + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        if (ph->p_offset > got || ph->p_filesz > got - ph->p_offset ||
            ph->p_filesz > ph->p_memsz ||
            ph->p_vaddr < USER_VADDR_MIN ||
            ph->p_memsz > USER_VADDR_MAX - ph->p_vaddr) {
            mprintf(LOGLEVEL_DEFAULT, "elf_exec: '%s' has a bad PT_LOAD segment\n", kpath);
            kfree(buf);
            return -1;
        }
    }

    // Give the program its own address space. Cloning the kernel directory
    // shares every kernel/heap mapping (so the task's kernel stack, the heap,
    // and this loader all stay reachable) while leaving the user region empty,
    // so each program can link at the same address without colliding.
    struct page_directory *dir = clone_directory(kernel_directory);

    // To write the program image to its virtual addresses we must make `dir`
    // active. Do it with interrupts off (so the scheduler doesn't run with a
    // half-built cr3) and restore the previous address space before returning.
    asm volatile("cli");
    uint32_t old_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(old_cr3));
    asm volatile("mov %0, %%cr3" :: "r"(dir->phys_addr));

    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        struct elf32_phdr *ph =
            (struct elf32_phdr *)(buf + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        uint32_t end = ph->p_vaddr + ph->p_memsz;
        for (uint32_t a = ph->p_vaddr & ~0xFFFU; a < end; a += 0x1000) {
            alloc_frame(get_page(a, 1, dir), 0 /* user */, 1 /* writable */);
        }
    }

    // Map the process's ring-3 stack.
    for (uint32_t a = USER_STACK_TOP - USER_STACK_SIZE; a < USER_STACK_TOP;
         a += 0x1000) {
        alloc_frame(get_page(a, 1, dir), 0 /* user */, 1 /* writable */);
    }

    // Flush, then copy each segment's file image and zero the trailing .bss.
    asm volatile("mov %0, %%cr3" :: "r"(dir->phys_addr));
    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        struct elf32_phdr *ph =
            (struct elf32_phdr *)(buf + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        memcpy((void *)ph->p_vaddr, buf + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz) {
            memset((void *)(ph->p_vaddr + ph->p_filesz), 0,
                   ph->p_memsz - ph->p_filesz);
        }
    }

    // Fresh frames hold whatever was in them last; hand the process a clean
    // stack rather than another task's leftovers.
    memset((void *)(USER_STACK_TOP - USER_STACK_SIZE), 0, USER_STACK_SIZE);

    // Lay out the initial stack so _start(int argc, char **argv) works as a
    // plain cdecl function: the argument strings at the very top, then the
    // argv pointer array, then [fake return address][argc][argv].
    uint32_t usp = USER_STACK_TOP;
    uint32_t uargs[EXEC_MAX_ARGS];
    for (uint32_t i = 0; i < argc; i++) {
        uint32_t len = strlen(kargv[i]) + 1;
        usp -= len;
        memcpy((void *)usp, kargv[i], len);
        uargs[i] = usp;
    }
    usp &= ~3U; // word-align below the strings
    usp -= (argc + 1) * 4;
    uint32_t uargv = usp;
    for (uint32_t i = 0; i < argc; i++) {
        ((uint32_t *)uargv)[i] = uargs[i];
    }
    ((uint32_t *)uargv)[argc] = 0;
    usp -= 4; *(uint32_t *)usp = uargv;      // argv
    usp -= 4; *(uint32_t *)usp = argc;       // argc
    usp -= 4; *(uint32_t *)usp = 0;          // fake return address for _start

    asm volatile("mov %0, %%cr3" :: "r"(old_cr3));
    asm volatile("sti");

    uint32_t entry = eh->e_entry;
    kfree(buf);

    mprintf(LOGLEVEL_DEFAULT, "elf_exec: '%s' loaded, entry 0x%08x\n", kpath, entry);
    return spawn_task(kpath, (void *)entry, dir, usp, stdio);
}
