#ifndef __ELF_H__
#define __ELF_H__

#include <stdint.h>

struct elf32_ehdr
{
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf32_phdr
{
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed));

#define PT_LOAD 1

// The virtual address window user program segments may occupy. Programs link
// at 1GB (user/user.ld); everything below is kernel identity-mapped memory.
#define USER_VADDR_MIN 0x40000000U
#define USER_VADDR_MAX 0x80000000U

// Each process gets its own user-mode stack, mapped by elf_exec just below
// this address (well above where the small program images end).
#define USER_STACK_TOP  0x50000000U
#define USER_STACK_SIZE 0x4000U // 16KB

// The user heap grows from USER_HEAP_BASE via SYS_SBRK, up to at most
// USER_HEAP_MAX. Pages are mapped on demand in the calling task's directory.
#define USER_HEAP_BASE 0x60000000U
#define USER_HEAP_MAX  0x70000000U

// SYS_FBMAP maps the linear framebuffer here in the calling process.
#define USER_FB_BASE   0x78000000U

// Window surfaces (SYS_WCREATE/SYS_WMAP): a client's own surface pixels map
// at USER_WSURF_BASE; a server's view of slot i maps at
// USER_WSRV_BASE + i * WSURF_SLOT_SPAN (2MB per slot covers 800x600x32).
#define USER_WSURF_BASE 0x74000000U
#define USER_WSRV_BASE  0x74400000U
#define WSURF_SLOT_SPAN 0x00200000U

// exec() argument limits: how many argv entries and how many total string
// bytes elf_exec will copy onto the new process's stack.
#define EXEC_MAX_ARGS    16
#define EXEC_ARG_BYTES   1024

struct file;

// Load an ELF32 program from the initrd, mapping its PT_LOAD segments, and
// start it as a ring-3 task. `argv` is a NULL-terminated array of argument
// strings in the CALLER's address space (or NULL for none); they are copied
// onto the new process's stack so its _start receives (int argc, char **argv).
// `stdio` is the fd 0/1/2 triple to install (NULL = the real console); pass
// the parent's own files to inherit, Unix-style.
// Returns the new task id, or -1 on failure.
int elf_exec(const char *path, const char *const *argv,
             const struct file *stdio);

#endif
