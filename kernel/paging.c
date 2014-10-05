#include <debug.h>
#include <kernel.h>
#include <mm.h>
#include <string.h>

MODULE("PAGE");

int paging_enabled = 0;

uint32_t *frames;
uint32_t nframes;

// From kmalloc.c
extern uint32_t heap;
extern uint32_t heap_end;

struct page_directory *kernel_directory;
struct page_directory *current_directory;

// Macros used in the bitset algorithms.
#define INDEX_FROM_BIT(a) (a/(8*4))
#define OFFSET_FROM_BIT(a) (a%(8*4))

extern void copy_page_physical(uint32_t src, uint32_t dest);

static void set_frame(uint32_t frame_addr)
{
    uint32_t frame = frame_addr / 0x1000;
    uint32_t idx = INDEX_FROM_BIT(frame);
    uint32_t off = OFFSET_FROM_BIT(frame);
    frames[idx] |= (0x1 << off);
}

static void clear_frame(uint32_t frame_addr)
{
    uint32_t frame = frame_addr / 0x1000;
    uint32_t idx = INDEX_FROM_BIT(frame);
    uint32_t off = OFFSET_FROM_BIT(frame);
    frames[idx] &= ~(0x1 << off);
}

static uint32_t test_frame(uint32_t frame_addr)
{
    uint32_t frame = frame_addr / 0x1000;
    uint32_t idx = INDEX_FROM_BIT(frame);
    uint32_t off = OFFSET_FROM_BIT(frame);
    return (frames[idx] & (0x1 << off));
}

static uint32_t first_frame(void)
{
    uint32_t i, j;
    for(i = 0; i < INDEX_FROM_BIT(nframes); i++) {
        if(frames[i] != 0xFFFFFFFF) {
            for(j = 0; j < 32; j++) {
                uint32_t to_test = 0x1 << j;
                if(!(frames[i] & to_test)) {
                    return i * 4 * 8 + j;
                }
            }
        }
    }
    return 0;
}

void alloc_frame(struct page *page, int is_kernel, int is_writeable)
{
    if(page->frame != 0) {
        return;
    } else {
        uint32_t idx = first_frame();
        if(idx == (uint32_t) - 1) {
            panic("No free frames");
        }
        set_frame(idx * 0x1000);
        page->present = 1;
        page->rw = (is_writeable) ? 1 : 0;
        page->user = (is_kernel) ? 0 : 1;
        page->frame = idx;
    }
}

void free_frame(struct page *page)
{
    uint32_t frame;
    if(!(frame = page->frame)) {
        return;
    } else {
        clear_frame(frame);
        page->frame = 0x0;
    }
}

void mm_paging_init(void)
{
    mprintf(LOGLEVEL_DEFAULT, "Initializing Paging\n");

    // The size of physical memory. For the moment we
    // assume it is 16MB big.
    uint32_t mem_end_page = 0x1000000;

    nframes = mem_end_page / 0x1000;
    frames = (uint32_t *)kmalloc(INDEX_FROM_BIT(nframes));
    memset(frames, 0, INDEX_FROM_BIT(nframes));

    kernel_directory = kmalloc_a(sizeof(struct page_directory));
    memset(kernel_directory, 0, sizeof(struct page_directory));

    uint32_t i = 0;
    for (; i < heap_end; i += 0x1000)
        get_page(i, 1, kernel_directory);

    // Identity map memory range to heap space
    i = 0;
    while(i < heap_end + 0x1000) {
        alloc_frame(get_page(i, 1, kernel_directory), 0, 0);
        i += 0x1000;
    }

    /*// Now allocate those pages we mapped earlier.
    for (i = KHEAP_START; i < KHEAP_START+KHEAP_INITIAL_SIZE; i += 0x1000)
        alloc_frame( get_page(i, 1, kernel_directory), 0, 0);*/

    i = 0;
    for (; i < heap_end; i += 0x1000)
        alloc_frame(get_page(i, 1, kernel_directory), 0, 0);

    switch_page_directory(kernel_directory);

    paging_enabled = 1;

    current_directory = clone_directory(kernel_directory);
    switch_page_directory(current_directory);
}

void switch_page_directory(struct page_directory *dir)
{
    current_directory = dir;
    asm volatile("mov %0, %%cr3":: "r"(dir->tables_physical));
    uint32_t cr0;
    asm volatile("mov %%cr0, %0": "=r"(cr0));
    cr0 |= 0x80000000; // Enable paging!
    asm volatile("mov %0, %%cr0":: "r"(cr0));
}

struct page *get_page(uint32_t addr, int make, struct page_directory *dir)
{
    addr /= 0x1000;
    uint32_t table_idx = addr / 1024;
    if(dir->tables[table_idx]) {
        return &dir->tables[table_idx]->pages[addr % 1024];
    } else if(make) {
        uint32_t tmp;
        dir->tables[table_idx] = kmalloc_ap(sizeof(struct page_table), &tmp);
        memset(dir->tables[table_idx], 0, 0x1000);
        dir->tables_physical[table_idx] = tmp | 0x7; // PRESENT, RW, US
        return &dir->tables[table_idx]->pages[addr % 1024];
    } else {
        return 0;
    }
}

struct page_directory *clone_directory(struct page_directory *src)
{
    uint32_t phys;

    struct page_directory *dir = kmalloc_ap(sizeof(struct page_directory), &phys);

    memset(dir, 0, sizeof(struct page_directory));

    uint32_t offset = (uint32_t)dir->tables_physical - (uint32_t)dir;
    dir->phys_addr = phys + offset;
    int i;

    for(i = 0; i < 1024; i++) {
        if(!src->tables[i]) {
            continue;
        }
        if(kernel_directory->tables[i] == src->tables[i]) {
            dir->tables[i] = src->tables[i];
            dir->tables_physical[i] = src->tables_physical[i];
        } else {
            uint32_t phys;
            dir->tables[i] = clone_table(src->tables[i], &phys);
            dir->tables_physical[i] = phys | 0x07;
        }
    }
    return dir;
}

struct page_table *clone_table(struct page_table *src, uint32_t *phys_addr)
{
    struct page_table *table = kmalloc_ap(sizeof(struct page_table), phys_addr);

    memset(table, 0, sizeof(struct page_table));

    int i;
    for(i = 0; i < 1024; i++) {
        if(!src->pages[i].frame) {
            continue;
        }
        alloc_frame(&table->pages[i], 0, 0);

        if(src->pages[i].present) {
            table->pages[i].present = 1;
        }
        if(src->pages[i].rw) {
            table->pages[i].rw = 1;
        }
        if(src->pages[i].user) {
            table->pages[i].user = 1;
        }
        if(src->pages[i].accessed) {
            table->pages[i].accessed = 1;
        }
        if(src->pages[i].dirty) {
            table->pages[i].dirty = 1;
        }
        copy_page_physical(src->pages[i].frame * 0x1000, table->pages[i].frame * 0x1000);
    }
    return table;
}