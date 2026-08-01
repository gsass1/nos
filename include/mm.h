#ifndef __MM_H__
#define __MM_H__

#include <stdint.h>

struct page
{
   uint32_t present    : 1;   // Page present in memory
   uint32_t rw         : 1;   // Read-only if clear, readwrite if set
   uint32_t user       : 1;   // Supervisor level only if clear
   uint32_t accessed   : 1;   // Has the page been accessed since last refresh?
   uint32_t dirty      : 1;   // Has the page been written to since last refresh?
   uint32_t unused     : 7;   // Amalgamation of unused and reserved bits
   uint32_t frame      : 20;  // Frame address (shifted right 12 bits)
};

struct page_table
{
    struct page pages[1024];
};

struct page_directory
{
    struct page_table *tables[1024];
    uint32_t tables_physical[1024];
    uint32_t phys_addr;
};

extern struct page_directory *current_directory;

void alloc_frame(struct page *page, int is_kernel, int is_writeable);

// reserved_end: first byte past bootloader-placed data (kernel image, initrd
// module) that the heap must not overlap. The heap starts page-aligned above
// max(kernel_end, reserved_end).
void heap_init(uint32_t reserved_end);

// mem_size is the total physical memory in bytes (sizes the frame allocator).
void mm_paging_init(uint32_t mem_size);

void switch_page_directory(struct page_directory *new);

struct page *get_page(uint32_t addr, int make, struct page_directory *dir);

// Validate a user-supplied pointer before the kernel touches it: nonzero iff
// [p, p+len) lies inside the user window and every page is present and
// user-accessible (and writable, when write is set) in the current address
// space. After a successful check the kernel may use the range in place --
// nothing ever unmaps a live task's user pages while that task is executing
// a syscall (sbrk only adds; exit/reap only run once the task is done).
int user_ok(const void *p, uint32_t len, int write);

// Same for a NUL-terminated user string of at most max bytes including the
// NUL: nonzero iff every byte up to and including the NUL is user-readable.
int user_str_ok(const char *s, uint32_t max);

struct page_table *clone_table(struct page_table *src, uint32_t *phys_addr);

struct page_directory *clone_directory(struct page_directory *src);

// Free a cloned address space (frames + user page tables + the directory).
void free_directory(struct page_directory *dir);

void free_frame(struct page *page);

// Page aligned
void *kmalloc_a(uint32_t size);

void kfree(void *ptr);

// Returns a physical address
void *kmalloc_p(uint32_t size, uint32_t *phys);

// Page aligned and returns a physical address
void *kmalloc_ap(uint32_t size, uint32_t *phys);

void *kmalloc(uint32_t size);

#endif