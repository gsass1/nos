#include <kernel.h>
#include <mutex.h>
#include <serial.h>
#include <string.h>
#include <vsprintf.h>
#include <vga.h>
#include <vfs.h>
#include <task.h>
#include <mm.h>

static int panic_nested = 0;

/*

TODO:
    For the kernel symbol table, load the file from initrd!
    Then parse it here.

*/

struct sym
{
    uint32_t addr;
    char type;
    char name[64];
    struct sym *next;
};

static struct sym *symbol_map;

static int hextoint(const char *str)
{
    return (int)strtol(str, 0, 16);
}

static void sym_parse_symtable(uint8_t *buffer)
{
}

void sym_init(void)
{ 
    uint8_t *temp = kmalloc(8192);
    int i = 0;
    struct dirent *node = 0;
    while((vfs_readdir(fs_root, i)) != 0) {
        if(strcmp(node->name, "symtable") == 0) {
            if(vfs_read(node, 0, 8192, temp) != 0) {
                sym_parse_symtable(temp);
            } else {
				kprintf("Sym: vfs_read failed!\n");
			}	
        }
        i++;
    }
    kprintf("Sym: failed to init!\n");
}

const char *sym_get(uint32_t addr)
{
    #if 0
    struct sym *sym = symbol_map;
    while(sym) {
        if(sym->addr < addr && sym->next->addr > addr) {
            return sym->name;
        } else {
            sym = sym->next;
        }
    }
    #endif
    return "unknown";
}

void halt(void)
{
    while(1) {
        asm volatile("hlt");
    }   
}

void DPRINT(const char *fmt, ...)
{
    static char temp[1024];
    va_list list;

    va_start(list, fmt);
    vsprintf(temp, fmt, list);
    va_end(list);

    serial_write_str(temp);
}

static struct mutex kprintf_mtx = MUTEX_INIT;

void kprintf(const char *fmt, ...)
{
    mutex_lock(&kprintf_mtx);

    static char temp[1024];
    va_list list;

    va_start(list, fmt);
    vsprintf(temp, fmt, list);
    va_end(list);

    vga_print(temp);
    serial_write_str(temp);

    mutex_unlock(&kprintf_mtx);
}

static void show_stack_trace(unsigned int max_frames)
{
    kprintf("Stack trace:\n");

    unsigned int *ebp = &max_frames - 2;
    unsigned int i;

    for(i = 0; i < max_frames; ++i) {
        unsigned int eip = ebp[1];
        if(eip == 0)
            break;
        ebp = (unsigned int *)ebp[0];
        unsigned int *args = &ebp[2];
        kprintf("    0x%08x [%s]\n", eip, sym_get(eip));
    }
}

void panic(const char *fmt, ...)
{
    if(panic_nested) {
        return;
    }

    panic_nested = 1;

    asm volatile("cli");

    static char temp[1024];
    va_list list;

    va_start(list, fmt);
    vsprintf(temp, fmt, list);
    va_end(list);

    kprintf("panic: %s\n", temp);

    print_task_info();

    kprintf("\n");

    show_stack_trace(10);
    
    halt();
}

static struct mutex mprintf_mtx = MUTEX_INIT;

void __mprintf(uint8_t type, const char *fmt, ...)
{
    static char temp[1024];

    va_list ap;
    va_start(ap, fmt);
    kprintf("[%s]: ", fmt);
    char *fmt2 = va_arg(ap, char *);
    vsprintf(temp, fmt2, ap);
    va_end(ap);
    kprintf("%s", temp);
}
