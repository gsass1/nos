#ifndef __PIT_H__
#define __PIT_H__

#include <stdint.h>

// The rate kmain programs the timer at; SYS_SLEEP converts ms with this.
#define PIT_HZ 200

void pit_init(void);
void pit_init_timer(uint32_t freq);

// Timer interrupts since boot (PIT_HZ per second), bumped in _asm_irq_0.
extern volatile uint32_t timer_ticks;

#endif