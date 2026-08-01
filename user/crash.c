// Deliberately faults from ring 3. Exists to prove fault isolation: the
// kernel must kill this task (page fault: NULL is a supervisor-only page)
// and the shell must keep running afterwards.
#include "ulib.h"

void _start(void)
{
    put("crash: dereferencing NULL from ring 3...\n");

    *(volatile int *)0 = 42;

    // If we get here, ring 3 memory protection is broken.
    put("crash: STILL ALIVE - kernel memory is user-writable!\n");
    exit(1);
}
