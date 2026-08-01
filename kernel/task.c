#include <elf.h>
#include <fb.h>
#include <gdt.h>
#include <wsurf.h>
#include <mm.h>
#include <pipe.h>
#include <string.h>
#include <task.h>
#include <tcp.h>
#include <kernel.h>
#include <debug.h>

MODULE("TASK");

// The task currently on the CPU and the head of the round-robin ready queue.
// Both are NULL until tasking_init() runs, which is how schedule()/task_switch()
// stay safe no-ops during early boot (mprintf -> mutex -> task_switch).
volatile struct task *current_task;
volatile struct task *ready_queue;

// Tasks that have exit()ed but whose resources (stack, page directory, struct)
// haven't been freed yet. They can't free themselves while running on their own
// stack, so reap_tasks() (called from the idle loop) does it later.
static volatile struct task *zombie_queue;

static uint32_t next_pid = 0;

// Ring buffer of recent exit statuses. The reaper frees a dead task's struct
// before the parent necessarily calls wait, so the status must outlive it.
// Old entries are overwritten; wait on a long-dead pid just reports 0.
#define EXIT_RECORDS 32
static struct { int pid; int code; } exit_records[EXIT_RECORDS];
static int exit_record_next;

void file_addref(struct file *f)
{
    if (f->type == FD_PIPE_R || f->type == FD_PIPE_W) {
        pipe_addref(f->pipe, f->type == FD_PIPE_W);
    } else if (f->type == FD_SOCKET) {
        tcp_addref(f->sock);
    }
}

void file_close(struct file *f)
{
    if (f->type == FD_PIPE_R || f->type == FD_PIPE_W) {
        pipe_release(f->pipe, f->type == FD_PIPE_W);
    } else if (f->type == FD_SOCKET) {
        tcp_release(f->sock);
    }
    f->type = FD_NONE;
    f->node = 0;
    f->pipe = 0;
    f->sock = 0;
}

// Drop every fd a dying task holds. This must happen when the task dies, not
// at reap time: a peer blocked on the far end of a pipe only unblocks (EOF
// for readers, error for writers) once these references are gone.
static void close_all_files(struct task *t)
{
    for (int i = 0; i < TASK_MAX_FILES; i++) {
        if (t->files[i].type != FD_NONE) {
            file_close(&t->files[i]);
        }
    }
}

// Selects the next task to run. Called from assembly (both the timer IRQ and
// the cooperative task_switch yield) with `esp` pointing at the outgoing task's
// trap frame; returns the incoming task's saved esp. On the very first tick,
// current_task is already set but its esp is stale (0) -- we still overwrite it
// with the real boot esp here, capturing the kernel task's live context.
uint32_t schedule(uint32_t esp)
{
    if (!current_task) {
        // Tasking not initialised yet: nothing to switch to.
        return esp;
    }

    current_task->esp = esp;

    current_task = current_task->next ? current_task->next : ready_queue;

    // Only reload cr3 when the address space actually changes. Kernel threads
    // all share one directory, so this normally avoids a needless TLB flush.
    if (current_task->page_directory &&
        current_task->page_directory != current_directory) {
        current_directory = current_task->page_directory;
        asm volatile("mov %0, %%cr3" :: "r"(current_directory->phys_addr));
    }

    // If the incoming task can run in ring 3, the next syscall/IRQ/fault it
    // takes must land at the top of ITS kernel stack.
    if (current_task->kstack_top) {
        tss_set_esp0(current_task->kstack_top);
    }

    return current_task->esp;
}

void tasking_init(void)
{
    mprintf(LOGLEVEL_DEFAULT, "Initializing Tasking\n");

    asm volatile("cli");

    // The kernel task adopts the stack we are already running on (from boot.S).
    // We don't build a frame for it: its real esp is captured by schedule() on
    // the first timer tick, and it resumes wherever it was preempted.
    current_task = ready_queue = kmalloc(sizeof(struct task));
    current_task->id = next_pid++;
    current_task->esp = 0;
    current_task->stack_mem = 0;
    current_task->kstack_top = 0; // pure ring0 task, esp0 never used
    current_task->brk = 0;
    memset((void *)current_task->files, 0, sizeof(current_task->files));
    for (int i = 0; i < 3; i++) {
        current_task->files[i].type = FD_CONSOLE;
    }
    current_task->page_directory = current_directory;
    current_task->owns_dir = 0; // shares the boot address space; never reaped
    current_task->next = 0;
    strcpy((char *)current_task->name, "kernel_task");

    mprintf(LOGLEVEL_DEFAULT, "Created kernel_task (pid %d)\n", current_task->id);

    asm volatile("sti");
}

int spawn_task(const char *name, void *entry, struct page_directory *dir,
               uint32_t user_esp, const struct file *stdio)
{
    mprintf(LOGLEVEL_DEBUG, "Spawning task %s with entry: 0x%08x\n", name, entry);

    asm volatile("cli");

    struct task *task = kmalloc(sizeof(struct task));
    task->id = next_pid++;
    task->page_directory = dir ? dir : current_directory;
    // We own (and must free on reap) any directory that isn't the shared one.
    task->owns_dir = (dir && dir != current_directory);
    task->next = 0;
    strcpy(task->name, name);

    // Allocate a kernel stack and prime it with a trap frame identical to the
    // one _asm_irq_0 leaves behind, so the scheduler can iret straight into
    // `entry` as if this task had just been preempted there.
    const uint32_t stack_size = 0x4000; // 16KB kernel stack (headroom for nested IRQs/syscalls)
    uint8_t *stack_mem = kmalloc(stack_size);
    task->stack_mem = stack_mem;
    task->kstack_top = user_esp ? (uint32_t)(stack_mem + stack_size) : 0;
    task->brk = user_esp ? USER_HEAP_BASE : 0;
    memset(task->files, 0, sizeof(task->files));
    if (stdio) {
        memcpy(task->files, stdio, 3 * sizeof(struct file));
        for (int i = 0; i < 3; i++) {
            file_addref(&task->files[i]);
        }
    } else {
        for (int i = 0; i < 3; i++) {
            task->files[i].type = FD_CONSOLE;
        }
    }

    uint32_t *sp = (uint32_t *)(stack_mem + stack_size);

    // Selectors: ring0 kernel segments, or the user segments (0x18/0x20) with
    // RPL 3 for a ring 3 task.
    uint32_t cs  = user_esp ? 0x1B : 0x08;
    uint32_t seg = user_esp ? 0x23 : 0x10;

    // iret frame. For ring0 -> ring0 the CPU pushes no ss/esp; for a ring 3
    // task iret additionally pops ss:esp, switching to the user stack.
    if (user_esp) {
        *--sp = seg;            // ss (user data, RPL 3)
        *--sp = user_esp;       // esp (user stack top)
    }
    *--sp = 0x202;              // eflags: reserved bit 1 + IF set (IOPL 0)
    *--sp = cs;                 // cs
    *--sp = (uint32_t)entry;    // eip
    // pushad block (values are don't-cares; order matches popad).
    *--sp = 0;                  // eax
    *--sp = 0;                  // ecx
    *--sp = 0;                  // edx
    *--sp = 0;                  // ebx
    *--sp = 0;                  // esp (ignored by popad)
    *--sp = 0;                  // ebp
    *--sp = 0;                  // esi
    *--sp = 0;                  // edi
    // Segment registers, laid out so RESTORE_REGS pops gs,fs,es,ds in order.
    *--sp = seg;               // ds
    *--sp = seg;               // es
    *--sp = seg;               // fs
    *--sp = seg;               // gs

    task->esp = (uint32_t)sp;

    // Append to the end of the ready queue.
    struct task *tail = (struct task *)ready_queue;
    while (tail->next) {
        tail = tail->next;
    }
    tail->next = task;

    asm volatile("sti");

    return task->id;
}

void exit(int code)
{
    asm volatile("cli");

    if (current_task == ready_queue && current_task->next == 0) {
        panic("Cannot exit the last remaining task!\n");
    }

    close_all_files((struct task *)current_task);
    // Like the fd table, the display and any window surface must be released
    // at death, not later: past this point the task never runs again, and
    // code after the sti below is abandoned if the timer preempts first.
    fb_task_exit(current_task->id);
    wsurf_task_exit(current_task->id,
                    (struct page_directory *)current_task->page_directory);

    exit_records[exit_record_next].pid = current_task->id;
    exit_records[exit_record_next].code = code;
    exit_record_next = (exit_record_next + 1) % EXIT_RECORDS;

    // Unlink ourselves from the ready queue. We keep current_task->next intact
    // so schedule() can still advance from this (now orphaned) node.
    if (current_task == ready_queue) {
        ready_queue = current_task->next;
    } else {
        struct task *prev = (struct task *)ready_queue;
        while (prev->next != current_task) {
            prev = prev->next;
        }
        prev->next = current_task->next;
    }

    // We're still executing on this task's kernel stack, so we can't free it
    // here. Hand ourselves to the zombie list (via reap_next, a separate link
    // so schedule()'s use of ->next below still walks the ready queue). Our own
    // ->next is left untouched so the switch continues round-robin correctly.
    current_task->reap_next = (struct task *)zombie_queue;
    zombie_queue = current_task;

    asm volatile("sti");

    // Switch away for good. Because we're off the ready queue, the scheduler
    // never picks this task again, so task_switch() never returns.
    task_switch();

    panic("exit(): returned from final task_switch!\n");
}

void reap_tasks(void)
{
    asm volatile("cli");
    struct task *z = (struct task *)zombie_queue;
    zombie_queue = 0;
    asm volatile("sti");

    while (z) {
        struct task *next = z->reap_next;

        if (z->stack_mem) {
            kfree(z->stack_mem);
        }
        if (z->owns_dir && z->page_directory) {
            free_directory(z->page_directory);
        }
        kfree(z);

        z = next;
    }
}

int task_alive(int pid)
{
    struct task *t = (struct task *)ready_queue;
    while (t) {
        if (t->id == pid) {
            return 1;
        }
        t = t->next;
    }
    return 0;
}

int task_kill(int pid)
{
    asm volatile("cli");

    if (current_task && current_task->id == pid) {
        asm volatile("sti");
        exit(-9); // never returns
    }

    // Find the victim in the ready queue. pid 0 is the kernel task: the head
    // of the queue and not killable (also the panic guard in exit()).
    struct task *prev = 0;
    struct task *t = (struct task *)ready_queue;
    while (t && t->id != pid) {
        prev = t;
        t = t->next;
    }
    if (!t || t == ready_queue) {
        asm volatile("sti");
        return -1;
    }

    // Same dance as exit(), but performed on the victim's behalf: unlink it
    // (leaving t->next intact in case the scheduler is mid-walk) and hand it
    // to the reaper. It is not running -- we are -- so its saved context just
    // dies with its stack.
    prev->next = t->next;

    close_all_files(t);
    fb_task_exit(t->id);
    wsurf_task_exit(t->id, t->page_directory);

    exit_records[exit_record_next].pid = t->id;
    exit_records[exit_record_next].code = -9;
    exit_record_next = (exit_record_next + 1) % EXIT_RECORDS;

    t->reap_next = (struct task *)zombie_queue;
    zombie_queue = t;

    asm volatile("sti");
    return 0;
}

int task_exit_code(int pid)
{
    for (int i = 0; i < EXIT_RECORDS; i++) {
        if (exit_records[i].pid == pid) {
            return exit_records[i].code;
        }
    }
    return 0;
}

struct task *task_current(void)
{
    return (struct task *)current_task;
}

int getpid(void)
{
    return current_task ? current_task->id : -1;
}

const char *getpname(void)
{
    return current_task ? (const char *)current_task->name : "<none>";
}

void print_task_info(void)
{
    kprintf("Task info:\n");
    kprintf("PID: %d\n", getpid());
    kprintf("Name: %s\n", getpname());
}
