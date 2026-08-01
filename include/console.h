#ifndef __CONSOLE_H__
#define __CONSOLE_H__

#include <stdint.h>

// A console channel decouples a task's terminal I/O from the VGA text screen:
// its SYS_WRITE output lands in the out ring (drained by whoever owns the
// channel, e.g. the wm's terminal window) and SYS_GETC reads come from the in
// ring (fed by the owner). Tasks spawned by a console task inherit it, so a
// shell's children print into the same terminal.

#define NCONSOLE 8

// Allocate a channel; -1 if none free.
int console_alloc(void);

// Mark a channel free again. The owner should only do this after the attached
// task has exited (slots are reused).
void console_free(int cid);

int console_valid(int cid);

// Record / query the pid of the task the channel was created for.
void console_set_pid(int cid, int pid);
int console_pid(int cid);

// Task side: append output (bytes beyond a full ring are dropped), fetch one
// input byte (-1 if none pending).
int console_out_write(int cid, const char *buf, uint32_t len);
int console_in_getc(int cid);

// Owner side: drain output (non-blocking), feed input.
int console_out_read(int cid, char *buf, uint32_t len);
int console_in_write(int cid, const char *buf, uint32_t len);

#endif
