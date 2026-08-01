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

int elf_exec(const char *path)
{
    // Copy the path out of the caller's address space now: once we activate the
    // new page directory below, the caller's user memory is no longer mapped.
    char kpath[128];
    strncpy(kpath, path, sizeof(kpath) - 1);
    kpath[sizeof(kpath) - 1] = '\0';

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

    asm volatile("mov %0, %%cr3" :: "r"(old_cr3));
    asm volatile("sti");

    uint32_t entry = eh->e_entry;
    kfree(buf);

    mprintf(LOGLEVEL_DEFAULT, "elf_exec: '%s' loaded, entry 0x%08x\n", kpath, entry);
    return spawn_task(kpath, (void *)entry, dir);
}
