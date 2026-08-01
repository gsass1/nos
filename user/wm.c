// wm -- a single-process desktop for NOS: teal desktop, taskbar with a start
// menu, dynamically created windows with close/minimize buttons, title-bar
// dragging, corner resizing, focus/z-order, per-window text input, and an
// uptime clock. Renders into an sbrk'd backbuffer copied to the framebuffer
// each frame.
//
// This is the GUI milestone-3 desktop, iterated. Multi-app windowing
// (milestone 4) will split it into a window server + clients over IPC.
#include "ulib.h"
#include "gfx.h"

#define COL_DESKTOP  0x00008080
#define COL_TASKBAR  0x00C0C0C0
#define COL_TITLE    0x00000080
#define COL_TITLE_BG 0x00808080
#define COL_BODY     0x00FFFFFF
#define COL_TEXT     0x00000000
#define COL_BORDER   0x00000000
#define COL_MENU     0x00E8E8E8
#define COL_BTN      0x00C0C0C0

#define TITLE_H   24
#define TASKBAR_H 32
#define BTN_SZ    18   // title-bar button side
#define GRIP_SZ   16   // resize grab area in the bottom-right corner
#define MIN_W     160
#define MIN_H     100

#define MENU_W    160
#define MENU_ITEM_H 24
#define MENU_ITEMS  3
#define MENU_H    (MENU_ITEMS * MENU_ITEM_H + 8)

// Terminal windows: fixed character grid rendered into the body.
#define TCOLS 42
#define TROWS 12
#define COL_TERM_BG 0x00000000
#define COL_TERM_FG 0x00E0E0E0

#define START_W   64   // taskbar start-button width (incl. left margin)
#define ENTRY_X0  72   // first taskbar entry
#define ENTRY_W   152
#define ENTRY_STEP 160

#define MAXWIN 8

struct win
{
    int alive, min;
    int x, y, w, h;
    char title[24];
    char text[40];
    int tlen;
    // Terminal windows host a real process via a console channel.
    int term;
    int cid;
    char grid[TROWS][TCOLS + 1];
    int trow, tcol;
};

static struct win wins[MAXWIN];
static int zorder[MAXWIN]; // bottom to top, alive windows only
static int nz;
static int next_no = 3;    // names for menu-spawned windows
static int menu_open;

static struct gfx bb;             // backbuffer
static volatile unsigned int *fb; // real framebuffer
static struct fb_info info;

static void scopy(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int fmt_int(char *dst, int v)
{
    char tmp[12];
    int n = 0, len = 0;
    unsigned u = v < 0 ? -(unsigned)v : (unsigned)v;
    if (v < 0) {
        dst[len++] = '-';
    }
    do {
        tmp[n++] = '0' + u % 10;
        u /= 10;
    } while (u);
    while (n) {
        dst[len++] = tmp[--n];
    }
    dst[len] = '\0';
    return len;
}

static int zfind(int wi)
{
    for (int z = 0; z < nz; z++) {
        if (zorder[z] == wi) {
            return z;
        }
    }
    return -1;
}

static void raise_win(int wi)
{
    int z = zfind(wi);
    if (z < 0) {
        return;
    }
    for (int i = z; i < nz - 1; i++) {
        zorder[i] = zorder[i + 1];
    }
    zorder[nz - 1] = wi;
}

// Topmost visible (non-minimized) window: it has focus and gets key input.
static int topmost(void)
{
    for (int z = nz - 1; z >= 0; z--) {
        if (!wins[zorder[z]].min) {
            return zorder[z];
        }
    }
    return -1;
}

static void close_win(int wi)
{
    int z = zfind(wi);
    if (z < 0) {
        return;
    }
    for (int i = z; i < nz - 1; i++) {
        zorder[i] = zorder[i + 1];
    }
    nz--;
    wins[wi].alive = 0;
}

static int spawn(const char *title)
{
    int slot = -1;
    for (int i = 0; i < MAXWIN; i++) {
        if (!wins[i].alive) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }
    struct win *w = &wins[slot];
    w->alive = 1;
    w->min = 0;
    w->x = 140 + 40 * slot;
    w->y = 120 + 32 * slot;
    w->w = 360;
    w->h = 240;
    w->text[0] = '\0';
    w->tlen = 0;
    w->term = 0;
    w->cid = -1;
    if (title) {
        scopy(w->title, title, sizeof(w->title));
    } else {
        char *p = w->title;
        scopy(p, "window ", sizeof(w->title));
        fmt_int(p + 7, next_no++);
    }
    zorder[nz++] = slot;
    return slot;
}

static void draw_window(struct win *w, int focused)
{
    gfx_fill(&bb, w->x, w->y, w->w, w->h, COL_BODY);
    gfx_fill(&bb, w->x, w->y, w->w, TITLE_H, focused ? COL_TITLE : COL_TITLE_BG);
    gfx_frame(&bb, w->x, w->y, w->w, w->h, COL_BORDER);
    gfx_text(&bb, w->x + 8, w->y + 4, w->title, 0x00FFFFFF);

    // Title-bar buttons: minimize, then close, right-aligned.
    int by = w->y + 3;
    int bxm = w->x + w->w - 2 * BTN_SZ - 5;
    int bxc = w->x + w->w - BTN_SZ - 3;
    gfx_fill(&bb, bxm, by, BTN_SZ, BTN_SZ, COL_BTN);
    gfx_frame(&bb, bxm, by, BTN_SZ, BTN_SZ, COL_BORDER);
    gfx_char(&bb, bxm + 5, by - 1, '_', COL_TEXT);
    gfx_fill(&bb, bxc, by, BTN_SZ, BTN_SZ, COL_BTN);
    gfx_frame(&bb, bxc, by, BTN_SZ, BTN_SZ, COL_BORDER);
    gfx_char(&bb, bxc + 5, by + 1, 'x', COL_TEXT);

    if (w->term) {
        gfx_fill(&bb, w->x + 1, w->y + TITLE_H, w->w - 2, w->h - TITLE_H - 1,
                 COL_TERM_BG);
        for (int r = 0; r < TROWS; r++) {
            gfx_text(&bb, w->x + 8, w->y + TITLE_H + 6 + r * 16, w->grid[r],
                     COL_TERM_FG);
        }
    } else {
        gfx_text(&bb, w->x + 12, w->y + TITLE_H + 12, "NOS window", COL_TEXT);
        gfx_text(&bb, w->x + 12, w->y + TITLE_H + 36, "type: ", COL_TEXT);
        gfx_text(&bb, w->x + 12 + 6 * 8, w->y + TITLE_H + 36, w->text, COL_TEXT);
    }

    // Resize grip: a few dots in the bottom-right corner.
    for (int i = 0; i < 3; i++) {
        gfx_fill(&bb, w->x + w->w - 5 - i * 4, w->y + w->h - 5, 2, 2, COL_BORDER);
    }
}

static void draw_all(int cx, int cy)
{
    gfx_fill(&bb, 0, 0, bb.w, bb.h, COL_DESKTOP);

    int top = topmost();
    for (int z = 0; z < nz; z++) {
        struct win *w = &wins[zorder[z]];
        if (!w->min) {
            draw_window(w, zorder[z] == top);
        }
    }

    // Taskbar: start button, one entry per window, uptime + help on the right.
    int ty = bb.h - TASKBAR_H;
    gfx_fill(&bb, 0, ty, bb.w, TASKBAR_H, COL_TASKBAR);
    gfx_fill(&bb, 0, ty, bb.w, 1, 0x00FFFFFF);

    gfx_fill(&bb, 4, ty + 4, START_W - 8, TASKBAR_H - 8,
             menu_open ? 0x00E0E0E0 : 0x00D0D0D0);
    gfx_frame(&bb, 4, ty + 4, START_W - 8, TASKBAR_H - 8, COL_BORDER);
    gfx_text(&bb, 12, ty + 8, "NOS", COL_TEXT);

    int k = 0;
    for (int i = 0; i < MAXWIN; i++) {
        if (!wins[i].alive) {
            continue;
        }
        int ex = ENTRY_X0 + k * ENTRY_STEP;
        unsigned int bg = wins[i].min ? 0x00909090
                        : (i == top ? 0x00E0E0E0 : 0x00A0A0A0);
        gfx_fill(&bb, ex, ty + 4, ENTRY_W, TASKBAR_H - 8, bg);
        gfx_frame(&bb, ex, ty + 4, ENTRY_W, TASKBAR_H - 8, COL_BORDER);
        gfx_text(&bb, ex + 8, ty + 8, wins[i].title, COL_TEXT);
        k++;
    }

    char right[32];
    scopy(right, "up ", sizeof(right));
    int len = 3 + fmt_int(right + 3, uptime_ms() / 1000);
    scopy(right + len, "s  esc quits", (int)sizeof(right) - len);
    gfx_text(&bb, bb.w - 8 * 20, ty + 8, right, COL_TEXT);

    if (menu_open) {
        int my = bb.h - TASKBAR_H - MENU_H;
        gfx_fill(&bb, 4, my, MENU_W, MENU_H, COL_MENU);
        gfx_frame(&bb, 4, my, MENU_W, MENU_H, COL_BORDER);
        gfx_text(&bb, 16, my + 8, "terminal", COL_TEXT);
        gfx_text(&bb, 16, my + 8 + MENU_ITEM_H, "new window", COL_TEXT);
        gfx_text(&bb, 16, my + 8 + 2 * MENU_ITEM_H, "about", COL_TEXT);
    }

    gfx_cursor(&bb, cx, cy);

    int n = bb.w * bb.h;
    for (int i = 0; i < n; i++) {
        fb[i] = bb.buf[i];
    }
}

static void term_clear(struct win *w)
{
    for (int r = 0; r < TROWS; r++) {
        for (int c = 0; c < TCOLS; c++) {
            w->grid[r][c] = ' ';
        }
        w->grid[r][TCOLS] = '\0';
    }
    w->trow = 0;
    w->tcol = 0;
}

static void term_scroll(struct win *w)
{
    for (int r = 0; r < TROWS - 1; r++) {
        scopy(w->grid[r], w->grid[r + 1], TCOLS + 1);
    }
    for (int c = 0; c < TCOLS; c++) {
        w->grid[TROWS - 1][c] = ' ';
    }
    w->trow = TROWS - 1;
    w->tcol = 0;
}

static void term_feed(struct win *w, const char *buf, int n)
{
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
            w->tcol = 0;
            if (++w->trow == TROWS) {
                term_scroll(w);
            }
        } else if (c == '\r') {
            w->tcol = 0;
        } else if (c == '\b') {
            if (w->tcol > 0) {
                w->tcol--;
            }
        } else if (c == '\f') { // SYS_CLEAR from inside the terminal
            term_clear(w);
        } else if (c >= 32 && c < 127) {
            w->grid[w->trow][w->tcol] = c;
            if (++w->tcol == TCOLS) {
                w->tcol = 0;
                if (++w->trow == TROWS) {
                    term_scroll(w);
                }
            }
        }
    }
}

static void spawn_terminal(void)
{
    int slot = spawn("terminal");
    if (slot < 0) {
        return;
    }
    struct win *w = &wins[slot];
    term_clear(w);
    w->cid = execc("sh", 0);
    if (w->cid < 0) {
        close_win(slot);
        return;
    }
    w->term = 1;
}

// Ask the shell inside to exit (best effort), then drop the channel.
static void close_terminal(struct win *w)
{
    if (w->cid >= 0) {
        cwrite(w->cid, "exit\n", 5);
        sleep(50); // give it a beat to run the builtin
        cclose(w->cid);
        w->cid = -1;
    }
}

static void spawn_about(void)
{
    int slot = spawn("about");
    if (slot < 0) {
        return;
    }
    char *t = wins[slot].text;
    scopy(t, "NOS ", sizeof(wins[slot].text));
    int len = 4 + fmt_int(t + 4, (int)info.width);
    t[len++] = 'x';
    len += fmt_int(t + len, (int)info.height);
    scopy(t + len, " 32bpp", (int)sizeof(wins[slot].text) - len);
    wins[slot].tlen = slen(t); // typed input appends after the preset
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

    // Initial demo windows (fixed geometry: the test suite asserts on it).
    struct win *w = &wins[0];
    w->alive = 1; w->x = 140; w->y = 120; w->w = 400; w->h = 300;
    scopy(w->title, "welcome", sizeof(w->title));
    w = &wins[1];
    w->alive = 1; w->x = 420; w->y = 260; w->w = 360; w->h = 240;
    scopy(w->title, "notes", sizeof(w->title));
    zorder[0] = 0;
    zorder[1] = 1;
    nz = 2;

    struct mouse_state m, prev;
    mouse(&prev);
    int dragging = -1, resizing = -1;
    int drag_ox = 0, drag_oy = 0;

    draw_all(prev.x, prev.y);
    put("wm: started\n");

    for (;;) {
        mouse(&m);

        int c;
        while ((c = pollc()) != -1) {
            if (c == 27) { // ESC
                goto out;
            }
            int t = topmost();
            if (t < 0) {
                continue;
            }
            struct win *fw = &wins[t];
            if (fw->term) {
                // Forward everything (incl. \n and \b) to the process in the
                // terminal; its own echo comes back through the out ring.
                char ch = (char)c;
                cwrite(fw->cid, &ch, 1);
            } else if (c == '\b') {
                if (fw->tlen > 0) {
                    fw->text[--fw->tlen] = '\0';
                }
            } else if (c >= 32 && c < 127 &&
                       fw->tlen < (int)sizeof(fw->text) - 1) {
                fw->text[fw->tlen++] = (char)c;
                fw->text[fw->tlen] = '\0';
            }
        }

        int pressed = (m.buttons & 1) && !(prev.buttons & 1);
        int released = !(m.buttons & 1) && (prev.buttons & 1);

        if (pressed && menu_open) {
            int my = bb.h - TASKBAR_H - MENU_H;
            if (m.x >= 4 && m.x < 4 + MENU_W && m.y >= my && m.y < my + MENU_H) {
                int item = (m.y - my - 4) / MENU_ITEM_H;
                if (item == 0) {
                    spawn_terminal();
                } else if (item == 1) {
                    spawn(0);
                } else if (item == 2) {
                    spawn_about();
                }
            }
            menu_open = 0; // any click closes the menu, consuming the press
        } else if (pressed && m.y >= bb.h - TASKBAR_H) {
            if (m.x < START_W + 4) {
                menu_open = 1;
            } else {
                // Map the click onto the taskbar entry sequence.
                int k = 0;
                for (int i = 0; i < MAXWIN; i++) {
                    if (!wins[i].alive) {
                        continue;
                    }
                    int ex = ENTRY_X0 + k * ENTRY_STEP;
                    if (m.x >= ex && m.x < ex + ENTRY_W) {
                        if (wins[i].min) {
                            wins[i].min = 0;
                            raise_win(i);
                        } else if (i == topmost()) {
                            wins[i].min = 1; // click focused entry: minimize
                        } else {
                            raise_win(i);
                        }
                        break;
                    }
                    k++;
                }
            }
        } else if (pressed) {
            for (int z = nz - 1; z >= 0; z--) {
                struct win *hw = &wins[zorder[z]];
                if (hw->min ||
                    m.x < hw->x || m.x >= hw->x + hw->w ||
                    m.y < hw->y || m.y >= hw->y + hw->h) {
                    continue;
                }
                int wi = zorder[z];
                if (m.y < hw->y + TITLE_H) {
                    int by = hw->y + 3;
                    int bxm = hw->x + hw->w - 2 * BTN_SZ - 5;
                    int bxc = hw->x + hw->w - BTN_SZ - 3;
                    int in_y = m.y >= by && m.y < by + BTN_SZ;
                    if (in_y && m.x >= bxc && m.x < bxc + BTN_SZ) {
                        if (hw->term) {
                            close_terminal(hw);
                        }
                        close_win(wi);
                    } else if (in_y && m.x >= bxm && m.x < bxm + BTN_SZ) {
                        wins[wi].min = 1;
                        raise_win(wi);
                    } else {
                        dragging = wi;
                        drag_ox = m.x - hw->x;
                        drag_oy = m.y - hw->y;
                        raise_win(wi);
                    }
                } else if (m.x >= hw->x + hw->w - GRIP_SZ &&
                           m.y >= hw->y + hw->h - GRIP_SZ) {
                    resizing = wi;
                    drag_ox = hw->x + hw->w - m.x;
                    drag_oy = hw->y + hw->h - m.y;
                    raise_win(wi);
                } else {
                    raise_win(wi);
                }
                break;
            }
        }

        if (dragging >= 0 && wins[dragging].alive) {
            struct win *dw = &wins[dragging];
            dw->x = m.x - drag_ox;
            dw->y = m.y - drag_oy;
            if (dw->x < 0) dw->x = 0;
            if (dw->y < 0) dw->y = 0;
            if (dw->x > bb.w - dw->w) dw->x = bb.w - dw->w;
            if (dw->y > bb.h - TASKBAR_H - dw->h) dw->y = bb.h - TASKBAR_H - dw->h;
        }
        if (resizing >= 0 && wins[resizing].alive) {
            struct win *rw = &wins[resizing];
            int nw = m.x + drag_ox - rw->x;
            int nh = m.y + drag_oy - rw->y;
            if (nw < MIN_W) nw = MIN_W;
            if (nh < MIN_H) nh = MIN_H;
            if (rw->x + nw > bb.w) nw = bb.w - rw->x;
            if (rw->y + nh > bb.h - TASKBAR_H) nh = bb.h - TASKBAR_H - rw->y;
            rw->w = nw;
            rw->h = nh;
        }
        if (released) {
            dragging = -1;
            resizing = -1;
        }

        // Drain every terminal's process output into its grid.
        for (int i = 0; i < MAXWIN; i++) {
            if (wins[i].alive && wins[i].term && wins[i].cid >= 0) {
                char cbuf[256];
                int n;
                while ((n = cread(wins[i].cid, cbuf, sizeof(cbuf))) > 0) {
                    term_feed(&wins[i], cbuf, n);
                }
            }
        }

        prev = m;
        draw_all(m.x, m.y);
        sleep(15);
    }

out:
    // Ask any shells still running in terminals to exit before we leave.
    for (int i = 0; i < MAXWIN; i++) {
        if (wins[i].alive && wins[i].term) {
            close_terminal(&wins[i]);
        }
    }
    fboff();
    put("wm: exit\n");
    exit(0);
}
