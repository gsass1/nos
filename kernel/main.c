#include <elf.h>
#include <gdt.h>
#include <idt.h>
#include <initrd.h>
#include <kernel.h>
#include <keyboard.h>
#include <mm.h>
#include <pic.h>
#include <pit.h>
#include <serial.h>
#include <string.h>
#include <sym.h>
#include <syscall.h>
#include <task.h>
#include <va_list.h>
#include <vsprintf.h>
#include <vfs.h>
#include <vga.h>

struct multiboot {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_Device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t num;
    uint32_t size;
    uint32_t addr;
    uint32_t shndx;
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint32_t vbe_mode;
    uint32_t vbe_interface_seg;
    uint32_t vbe_interface_off;
    uint32_t vbe_interface_len;
} __attribute__((packed));

struct multiboot *mbootptr;

uint32_t initial_esp;

extern uint32_t kernel_base, kernel_end;

// Assembly code from boot.S jumps directly to here
void kmain(struct multiboot *multiboot, uint32_t initial_stack)
{
    mbootptr = multiboot;
    initial_esp = initial_stack;

	// Initialize these first, want early text output!
	vga_init();
	serial_init();

	// initrd should be the only module
    uint32_t initrd_location = *((uint32_t*)mbootptr->mods_addr);
    uint32_t initrd_end = *(uint32_t*)(mbootptr->mods_addr + 4);

    DPRINT("initrd_locaton: 0x%08x\n", initrd_location);
    DPRINT("initrd_end: 0x%08x\n", initrd_end);

    if(mbootptr->mods_count == 0) {
        panic("Can't find initrd\n");
    }

	kprintf("NOS is booting...\n");

	// Initialize the annoying x86 stuff
    gdt_init();
    idt_init();
    pic_init();
    pit_init();
    pit_init_timer(200);

	// We can enable interrupts now
    asm volatile("sti");

	// Memory initialization
    heap_init();
    mm_paging_init();

	// PS/2 keyboard initalization
    kbd_init();

	// Initialize initial ram disk
    fs_root = initrd_init((void *)(*(uint32_t *)mbootptr->mods_addr));
  
	// Initialize symbol resolution
	sym_init();

	// Install the int 0x80 syscall gate userspace uses to call the kernel.
	syscall_init();

	// Initialize preemptive tasking. The kernel thread adopts the current
	// stack; the PIT IRQ drives round-robin scheduling from here on.
	tasking_init();

	// Hooray, we are booted.
    kprintf("Welcome to NOS!\n");

	// Load the shell from the initrd and run it as its own program. It talks to
	// the kernel only through syscalls; it runs in ring 0 for now but will move
	// to ring 3 unchanged once user mode lands.
	if(elf_exec("sh") < 0) {
		panic("Failed to load /sh from initrd\n");
	}

	// The kernel task's job is done. Idle so the timer keeps scheduling the
	// shell (and anything it spawns); hlt parks the CPU until the next IRQ.
	for(;;) {
		asm volatile("hlt");
	}
}
