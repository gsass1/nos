#include <mutex.h>
#include <task.h>

void mutex_lock(struct mutex *mutex)
{
    while(mutex->locked) {
        task_switch();
    }
    mutex->locked = 1;
}

void mutex_unlock(struct mutex *mutex)
{
    mutex->locked = 0;
    task_switch();
}