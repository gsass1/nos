#ifndef __MUTEX_H__
#define __MUTEX_H__

struct mutex
{
    int locked;
};

#define MUTEX_INIT { .locked = 0 }

void mutex_lock(struct mutex *mutex);

void mutex_unlock(struct mutex *mutex);

#endif