#ifndef __WSURF_H__
#define __WSURF_H__

#include <stdint.h>
#include <syscall.h> // struct wev / struct wsurf_info / WSURF_* (the ABI)

struct page_directory;

// Syscall backends (kernel/wsurf.c). The int-returning calls give back the
// mapped user VA (wsurf_create/wsurf_map) or 0/-1.
int wsurf_create(int w, int h);
int wsurf_event(struct wev *out);
int wsurf_stat(int slot, struct wsurf_info *out);
int wsurf_map(int slot);
int wsurf_send(int slot, const struct wev *ev);
int wsurf_unmap(int slot);

// Death hook: tears down the dying task's surface and/or server mappings.
// Runs inside the exit()/task_kill() cli sections, before the reaper frees
// the task's directory -- the surface pixels are kernel memory aliased into
// user space, and free_directory would push those frames into the allocator
// bitmap if they were still mapped.
void wsurf_task_exit(int task_id, struct page_directory *dir);

#endif
