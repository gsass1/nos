// Tiny software-rendering helpers for NOS user programs. Header-only, like
// ulib.h. All drawing clips against the target's bounds, so callers may draw
// partially (or fully) off-screen shapes safely.
#ifndef __GFX_H__
#define __GFX_H__

#include "ulib.h"

struct gfx
{
    unsigned int *buf; // 32bpp 0x00RRGGBB
    int w, h;          // dimensions; stride is w pixels
};

static unsigned char gfx_font[8192]; // 256 glyphs x 32 bytes (8x16 in rows 0-15)

static inline int gfx_init_font(void)
{
    return getfont(gfx_font);
}

static inline void gfx_fill(struct gfx *g, int x, int y, int w, int h,
                            unsigned int color)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > g->w ? g->w : x + w;
    int y1 = y + h > g->h ? g->h : y + h;
    for (int yy = y0; yy < y1; yy++) {
        unsigned int *row = g->buf + yy * g->w;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = color;
        }
    }
}

// 1px border along the inside edge of the rect.
static inline void gfx_frame(struct gfx *g, int x, int y, int w, int h,
                             unsigned int color)
{
    gfx_fill(g, x, y, w, 1, color);
    gfx_fill(g, x, y + h - 1, w, 1, color);
    gfx_fill(g, x, y, 1, h, color);
    gfx_fill(g, x + w - 1, y, 1, h, color);
}

// Draw one 8x16 glyph, transparent background.
static inline void gfx_char(struct gfx *g, int x, int y, char ch,
                            unsigned int fg)
{
    const unsigned char *glyph = &gfx_font[(unsigned char)ch * 32];
    for (int row = 0; row < 16; row++) {
        int yy = y + row;
        if (yy < 0 || yy >= g->h) {
            continue;
        }
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80 >> col))) {
                continue;
            }
            int xx = x + col;
            if (xx >= 0 && xx < g->w) {
                g->buf[yy * g->w + xx] = fg;
            }
        }
    }
}

static inline void gfx_text(struct gfx *g, int x, int y, const char *s,
                            unsigned int fg)
{
    for (int i = 0; s[i]; i++) {
        gfx_char(g, x + i * 8, y, s[i], fg);
    }
}

// Classic arrow cursor, 8x14: white fill with a black drop shadow.
static inline void gfx_cursor(struct gfx *g, int x, int y)
{
    static const unsigned char arrow[14] = {
        0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF,
        0xF8, 0xD8, 0x8C, 0x0C, 0x06, 0x06,
    };
    for (int pass = 0; pass < 2; pass++) {
        int ox = pass ? 0 : 1, oy = pass ? 0 : 1;
        unsigned int color = pass ? 0x00FFFFFF : 0x00000000;
        for (int row = 0; row < 14; row++) {
            for (int col = 0; col < 8; col++) {
                if (!(arrow[row] & (0x80 >> col))) {
                    continue;
                }
                int xx = x + col + ox, yy = y + row + oy;
                if (xx >= 0 && xx < g->w && yy >= 0 && yy < g->h) {
                    g->buf[yy * g->w + xx] = color;
                }
            }
        }
    }
}

#endif
