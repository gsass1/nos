#ifndef __TASK_H__
#define __TASK_H__

#include <mm.h>
#include <stdint.h>

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
    struct page_directory *page_directory;
    struct task *next;
};

void tasking_init(void);

// Create a runnable kernel thread that begins executing at `entry`.
int spawn_task(const char *name, void *entry);

// Cooperative yield. Picks the next task and switches to it, returning to the
// caller when this task is next scheduled. Also used as the timer-preemption
// entry point (see _asm_irq_0 / schedule()).
void task_switch(void);

// Terminate the current task and switch away permanently. Never returns.
void exit(void);

int getpid(void);

const char *getpname(void);

void print_task_info(void);

#endif
