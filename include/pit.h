#ifndef __PIT_H__
#define __PIT_H__

#include <stdint.h>

void pit_init(void);
void pit_init_timer(uint32_t freq);

#endif