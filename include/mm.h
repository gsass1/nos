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

void heap_init(void);

void mm_paging_init(void);

void switch_page_directory(struct page_directory *new);

struct page *get_page(uint32_t addr, int make, struct page_directory *dir);

struct page_table *clone_table(struct page_table *src, uint32_t *phys_addr);

struct page_directory *clone_directory(struct page_directory *src);

// Page aligned
void *kmalloc_a(uint32_t size);

void kfree(void *ptr);

// Returns a physical address
void *kmalloc_p(uint32_t size, uint32_t *phys);

// Page aligned and returns a physical address
void *kmalloc_ap(uint32_t size, uint32_t *phys);

void *kmalloc(uint32_t size);

#endif