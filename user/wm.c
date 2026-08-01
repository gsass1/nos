// wm -- a single-process desktop for NOS: teal desktop, taskbar, draggable
// overlapping windows with focus/z-order, live keyboard echo into the focused
// window, and a mouse cursor. Everything renders into an sbrk'd backbuffer
// that is copied to the framebuffer each frame (~60fps).
//
// This is GUI milestone 3: one process owns the screen and manages its own
// windows. Multi-app windowing (milestone 4) will split this into a window
// server + clients over IPC.
#include "ulib.h"
#include "gfx.h"

#define COL_DESKTOP  0x00008080
#define COL_TASKBAR  0x00C0C0C0
#define COL_TITLE    0x00000080
#define COL_TITLE_BG 0x00808080
#define COL_BODY     0x00FFFFFF
#define COL_TEXT     0x00000000
#define COL_BORDER   0x00000000

#define TITLE_H   24
#define TASKBAR_H 32

struct win
{
    int x, y, w, h;
    const char *title;
};

#define NWIN 2
static struct win wins[NWIN] = {
    { 140, 120, 400, 300, "welcome" },
    { 420, 260, 360, 240, "notes" },
};
// Draw order, bottom to top; the last entry has focus.
static int zorder[NWIN] = { 0, 1 };

static struct gfx bb;             // backbuffer
static volatile unsigned int *fb; // real framebuffer
static struct fb_info info;

static char typed[48];
static int typed_len;

static void raise_win(int zi)
{
    int w = zorder[zi];
    for (int i = zi; i < NWIN - 1; i++) {
        zorder[i] = zorder[i + 1];
    }
    zorder[NWIN - 1] = w;
}

static void draw_window(struct win *w, int focused)
{
    gfx_fill(&bb, w->x, w->y, w->w, w->h, COL_BODY);
    gfx_fill(&bb, w->x, w->y, w->w, TITLE_H, focused ? COL_TITLE : COL_TITLE_BG);
    gfx_frame(&bb, w->x, w->y, w->w, w->h, COL_BORDER);
    gfx_text(&bb, w->x + 8, w->y + 4, w->title, 0x00FFFFFF);

    gfx_text(&bb, w->x + 12, w->y + TITLE_H + 12,
             "NOS window - drag my title bar", COL_TEXT);
    if (focused) {
        gfx_text(&bb, w->x + 12, w->y + TITLE_H + 36, "type: ", COL_TEXT);
        gfx_text(&bb, w->x + 12 + 6 * 8, w->y + TITLE_H + 36, typed, COL_TEXT);
    }
}

static void draw_all(int cx, int cy)
{
    gfx_fill(&bb, 0, 0, bb.w, bb.h, COL_DESKTOP);

    for (int i = 0; i < NWIN; i++) {
        draw_window(&wins[zorder[i]], i == NWIN - 1);
    }

    // Taskbar with one entry per window; the focused one is highlighted.
    int ty = bb.h - TASKBAR_H;
    gfx_fill(&bb, 0, ty, bb.w, TASKBAR_H, COL_TASKBAR);
    gfx_fill(&bb, 0, ty, bb.w, 1, 0x00FFFFFF);
    for (int i = 0; i < NWIN; i++) {
        int ex = 4 + i * 160;
        int focused = zorder[NWIN - 1] == i;
        gfx_fill(&bb, ex, ty + 4, 152, TASKBAR_H - 8,
                 focused ? 0x00E0E0E0 : 0x00A0A0A0);
        gfx_frame(&bb, ex, ty + 4, 152, TASKBAR_H - 8, COL_BORDER);
        gfx_text(&bb, ex + 8, ty + 8, wins[i].title, COL_TEXT);
    }
    gfx_text(&bb, bb.w - 152, ty + 8, "q quits to shell", COL_TEXT);

    gfx_cursor(&bb, cx, cy);

    // Present.
    int n = bb.w * bb.h;
    for (int i = 0; i < n; i++) {
        fb[i] = bb.buf[i];
    }
}

void _start(void)
{
    if (fbinfo(&info) != 0 || gfx_init_font() != 0) {
        put("wm: no framebuffer\n");
        exit(1);
    }
    bb.w = info.width;
    bb.h = info.height;
    bb.buf = sbrk(bb.w * bb.h * 4);
    if (bb.buf == (unsigned int *)-1) {
        put("wm: out of memory\n");
        exit(1);
    }
    fb = fbmap();
    if (fb == (volatile unsigned int *)-1) {
        put("wm: map failed\n");
        exit(1);
    }

    struct mouse_state m, prev;
    mouse(&prev);
    int dragging = -1; // window index being dragged
    int drag_ox = 0, drag_oy = 0;

    draw_all(prev.x, prev.y);
    put("wm: started\n");

    for (;;) {
        mouse(&m);

        int c;
        while ((c = pollc()) != -1) {
            if (c == 'q' || c == 27) {
                goto out;
            }
            if (c == '\b') {
                if (typed_len > 0) {
                    typed[--typed_len] = '\0';
                }
            } else if (c >= 32 && c < 127 &&
                       typed_len < (int)sizeof(typed) - 1) {
                typed[typed_len++] = (char)c;
                typed[typed_len] = '\0';
            }
        }

        int pressed = (m.buttons & 1) && !(prev.buttons & 1);
        int released = !(m.buttons & 1) && (prev.buttons & 1);

        if (pressed) {
            if (m.y >= bb.h - TASKBAR_H) {
                // Taskbar entry: raise that window.
                for (int i = 0; i < NWIN; i++) {
                    int ex = 4 + i * 160;
                    if (m.x >= ex && m.x < ex + 152) {
                        for (int z = 0; z < NWIN; z++) {
                            if (zorder[z] == i) {
                                raise_win(z);
                                break;
                            }
                        }
                        break;
                    }
                }
            } else {
                // Topmost window under the cursor gets focus; a title-bar
                // hit also starts a drag.
                for (int z = NWIN - 1; z >= 0; z--) {
                    struct win *w = &wins[zorder[z]];
                    if (m.x >= w->x && m.x < w->x + w->w &&
                        m.y >= w->y && m.y < w->y + w->h) {
                        if (m.y < w->y + TITLE_H) {
                            dragging = zorder[z];
                            drag_ox = m.x - w->x;
                            drag_oy = m.y - w->y;
                        }
                        raise_win(z);
                        break;
                    }
                }
            }
        }
        if (dragging >= 0) {
            struct win *w = &wins[dragging];
            w->x = m.x - drag_ox;
            w->y = m.y - drag_oy;
            // Keep the window fully on the desktop (above the taskbar).
            if (w->x < 0) w->x = 0;
            if (w->y < 0) w->y = 0;
            if (w->x > bb.w - w->w) w->x = bb.w - w->w;
            if (w->y > bb.h - TASKBAR_H - w->h) w->y = bb.h - TASKBAR_H - w->h;
        }
        if (released) {
            dragging = -1;
        }

        prev = m;
        draw_all(m.x, m.y);
        sleep(15);
    }

out:
    fboff();
    put("wm: exit\n");
    exit(0);
}
