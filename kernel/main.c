#include <gdt.h>
#include <idt.h>
#include <initrd.h>
#include <kernel.h>
#include <keyboard.h>
#include <mm.h>
#include <pic.h>
#include <pit.h>
#include <serial.h>
#include <sym.h>
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

int shell_exec(const char *str)
{
	if(strcmp(str, "help") == 0) {
		kprintf("help - Display this\n");
		return 0;
	} else if(strcmp(str, "ls") == 0) {
			int i = 0;
			struct dirent *node = 0;
			while((node = vfs_readdir(fs_root, i)) != 0) {
				kprintf("%s     ", node->name);
				struct fs_node *fs_node = vfs_finddir(fs_root, node->name);
				if((fs_node->flags & 0x7) == FS_DIRECTORY) {
					kprintf("(directory)");
				} else {
					kprintf("(file)");
				}
				kprintf("\n");
				i++;
			}
	} else if(strcmp(str, "cls") == 0) {
		vga_clear();
	} else {
		kprintf("Unknown command\n");
		return -1;
	}
}

void shell(void)
{

    kprintf("Entering debug kernel shell\n");
	char *buffer = kmalloc(256);

loop:
	memset(0, buffer, 256);
    kprintf("\nNOS: ");
    int i = 0;
	while(1) {
        char c = kbd_getc();
        if(c == 0) {
            continue;
        }

		if(c == '\n') {
			kprintf("\n");
			shell_exec(buffer);
			goto loop;
		}

		if(c == '\b') {
			if(i > 0) {
				buffer[--i] = 0;
			}
		}
		
		buffer[i++] = c;
		buffer[i] = '\0';

        kprintf("%c", c);
    }
}

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

	// Initialize tasking
	tasking_init();

	// Hooray, we are booted.
    kprintf("Welcome to NOS!\n");
	shell();
}
