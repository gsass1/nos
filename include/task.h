#ifndef __TASK_H__
#define __TASK_H__

#include <mm.h>
#include <stdint.h>

struct fs_node;
struct pipe;

// What an fd refers to. Every task gets fds 0/1/2 (stdin/stdout/stderr) at
// spawn -- either the real VGA console/keyboard or a console channel (a
// terminal window) -- and children inherit them through exec, Unix-style.
// fds 3+ are files opened from the initrd, pipe ends, or TCP sockets. Pipe
// ends and sockets are the refcounted types: duplicate through file_addref,
// drop through file_close, and exit()/task_kill() close a dying task's whole
// table so a blocked peer sees EOF promptly. FD_NONE (0) means the slot is
// free.
enum fd_type
{
    FD_NONE = 0,
    FD_CONSOLE, // VGA text output / PS/2 keyboard input
    FD_CHANNEL, // console channel (see console.h)
    FD_FILE,    // initrd file
    FD_PIPE_R,  // read end of a pipe (see pipe.h)
    FD_PIPE_W,  // write end of a pipe
    FD_SOCKET,  // TCP connection (see tcp.h)
};

// OR'd into an FD_FILE type to mark access mode. sys_write rejects FD_FILE
// without FD_WRITABLE; sys_read rejects FD_FILE with FD_WRITEONLY. Encoded
// in the upper bits of `type` so no struct size change is needed.
#define FD_WRITABLE   0x100
#define FD_WRITEONLY  0x200
#define FD_TYPE(t)    ((t) & 0xFF)

struct file
{
    int type;
    struct fs_node *node; // FD_FILE
    uint32_t offset;      // FD_FILE
    int cid;              // FD_CHANNEL
    struct pipe *pipe;    // FD_PIPE_R / FD_PIPE_W
    int sock;             // FD_SOCKET
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
    struct file files[TASK_MAX_FILES]; // indexed directly by fd
    struct task *next;      // ready-queue link
    struct task *reap_next; // zombie-list link (used only after exit())
};

// Account for a duplicated / dropped reference to the object behind a file.
// Pipe ends and sockets carry references; other types only need the slot
// cleared on close.
void file_addref(struct file *f);
void file_close(struct file *f);

void tasking_init(void);

// Create a runnable task that begins executing at `entry` in address space
// `dir` (pass 0 to share the current directory). If `user_esp` is non-zero the
// task starts in ring 3 with that stack pointer; otherwise it is a ring 0
// kernel thread. `stdio` is an array of 3 file objects installed as the new
// task's fds 0/1/2 (NULL = the real console/keyboard).
int spawn_task(const char *name, void *entry, struct page_directory *dir,
               uint32_t user_esp, const struct file *stdio);

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

// Forcefully terminate another task: unlink it from the ready queue and hand
// it to the reaper. Returns 0, or -1 if the pid isn't running (or is the
// kernel task). Killing yourself is exit(-9).
int task_kill(int pid);

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
