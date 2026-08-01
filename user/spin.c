// spin -- burns CPU forever. Exists so the test suite can verify preemption
// keeps the system responsive and that SYS_KILL can put down a task that will
// never exit on its own.
#include "ulib.h"

void _start(void)
{
    put("spin: spinning\n");
    for (;;) {
    }
}
