#ifndef __PIPE_H__
#define __PIPE_H__

#include <stdint.h>

// A pipe is a kmalloc'd byte ring referenced by FD_PIPE_R/FD_PIPE_W file
// objects in any number of tasks. It counts live read and write ends:
// reading an empty pipe returns EOF (0) once every write end is closed,
// writing fails once every read end is closed, and the object is freed when
// the last end of either kind goes away. Ends are duplicated by fd
// inheritance (spawn_task) and dropped by file_close().

struct pipe;

// Fresh pipe holding one reader and one writer reference.
struct pipe *pipe_create(void);

void pipe_addref(struct pipe *p, int writable);

// Drop one end; frees the pipe when no ends of either kind remain.
void pipe_release(struct pipe *p, int writable);

// Both block by yielding, like every other wait in the kernel. read returns
// as soon as at least one byte is available and 0 at EOF; write returns len
// once everything is buffered, or -1 when no read ends remain.
int pipe_read(struct pipe *p, char *buf, uint32_t len);
int pipe_write(struct pipe *p, const char *buf, uint32_t len);

#endif
