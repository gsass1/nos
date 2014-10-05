#include <mm.h>
#include <string.h>
#include <string.h>
#include <task.h>
#include <kernel.h>
#include <debug.h>

MODULE("TASK");

volatile struct task *current_task;
volatile struct task *ready_queue;

extern uint32_t initial_esp;
extern uint32_t read_eip();

uint32_t next_pid = 0;

void idle_task(void)
{
    while(1) {
        task_switch();
    }
}

void idle_task2(void)
{
    while(1) {
        kprintf("b\n");
        task_switch();
    }
}

void shell_task(void)
{
    kprintf("HELLO IM THE SHELL TASK!");
}

void exit(void)
{
    if(getpid() == 0) {
        panic("Cannot kill kernel task!\n");
    }

    asm volatile("cli");

//Find previous task
    struct task *task_prev = 0;
    struct task *task_r = ( struct task* ) ready_queue;

    for ( ; task_r->next != 0; task_r = task_r->next ) {
        if ( task_r->next == current_task ) {
            //We got the previous task
            task_prev = task_r;
            break; //Don't bother with the rest of the list
        }
    }

    //We didn't find the task and it is not the ready_queue
    if ( !task_prev && current_task != ready_queue ) {
        return;
    }

    //if our current task is the ready_queue then set the starting task as the next task after current_task
    if ( current_task == ready_queue ) {
        ready_queue = current_task->next;

    } else {
        task_prev->next = current_task->next;
    }

    kfree((void *)current_task);
    kfree(current_task->stack_top);
    kfree(current_task->page_directory);

    asm volatile("sti");
    task_switch();
}

void tasking_init(void)
{
    mprintf(LOGLEVEL_DEFAULT, "Initializing Tasking\n");

    asm volatile("cli");

    move_stack((void *)0xE0000000, 0x2000);

    current_task = ready_queue = kmalloc(sizeof(struct task));
    current_task->id = next_pid++;
    current_task->esp = current_task->ebp = 0;
    current_task->eip = 0;
    current_task->page_directory = current_directory;
    current_task->next = 0;
    current_task->stack = kmalloc(0x1000) + 0x1000;
    current_task->esp = (uint32_t)current_task->stack;
    current_task->stack_top = (void *)((uint32_t)current_task->stack - 0x1000);

    uint32_t *stack = (uint32_t *)current_task->stack;

    *--stack = 0x202; // EFLAGS
    *--stack = 0x08;  // CS
    *--stack = 0;     // EIP
    *--stack = 0;   // NULL
    *--stack = 0;   // EBX
    *--stack = 0;   // EDX
    *--stack = 0;   // ECX
    *--stack = 0;   // EAX
    *--stack = 0x10;    // DS
    *--stack = 0x10;    // ES
    *--stack = 0x10;    // FS
    *--stack = 0x10;    // GS

    current_task->stack = (void *)stack;

    strcpy(current_task->name, "kernel_task");

    mprintf(LOGLEVEL_DEFAULT, "Created kernel_task\n");

    //spawn_task("idle", idle_task);

    asm volatile("sti");
}

int spawn_task(const char *name, void *addr)
{
    mprintf(LOGLEVEL_DEBUG, "Spawning task %s with eip: 0x%08x\n", name, addr);

    asm volatile("cli");

    struct task *task = kmalloc(sizeof(struct task));
    task->stack = kmalloc(0x1000) + 0x1000;
    task->esp = (uint32_t)task->stack;
    task->stack_top = (void *)((uint32_t)task->stack - 0x1000);

    task->page_directory = 0;

    task->id = next_pid++;

    uint32_t *stack = (uint32_t *)task->stack;

    // processor data (iret)
    *--stack = 0x202;   // EFLAGS
    *--stack = 0x08;    // CS
    *--stack = (uint32_t)addr; // EIP
    task->eip = (uint32_t)addr;

    // pusha
    *--stack = 0;   // EDI
    *--stack = 0;   // ESI
    *--stack = 0;   // EBP
    task->ebp = 0;
    *--stack = 0;   // NULL
    *--stack = 0;   // EBX
    *--stack = 0;   // EDX
    *--stack = 0;   // ECX
    *--stack = 0;   // EAX

    // data segments
    *--stack = 0x10;    // DS
    *--stack = 0x10;    // ES
    *--stack = 0x10;    // FS
    *--stack = 0x10;    // GS

    task->stack = (void *)stack;

    strcpy(task->name, name);

    task->next = 0;

    struct task *tmp_task;
    tmp_task = ( struct task* )ready_queue;

    while ( tmp_task->next != 0 ) {
        tmp_task = tmp_task->next;
    }

    // ...And extend it.
    tmp_task->next = task;

    asm volatile("sti"); 

    return task->id; 
}

int fork(void)
{
    asm volatile("cli");

    struct task *parent_task = (struct task *)current_task;

    struct page_directory *directory = clone_directory(current_directory);

    struct task *new_task = kmalloc(sizeof(struct task));

    new_task->id = next_pid++;
    new_task->esp = new_task->ebp = 0;
    new_task->eip = 0;
    new_task->page_directory = directory;
    new_task->next = 0;

    struct task *tmp_task = (struct task *)ready_queue;

    while(tmp_task->next) {
        tmp_task = tmp_task->next;
    }

    tmp_task->next = new_task;

    uint32_t eip = read_eip();

    if(current_task == parent_task) {
            // We are the parent, so set up the esp/ebp/eip for our child.
           uint32_t esp; asm volatile("mov %%esp, %0" : "=r"(esp));
           uint32_t ebp; asm volatile("mov %%ebp, %0" : "=r"(ebp));
           new_task->esp = esp;
           new_task->ebp = ebp;
           new_task->eip = eip;
           // All finished: Reenable interrupts.
           asm volatile("sti");
           return new_task->id;
    } else {
        asm volatile("sti");
        return 0;
    }
}

void task_switch(void)
{
    if(!current_task) {
        return;
    }

    uint32_t esp, ebp, eip;
    asm volatile("mov %%esp, %0" : "=r"(esp));
    asm volatile("mov %%ebp, %0" : "=r"(ebp));

    // Read the instruction pointer. We do some cunning logic here:
    // One of two things could have happened when this function exits -
    // (a) We called the function and it returned the EIP as requested.
    // (b) We have just switched tasks, and because the saved EIP is essentially
    // the instruction after read_eip(), it will seem as if read_eip has just
    // returned.
    // In the second case we need to return immediately. To detect it we put a dummy
    // value in EAX further down at the end of this function. As C returns values in EAX,
    // it will look like the return value is this dummy value! (0x12345).
    eip = read_eip();

    // Have we just switched tasks?
    if (eip == 0x12345) {
        return;
    }

    // No, we didn't switch tasks. Let's save some register values and switch.
    current_task->eip = eip;
    current_task->esp = esp;
    current_task->ebp = ebp;

    current_task = current_task->next;

    if(!current_task) {
        current_task = ready_queue;
    }

    eip = current_task->eip;
    esp = current_task->esp;
    ebp = current_task->ebp;

    if(current_task->page_directory) {
        current_directory = current_task->page_directory;
    }

    // Here we:
    // * Stop interrupts so we don't get interrupted.
    // * Temporarily put the new EIP location in ECX.
    // * Load the stack and base pointers from the new task struct.
    // * Change page directory to the physical address (physicalAddr) of the new directory.
    // * Put a dummy value (0x12345) in EAX so that above we can recognise that we've just
    // switched task.
    // * Restart interrupts. The STI instruction has a delay - it doesn't take effect until after
    // the next instruction.
    // * Jump to the location in ECX (remember we put the new EIP in there).
    //DPRINT("SWITCHING TASK NOW\n");
    //DPRINT("current_directory->phys_addr: 0x%08x\n", current_directory->phys_addr);
    asm volatile("cli; \
        mov %0, %%ecx;\ 
        mov %1, %%esp;\ 
        mov %2, %%ebp;\ 
        mov %3, %%cr3;\ 
        mov $0x12345, %%eax;\
        sti; \
        jmp *%%ecx": : "r"(eip), "r"(esp), "r"(ebp), "r"(current_directory->phys_addr));
}

void move_stack(void *new_stack_start, uint32_t size)
{
    mprintf(LOGLEVEL_DEBUG, "Moving stack\n");
    mprintf(LOGLEVEL_DEBUG, "New stack start: 0x%08x\n", new_stack_start);
    mprintf(LOGLEVEL_DEBUG, "Size: %d\n", size);

    uint32_t i;

    for( i = (uint32_t)new_stack_start; i >= (uint32_t)new_stack_start - size; i -= 0x1000) {
        alloc_frame(get_page(i, 1, current_directory), 0 /* User mode */, 1 /* Is writable */ );
    }

    uint32_t pd_addr;
    asm volatile("mov %%cr3, %0" : "=r" (pd_addr));
    asm volatile("mov %0, %%cr3" : : "r" (pd_addr));

    uint32_t old_stack_pointer;
    asm volatile("mov %%esp, %0" : "=r" (old_stack_pointer));

    uint32_t old_base_pointer; 
    asm volatile("mov %%ebp, %0" : "=r" (old_base_pointer));

    uint32_t offset = (uint32_t)new_stack_start - initial_esp;

    uint32_t new_stack_pointer = old_stack_pointer + offset;
    uint32_t new_base_pointer  = old_base_pointer  + offset;

    memcpy((void*)new_stack_pointer, (void*)old_stack_pointer, initial_esp-old_stack_pointer);


    for(i = (uint32_t)new_stack_start; i > (uint32_t)new_stack_start - size; i -= 4)
    {
       uint32_t tmp = * (uint32_t*)i;
       // If the value of tmp is inside the range of the old stack, assume it is a base pointer
       // and remap it. This will unfortunately remap ANY value in this range, whether they are
       // base pointers or not.
       if (( old_stack_pointer < tmp) && (tmp < initial_esp))
       {
         tmp = tmp + offset;
         uint32_t *tmp2 = (uint32_t*)i;
         *tmp2 = tmp;
       }
    }

      // Change stacks.
      asm volatile("mov %0, %%esp" : : "r" (new_stack_pointer));
      asm volatile("mov %0, %%ebp" : : "r" (new_base_pointer));
}

int getpid(void)
{
    return current_task->id;
}

const char *getpname(void)
{
    return current_task->name;
}

void print_task_info(void)
{
    kprintf("Task info:\n");
    kprintf("PID: %d\n", getpid());
    kprintf("Name: %s\n", getpname());
}