#include <elf.h>
#include <kernel.h>
#include <mm.h>
#include <string.h>
#include <task.h>
#include <vfs.h>

MODULE("ELF ");

static int elf_is_valid(struct elf32_ehdr *eh)
{
    return eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' &&
           eh->e_ident[2] == 'L'  && eh->e_ident[3] == 'F';
}

int elf_exec(const char *path)
{
    struct fs_node *node = vfs_finddir(fs_root, (char *)path);
    if (!node) {
        mprintf(LOGLEVEL_DEFAULT, "elf_exec: '%s' not found\n", path);
        return -1;
    }

    uint8_t *buf = kmalloc(node->length);
    uint32_t got = vfs_read(node, 0, node->length, buf);
    if (got < sizeof(struct elf32_ehdr)) {
        mprintf(LOGLEVEL_DEFAULT, "elf_exec: '%s' too small to be an ELF\n", path);
        kfree(buf);
        return -1;
    }

    struct elf32_ehdr *eh = (struct elf32_ehdr *)buf;
    if (!elf_is_valid(eh)) {
        mprintf(LOGLEVEL_DEFAULT, "elf_exec: '%s' is not an ELF\n", path);
        kfree(buf);
        return -1;
    }

    // Map every page each PT_LOAD segment spans, user-accessible and writable
    // (so this same loader works once we start entering these in ring 3).
    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        struct elf32_phdr *ph =
            (struct elf32_phdr *)(buf + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        uint32_t end = ph->p_vaddr + ph->p_memsz;
        for (uint32_t a = ph->p_vaddr & ~0xFFFU; a < end; a += 0x1000) {
            alloc_frame(get_page(a, 1, current_directory),
                        0 /* user-accessible */, 1 /* writable */);
        }
    }

    // Flush the TLB so writes below land on the mappings we just created.
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" :: "r"(cr3));

    // Copy each segment's file image into place and zero the trailing .bss.
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

    uint32_t entry = eh->e_entry;
    kfree(buf);

    mprintf(LOGLEVEL_DEFAULT, "elf_exec: '%s' loaded, entry 0x%08x\n", path, entry);
    return spawn_task(path, (void *)entry);
}
