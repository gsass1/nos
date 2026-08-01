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

// Symbols sorted by ascending address, populated from the "symtable" file
// (nm output) in the initrd. Used to resolve return addresses in backtraces.
static struct sym *symbol_map;
static uint32_t symbol_count;

static int sym_is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint32_t sym_parse_hex8(const char *s)
{
    uint32_t v = 0;
    int i;
    for(i = 0; i < 8 && sym_is_hex(s[i]); i++) {
        char c = s[i];
        uint32_t d = (c <= '9') ? (uint32_t)(c - '0')
                   : (c <= 'F') ? (uint32_t)(c - 'A' + 10)
                   : (uint32_t)(c - 'a' + 10);
        v = (v << 4) | d;
    }
    return v;
}

static void sym_insert(uint32_t addr, char type, const char *name)
{
    struct sym *s = kmalloc(sizeof(struct sym));
    s->addr = addr;
    s->type = type;
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = '\0';

    // Keep the list sorted by ascending address so sym_get can scan it.
    if(!symbol_map || addr < symbol_map->addr) {
        s->next = symbol_map;
        symbol_map = s;
    } else {
        struct sym *cur = symbol_map;
        while(cur->next && cur->next->addr <= addr) {
            cur = cur->next;
        }
        s->next = cur->next;
        cur->next = s;
    }
    symbol_count++;
}

// Parses `nm` output: each line is "AAAAAAAA T name". Lines for undefined
// symbols have blanks instead of an address and are skipped. Only text
// (code) symbols are kept, since backtraces resolve instruction pointers.
static void sym_parse_symtable(char *buffer)
{
    char *p = buffer;
    while(*p) {
        char *line = p;
        while(*p && *p != '\n') {
            p++;
        }
        if(*p == '\n') {
            *p++ = '\0';
        }

        if(sym_is_hex(line[0])) {
            uint32_t addr = sym_parse_hex8(line);
            char *q = line + 8;
            while(*q == ' ') {
                q++;
            }
            char type = *q;
            if(type) {
                q++;
                while(*q == ' ') {
                    q++;
                }
                if(*q && (type == 't' || type == 'T')) {
                    sym_insert(addr, type, q);
                }
            }
        }
    }
}

void sym_init(void)
{
    char name[] = "symtable";
    struct fs_node *node = vfs_finddir(fs_root, name);
    if(!node) {
        kprintf("Sym: symtable not found in initrd\n");
        return;
    }

    uint8_t *buf = kmalloc(node->length + 1);
    uint32_t read = vfs_read(node, 0, node->length, buf);
    if(read == 0) {
        kprintf("Sym: failed to read symtable\n");
        return;
    }
    buf[read] = '\0';

    sym_parse_symtable((char *)buf);
    kprintf("Sym: loaded %d symbols\n", symbol_count);
}

const char *sym_get(uint32_t addr)
{
    // List is sorted ascending; the enclosing symbol is the last one whose
    // address is <= addr.
    const char *name = "unknown";
    struct sym *sym = symbol_map;
    while(sym && sym->addr <= addr) {
        name = sym->name;
        sym = sym->next;
    }
    return name;
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
