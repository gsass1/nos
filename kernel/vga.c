#include <kernel.h>
#include <io.h>
#include <stdint.h>
#include <string.h>
#include <vga.h>

MODULE("VGA ");

static uint16_t *vga_mem = (uint16_t *)0xB8000;
static uint32_t vga_row = 0;
static uint16_t vga_col = 0;
static uint16_t vga_current_color = 15;

static uint16_t make_vga_entry(uint8_t c, uint8_t color)
{
	uint16_t c16 = (uint16_t)c;
	uint16_t color16 = (uint16_t)color;
	return c16 | color16 << 8;
}

void vga_init(void)
{
	vga_clear();
}

void vga_clear(void)
{
	int i;
	int j;

	for(i = 0; i < VGA_HEIGHT; i++) {
		for(j = 0; j < VGA_WIDTH; j++) {
			vga_mem[i * VGA_WIDTH + j] = make_vga_entry(' ', vga_current_color);
		}
	}

	vga_row = 0;
	vga_col = 0;
	vga_set_cursor(0, 0);
}

void vga_set_color(enum vga_color fg, enum vga_color bg)
{
	vga_current_color = fg | bg << 4;
}

void vga_print(const char *str)
{
	unsigned int i;
    unsigned int len = strlen(str);

	for(i = 0; i < len; i++) {
		vga_putc(str[i]);
	}
}

void vga_putc_at(char c, unsigned char color, uint16_t x, uint16_t y)
{
	vga_mem[y * VGA_WIDTH + x] = make_vga_entry(c, color);
}

void vga_putc(char c)
{
	if(c == '\n') {
		vga_row++;
		if(vga_row >= VGA_HEIGHT) {
			vga_scroll();
		}
		vga_col = 0;
		return;
	}

	if(c == '\b') {
		// Move the cursor back one cell. Callers that want to erase send the
		// classic "\b \b" sequence (back, space, back).
		if(vga_col > 0) {
			vga_col--;
		} else if(vga_row > 0) {
			vga_row--;
			vga_col = VGA_WIDTH - 1;
		}
		vga_set_cursor(vga_col, vga_row);
		return;
	}

	vga_putc_at(c, vga_current_color, vga_col, vga_row);

	// Wrap at the right edge; scroll (don't run off-screen) at the bottom.
	// The old post-increment version let a wrapping line on the last row push
	// vga_row past VGA_HEIGHT without scrolling, after which ALL output landed
	// in invisible off-screen VGA memory: the first >80-column line (e.g. a
	// page-fault kill report) appeared to freeze the console for good.
	vga_col++;
	if(vga_col == VGA_WIDTH) {
		vga_col = 0;
		vga_row++;
		if(vga_row >= VGA_HEIGHT) {
			vga_scroll();
		}
	}
	vga_set_cursor(vga_col, vga_row);
}

void vga_set_cursor(uint16_t x, uint16_t y)
{
	uint16_t pos = (y * VGA_WIDTH) + x;
    // cursor LOW port to vga INDEX register
    outb(0x3D4, 0x0F);
    outb(0x3D5, (unsigned char)(pos & 0xFF));
    // cursor HIGH port to vga INDEX register
    outb(0x3D4, 0x0E);
    outb(0x3D5, (unsigned char)((pos >> 8) & 0xFF));
}

void vga_scroll(void)
{
	// Move rows 1..HEIGHT-1 up by one row, in place. This used to stage the
	// whole screen in a VGA_BUF_SIZE (4000-byte) stack buffer, which overflowed
	// the small per-task kernel stacks and corrupted the heap below them.
	// memcpy copies forward and dest < src, so the overlap is safe.
	memcpy(vga_mem, vga_mem + VGA_WIDTH, VGA_WIDTH * (VGA_HEIGHT - 1) * 2);

	// Blank the last row.
	for(int j = 0; j < VGA_WIDTH; j++) {
		vga_mem[(VGA_HEIGHT - 1) * VGA_WIDTH + j] = make_vga_entry(' ', vga_current_color);
	}

	vga_col = 0;
	vga_row = VGA_HEIGHT - 1;
}
