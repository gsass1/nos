#ifndef __SYM_H__
#define __SYM_H__

#include <stdint.h>

void sym_init(void);

const char *sym_get(uint32_t addr);

#endif
