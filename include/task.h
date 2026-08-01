#ifndef __TASK_H__
#define __TASK_H__

#include <mm.h>
#include <stdint.h>

struct fs_node;

// One open file. node == 0 means the slot is free. Files come from the
// (read-only) initrd, so there is nothing to release on close/reap beyond
// clearing the slot.
struct file
{
    struct fs_node *node;
    uint32_t offset;
};

#define TASK_MAX_FILES 8

struct task
{
    int id;
    char name[256];
    // Saved kernel stack pointer. Points at a trap frame laid out exactly like
    // the one _asm_irq_0 builds, so the scheduler can iret into any task
    // whether it was preempted by the timer or yielded cooperatively.
    uint32_t esp;
    // Base of the heap-allocated kernel stack (0 for the boot/kernel task,
    // which runs on the bootstrap stack from boot.S).
    void *stack_mem;
    // One past the top of that kernel stack. Loaded into tss.esp0 whenever
    // this task is scheduled, so a ring3 -> ring0 transition (syscall, IRQ,
    // fault) lands on an empty kernel stack. 0 for the kernel task, which
    // never runs in ring 3.
    uint32_t kstack_top;
    struct page_directory *page_directory;
    int owns_dir; // if set, page_directory is freed when the task is reaped
    // Program break for SYS_SBRK: the user heap occupies [USER_HEAP_BASE, brk).
    // 0 for kernel threads (no user heap).
    uint32_t brk;
    // Console channel this task's terminal I/O is routed through, or -1 for
    // the real VGA/keyboard console. Children spawned via SYS_EXEC inherit it.
    int console_id;
    struct file files[TASK_MAX_FILES]; // fds 3.. index this as fd-3
    struct task *next;      // ready-queue link
    struct task *reap_next; // zombie-list link (used only after exit())
};

void tasking_init(void);

// Create a runnable task that begins executing at `entry` in address space
// `dir` (pass 0 to share the current directory). If `user_esp` is non-zero the
// task starts in ring 3 with that stack pointer; otherwise it is a ring 0
// kernel thread. `console_id` routes the task's terminal I/O (-1 = the real
// console).
int spawn_task(const char *name, void *entry, struct page_directory *dir,
               uint32_t user_esp, int console_id);

// Returns non-zero while a task with the given pid is still in the ready queue.
int task_alive(int pid);

// Cooperative yield. Picks the next task and switches to it, returning to the
// caller when this task is next scheduled. Also used as the timer-preemption
// entry point (see _asm_irq_0 / schedule()).
void task_switch(void);

// Terminate the current task with the given exit status and switch away
// permanently. Never returns. The status is retrievable (for a while) via
// task_exit_code() after the task has died.
void exit(int code);

// Exit status of a recently-died task, or 0 if unknown/forgotten.
int task_exit_code(int pid);

// The task currently running on the CPU (0 before tasking_init).
struct task *task_current(void);

// Free the resources of tasks that have exit()ed. Safe to call from the idle
// loop; it runs in its own address space, so it can free a dead task's stack
// and page directory without pulling the rug out from under itself.
void reap_tasks(void);

int getpid(void);

const char *getpname(void);

void print_task_info(void);

#endif
