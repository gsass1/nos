#include <keyboard.h>
#include <kernel.h>
#include <io.h>
#include <mm.h>
#include <string.h>

MODULE("KBRD");

// Circular scancode->ascii buffer. Producer is kbd_irq, consumer is kbd_getc.
// Bounded so a burst of key events can never write past it (the old code grew
// keyloc without limit and scribbled past a 256-byte heap buffer, corrupting
// the next allocation).
#define KBD_BUF_SIZE 256
static uint8_t keybuf[KBD_BUF_SIZE];
static uint32_t kbd_head = 0; // next write slot
static uint32_t kbd_tail = 0; // next read slot

// US QWERTY, scancode set 1 (what the i8042 emits by default), indexed by
// make code. 0 = no printable character (modifiers, F-keys, unmapped).
static const char keymap_normal[128] = {
      0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
      0, /* lctrl */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
      0, /* lshift */
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
      0, /* rshift */
    '*',
      0, /* lalt */
    ' ',
};

static const char keymap_shift[128] = {
      0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
      0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
      0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
      0,
    '*',
      0,
    ' ',
};

#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_CAPSLOCK 0x3A

static int shift_down;
static int caps_on;
static int ext_pending; // last byte was the 0xE0 extended-scancode prefix

void kbd_irq(void)
{
    uint8_t code = inb(0x60);

    if(code == 0xE0) {
        ext_pending = 1; // arrows/etc: swallow the pair for now
        return;
    }
    if(ext_pending) {
        ext_pending = 0;
        return;
    }

    if(code & 0x80) { // key release
        uint8_t make = code & 0x7F;
        if(make == SC_LSHIFT || make == SC_RSHIFT) {
            shift_down = 0;
        }
        return;
    }

    if(code == SC_LSHIFT || code == SC_RSHIFT) {
        shift_down = 1;
        return;
    }
    if(code == SC_CAPSLOCK) {
        caps_on = !caps_on;
        return;
    }

    char c = keymap_normal[code];
    if(c >= 'a' && c <= 'z') {
        // Caps lock inverts shift, but only for letters.
        if(shift_down != caps_on) {
            c = keymap_shift[code];
        }
    } else if(shift_down) {
        c = keymap_shift[code];
    }
    if(c == 0) {
        return; // modifier or unmapped key
    }

    uint32_t next = (kbd_head + 1) % KBD_BUF_SIZE;
    if(next != kbd_tail) { // drop the key if the buffer is full
        keybuf[kbd_head] = (uint8_t)c;
        kbd_head = next;
    }
}

void kbd_init(void)
{
    mprintf(LOGLEVEL_DEFAULT, "Initializing PS/2 Keyboard\n");

    kbd_head = kbd_tail = 0;

    outb(0x60, 0xF4);  // Enable scanning (keyboard command via data port 0x60)
}

char kbd_getc(void)
{
    if(kbd_tail == kbd_head) {
        return 0; // buffer empty
    }
    uint8_t key = keybuf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return key;
}