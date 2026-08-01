#include <mm.h>
#include <string.h>
#include <task.h>
#include <kernel.h>
#include <debug.h>

MODULE("TASK");

// The task currently on the CPU and the head of the round-robin ready queue.
// Both are NULL until tasking_init() runs, which is how schedule()/task_switch()
// stay safe no-ops during early boot (mprintf -> mutex -> task_switch).
volatile struct task *current_task;
volatile struct task *ready_queue;

static uint32_t next_pid = 0;

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
    current_task->page_directory = current_directory;
    current_task->next = 0;
    strcpy((char *)current_task->name, "kernel_task");

    mprintf(LOGLEVEL_DEFAULT, "Created kernel_task (pid %d)\n", current_task->id);

    asm volatile("sti");
}

int spawn_task(const char *name, void *entry)
{
    mprintf(LOGLEVEL_DEBUG, "Spawning task %s with entry: 0x%08x\n", name, entry);

    asm volatile("cli");

    struct task *task = kmalloc(sizeof(struct task));
    task->id = next_pid++;
    task->page_directory = (struct page_directory *)current_directory;
    task->next = 0;
    strcpy(task->name, name);

    // Allocate a kernel stack and prime it with a trap frame identical to the
    // one _asm_irq_0 leaves behind, so the scheduler can iret straight into
    // `entry` as if this task had just been preempted there.
    const uint32_t stack_size = 0x1000;
    uint8_t *stack_mem = kmalloc(stack_size);
    task->stack_mem = stack_mem;

    uint32_t *sp = (uint32_t *)(stack_mem + stack_size);

    // iret frame (ring0 -> ring0: CPU pushes no ss/esp).
    *--sp = 0x202;              // eflags: reserved bit 1 + IF set
    *--sp = 0x08;              // cs (kernel code segment)
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
    *--sp = 0x10;              // ds
    *--sp = 0x10;              // es
    *--sp = 0x10;              // fs
    *--sp = 0x10;              // gs

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

void exit(void)
{
    asm volatile("cli");

    if (current_task == ready_queue && current_task->next == 0) {
        panic("Cannot exit the last remaining task!\n");
    }

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

    // We're still executing on this task's kernel stack, so we cannot free it
    // (or the task struct) here -- doing so would pull the rug out from under
    // the switch below. A proper implementation hands teardown to a reaper
    // task; for now the memory is intentionally leaked.
    asm volatile("sti");

    // Switch away for good. Because we're unlinked, the scheduler will never
    // pick this task again, so task_switch() never returns.
    task_switch();

    panic("exit(): returned from final task_switch!\n");
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
