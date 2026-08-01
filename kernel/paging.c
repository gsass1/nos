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
    // No free frame found. Return -1 so alloc_frame's check fires instead of
    // ambiguously reporting frame 0 as free.
    return (uint32_t)-1;
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
        // MMIO mappings (e.g. the framebuffer) point beyond RAM: they are not
        // in the allocator bitmap, and clear_frame on them would write far
        // past it into the heap. Just unmap those.
        if(frame < nframes) {
            // clear_frame expects a physical address, not a frame index.
            clear_frame(frame * 0x1000);
        }
        page->frame = 0x0;
        page->present = 0;
    }
}

// Free an address space created by clone_directory(): release the frames and
// page tables it owns, but leave the tables it shares with the kernel directory
// alone. The directory must not be the active cr3 when this is called.
void free_directory(struct page_directory *dir)
{
    if(!dir) {
        return;
    }

    for(int i = 0; i < 1024; i++) {
        if(!dir->tables[i]) {
            continue;
        }
        // Shared kernel table: not ours to free.
        if(kernel_directory->tables[i] == dir->tables[i]) {
            continue;
        }

        struct page_table *table = dir->tables[i];
        for(int j = 0; j < 1024; j++) {
            if(table->pages[j].frame) {
                free_frame(&table->pages[j]);
            }
        }
        kfree(table);
    }

    kfree(dir);
}

void mm_paging_init(uint32_t mem_size)
{
    mprintf(LOGLEVEL_DEFAULT, "Initializing Paging (%d MB physical)\n",
            mem_size / (1024 * 1024));

    nframes = mem_size / 0x1000;

    // The bitset needs one bit per frame. INDEX_FROM_BIT(nframes) is the number
    // of uint32_t words, so the byte size we must hand kmalloc is that count
    // times sizeof(uint32_t). Passing the word count directly (as before) under-
    // allocated the bitset 4x and let set_frame() scribble past it into the heap.
    uint32_t frames_bytes = INDEX_FROM_BIT(nframes) * sizeof(uint32_t);
    frames = (uint32_t *)kmalloc(frames_bytes);
    memset(frames, 0, frames_bytes);

    kernel_directory = kmalloc_a(sizeof(struct page_directory));
    memset(kernel_directory, 0, sizeof(struct page_directory));

    uint32_t i = 0;
    for (; i < heap_end; i += 0x1000)
        get_page(i, 1, kernel_directory);

    // Identity map everything up to the end of the heap, supervisor-only:
    // ring 3 must not be able to read or write kernel code, the heap (which
    // holds every task's kernel stack), or the VGA buffer. User program pages
    // are mapped separately by elf_exec with the user bit set.
    i = 0;
    while(i < heap_end + 0x1000) {
        alloc_frame(get_page(i, 1, kernel_directory), 1, 1);
        i += 0x1000;
    }

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