// Window surfaces: the kernel half of the milestone-4 windowing split. A
// client task gets one offscreen 32bpp pixel buffer mapped into its address
// space; the display server (wm) maps the same pixels read-write, composites
// them into a window each frame, and forwards input back as events. The
// pixel storage is a static kernel pool aliased into user space, so there is
// no frame allocation -- but the aliases MUST be unmapped when either side
// dies, before free_directory walks the dying task's tables: it would push
// the pool's (kernel) frames into the frame-allocator bitmap otherwise.
#include <wsurf.h>
#include <elf.h>
#include <kernel.h>
#include <mm.h>
#include <string.h>
#include <task.h>

MODULE("WSRF");

#define WSURF_BYTES (WSURF_MAX_W * WSURF_MAX_H * 4)
#define WEVQ 32

static uint8_t pool[WSURF_SLOTS][WSURF_BYTES] __attribute__((aligned(4096)));

struct wsurf
{
    int used;   // slot allocated (stays set until both sides are gone)
    int alive;  // owner task still running
    int owner;  // client task id
    int server; // task id holding the server mapping, or -1
    int w, h;
    char name[16];
    // Event queue, server -> client. evh = next write, evt = next read.
    struct wev evq[WEVQ];
    int evh, evt;
};

static struct wsurf surfs[WSURF_SLOTS];

static uint32_t surf_map_bytes(const struct wsurf *s)
{
    return ((uint32_t)s->w * s->h * 4 + 0xFFF) & ~0xFFFU;
}

static void map_range(struct page_directory *dir, uint32_t va, uint32_t phys,
                      uint32_t bytes)
{
    for (uint32_t off = 0; off < bytes; off += 0x1000) {
        struct page *pg = get_page(va + off, 1, dir);
        pg->present = 1;
        pg->rw = 1;
        pg->user = 1;
        pg->frame = (phys + off) >> 12;
    }
}

// Clear the alias PTEs without touching the frames (they are kernel memory).
static void unmap_range(struct page_directory *dir, uint32_t va, uint32_t bytes)
{
    for (uint32_t off = 0; off < bytes; off += 0x1000) {
        struct page *pg = get_page(va + off, 0, dir);
        if (pg) {
            pg->present = 0;
            pg->frame = 0;
        }
    }
}

int wsurf_create(int w, int h)
{
    struct task *t = task_current();
    if (!t->brk /* kernel thread */ || w < 1 || h < 1 ||
        w > WSURF_MAX_W || h > WSURF_MAX_H) {
        return -1;
    }

    int slot = -1;
    asm volatile("cli");
    for (int i = 0; i < WSURF_SLOTS; i++) {
        if (surfs[i].used && surfs[i].owner == t->id && surfs[i].alive) {
            asm volatile("sti");
            return -1; // one surface per task
        }
        if (slot < 0 && !surfs[i].used) {
            slot = i;
        }
    }
    if (slot < 0) {
        asm volatile("sti");
        return -1;
    }
    struct wsurf *s = &surfs[slot];
    s->used = 1;
    s->alive = 1;
    s->owner = t->id;
    s->server = -1;
    s->w = w;
    s->h = h;
    s->evh = s->evt = 0;
    int i = 0;
    while (t->name[i] && i < (int)sizeof(s->name) - 1) {
        s->name[i] = t->name[i];
        i++;
    }
    s->name[i] = '\0';
    asm volatile("sti");

    memset(pool[slot], 0, (uint32_t)w * h * 4);
    map_range(t->page_directory, USER_WSURF_BASE, (uint32_t)pool[slot],
              surf_map_bytes(s));
    return (int)USER_WSURF_BASE;
}

int wsurf_event(struct wev *out)
{
    struct task *t = task_current();
    for (int i = 0; i < WSURF_SLOTS; i++) {
        struct wsurf *s = &surfs[i];
        if (!s->used || s->owner != t->id || !s->alive) {
            continue;
        }
        asm volatile("cli");
        if (s->evt == s->evh) {
            asm volatile("sti");
            return -1;
        }
        *out = s->evq[s->evt];
        s->evt = (s->evt + 1) % WEVQ;
        asm volatile("sti");
        return 0;
    }
    return -1;
}

int wsurf_stat(int slot, struct wsurf_info *out)
{
    if (slot < 0 || slot >= WSURF_SLOTS || !surfs[slot].used) {
        return -1;
    }
    struct wsurf *s = &surfs[slot];
    out->alive = s->alive;
    out->w = s->w;
    out->h = s->h;
    out->owner = s->owner;
    for (int i = 0; i < (int)sizeof(out->name); i++) {
        out->name[i] = s->name[i];
    }
    return 0;
}

int wsurf_map(int slot)
{
    struct task *t = task_current();
    if (slot < 0 || slot >= WSURF_SLOTS || !surfs[slot].used || !t->brk) {
        return -1;
    }
    struct wsurf *s = &surfs[slot];
    if (s->server != -1 && s->server != t->id) {
        return -1; // one server at a time
    }
    s->server = t->id;
    uint32_t va = USER_WSRV_BASE + (uint32_t)slot * WSURF_SLOT_SPAN;
    map_range(t->page_directory, va, (uint32_t)pool[slot], surf_map_bytes(s));
    return (int)va;
}

int wsurf_send(int slot, const struct wev *ev)
{
    struct task *t = task_current();
    if (slot < 0 || slot >= WSURF_SLOTS || !surfs[slot].used ||
        surfs[slot].server != t->id || !surfs[slot].alive) {
        return -1;
    }
    struct wsurf *s = &surfs[slot];
    asm volatile("cli");
    // Coalesce mouse moves: overwrite the newest queued event if it is also
    // a mouse event, so a busy client never drowns in stale positions.
    int pending = (s->evh - s->evt + WEVQ) % WEVQ;
    if (ev->type == WEV_MOUSE && pending > 0 &&
        s->evq[(s->evh + WEVQ - 1) % WEVQ].type == WEV_MOUSE) {
        s->evq[(s->evh + WEVQ - 1) % WEVQ] = *ev;
    } else if (pending < WEVQ - 1) {
        s->evq[s->evh] = *ev;
        s->evh = (s->evh + 1) % WEVQ;
    } // else: queue full, drop (matches the keyboard buffer's discipline)
    asm volatile("sti");
    return 0;
}

int wsurf_unmap(int slot)
{
    struct task *t = task_current();
    if (slot < 0 || slot >= WSURF_SLOTS || !surfs[slot].used ||
        surfs[slot].server != t->id) {
        return -1;
    }
    struct wsurf *s = &surfs[slot];
    unmap_range(t->page_directory, USER_WSRV_BASE + (uint32_t)slot * WSURF_SLOT_SPAN,
                surf_map_bytes(s));
    s->server = -1;
    if (!s->alive) {
        s->used = 0; // both sides gone: slot reusable
    }
    return 0;
}

void wsurf_task_exit(int task_id, struct page_directory *dir)
{
    for (int i = 0; i < WSURF_SLOTS; i++) {
        struct wsurf *s = &surfs[i];
        if (!s->used) {
            continue;
        }
        if (s->owner == task_id && s->alive) {
            s->alive = 0;
            unmap_range(dir, USER_WSURF_BASE, surf_map_bytes(s));
            if (s->server == -1) {
                s->used = 0;
            }
        }
        if (s->server == task_id) {
            unmap_range(dir, USER_WSRV_BASE + (uint32_t)i * WSURF_SLOT_SPAN,
                        surf_map_bytes(s));
            s->server = -1;
            if (!s->alive) {
                s->used = 0;
            }
        }
    }
}
