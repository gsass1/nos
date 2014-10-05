#include <kernel.h>
#include <mm.h>
#include <string.h>

MODULE("HEAP")

extern int paging_enabled;
extern struct page_directory *kernel_directory;

struct alloc
{
    uint32_t size;
    uint32_t status;
};

uint32_t heap;
uint32_t heap_end;

#define POINTER_TO_HEAD(A) ((void *)(((uint32_t)A) - sizeof(struct alloc)))
#define HEAD_TO_POINTER(A) ((void *)(((uint32_t)A) + sizeof(struct alloc)))
#define PAGE_ALIGNED(a) !(((uint32_t)(a)) & 0xFFFFF000)
#define PAGE_ALIGN(a) (a) &= 0xFFFFF000; (a) += 0x1000;

void heap_init(void)
{
    mprintf(LOGLEVEL_DEFAULT, "Initializing Heap\n");
    heap = (uint32_t)&kernel_end + 0xF000;
    if(heap & 0xFFFFF000) {
        heap &= 0xFFFFF000;
        heap += 0x1000;
    }
    heap_end = heap + (0x1000*600);
    memset((void *)heap, 0, heap_end - heap);

    mprintf(LOGLEVEL_DEFAULT, "Heap: 0x%08x - 0x%08x\n", heap, heap_end);
    mprintf(LOGLEVEL_DEFAULT, "Heap size: %d bytes\n", heap_end - heap);
}

void *kmalloc(uint32_t size)
{
    uint32_t mem = heap;
    struct alloc *alloc;
    while(mem < heap_end) {
        alloc = (struct alloc *)mem;
        if(alloc->status == 1) {
            // Block is occupied.
            goto to_next_block;
        }

        if(alloc->status == 0) {
            // Block is unoccupied.
            if(alloc->size >= size) {
                // We can use it!
                alloc->status = 1;
                return HEAD_TO_POINTER(alloc);
            } else if(alloc->size == 0) {
                // This is the last block, it has no size!
                alloc->size = size;
                alloc->status = 1;
                return HEAD_TO_POINTER(alloc);         
            } else {
                // Not enough space
                goto to_next_block;
            }
        }

to_next_block:
            mem += sizeof(struct alloc);
            mem += alloc->size;
            continue;
    }
    panic("No more memory available for heap!");
    return 0;
}

void kfree(void *ptr)
{
    if(!ptr) {
        return;
    }
    struct alloc *alloc = POINTER_TO_HEAD(ptr);
    alloc->status = 0;
}

void *kmalloc_a(uint32_t size)
{
    uint32_t mem = heap;
    struct alloc *alloc;
    while(mem < heap_end) {
        alloc = (struct alloc *)mem;
        if(alloc->status == 1) {
            // Block is occupied.
            goto to_next_block;
        }

        if(alloc->status == 0) {
            // Block is unoccupied.
            if(alloc->size >= size) {
                // We can use it, check align.
                if(PAGE_ALIGNED(HEAD_TO_POINTER(alloc))) {
                    alloc->status = 1;
                    return HEAD_TO_POINTER(alloc);
                } else {
                    // goddamnit, its not page aligned :c
                    goto to_next_block; 
                }
            } else if(alloc->size == 0) {
                // This is the last block, it has no size!
                // Check align.
                if(PAGE_ALIGNED(HEAD_TO_POINTER(alloc))) {
                    alloc->status = 1;
                    alloc->size = size;
                    return HEAD_TO_POINTER(alloc);
                } else {
                    // It's not page aligned, but we can make it page aligned
                    // Align the current address and check the offset
                    uint32_t addr = (uint32_t)HEAD_TO_POINTER(alloc);
                    uint32_t offset = 0;

                    // Align the temp address
                    PAGE_ALIGN(addr);

                    // Calculate the offset
                    offset = addr - (uint32_t)HEAD_TO_POINTER(alloc);

                    // Atleast the alloc struct needs to fit in between the offset
                    if(offset >= sizeof(struct alloc)) {
                        // Great, now alloc a block before this block
                        alloc->size = offset - sizeof(struct alloc);
                        alloc = (struct alloc *)(addr - sizeof(struct alloc));
                        alloc->size = size;
                        alloc->status = 1;
                        return HEAD_TO_POINTER(alloc);
                    } else {
                        // Dang it, now we need to skip 4096 bytes + offset to get the next page aligned address
                        // First we have to allocate a empty block
                        alloc->size = offset + 0x1000;

                        // Now get to the next one
                        alloc = (struct alloc *)((addr + 0x1000) - sizeof(struct alloc));
                        alloc->size = size;
                        alloc->status = 1;
                        return HEAD_TO_POINTER(alloc);
                    }
                }
            } else {
                // Not enough space
                goto to_next_block;
            }
        }

to_next_block:
            mem += sizeof(struct alloc);
            mem += alloc->size;
            continue;
    }
    panic("No more memory available for heap!");
    return 0;
}

void *kmalloc_p(uint32_t size, uint32_t *phys)
{
    void *ptr = kmalloc(size);
    if (paging_enabled && phys)
    {
        struct page *page = get_page((uint32_t)ptr, 0, kernel_directory);
        *phys = page->frame * 0x1000 + ((uint32_t)ptr & 0xFFF);
    } else {
        if(phys) {
            *phys = (uint32_t)ptr;
        }
    }
    return ptr;
}

void *kmalloc_ap(uint32_t size, uint32_t *phys)
{    
    void *ptr = kmalloc_a(size);
    if (paging_enabled && phys)
    {
        struct page *page = get_page((uint32_t)ptr, 0, kernel_directory);
        *phys = page->frame * 0x1000 + ((uint32_t)ptr & 0xFFF);
    } else {
        if(phys) {
            *phys = (uint32_t)ptr;
        }
    }
    return ptr;
}