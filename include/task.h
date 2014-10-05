#ifndef __TASK_H__
#define __TASK_H__

#include <mm.h>
#include <stdint.h>

struct task
{
    int id;
    char name[256];
    uint32_t esp;
    uint32_t ebp;
    uint32_t eip;
    void *stack;
    void *stack_top;
    struct page_directory *page_directory;
    struct task *next;
};

void print_task_info(void);

int spawn_task(const char *name, void *addr);

void tasking_init(void);

void task_switch(void);

int fork(void);

void move_stack(void *new_stack_start, uint32_t size);

int getpid(void);

#endif