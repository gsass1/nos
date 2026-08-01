#include <keyboard.h>
#include <kernel.h>
#include <io.h>
#include <mm.h>
#include <string.h>

MODULE("KBRD");

enum keycode {
    NULL_KEY = 0,
    Q_PRESSED = 0x10,
    Q_RELEASED = 0x90,
    W_PRESSED = 0x11,
    W_RELEASED = 0x91,
    E_PRESSED = 0x12,
    E_RELEASED = 0x92,
    R_PRESSED = 0x13,
    R_RELEASED = 0x93,
    T_PRESSED = 0x14,
    T_RELEASED = 0x94,
    Y_PRESSED = 0x15,
    Y_RELEASED = 0x95,
    U_PRESSED = 0x16,
    U_RELEASED = 0x96,
    I_PRESSED = 0x17,
    I_RELEASED = 0x97,
    O_PRESSED = 0x18,
    O_RELEASED = 0x98,
    P_PRESSED = 0x19,
    P_RELEASED = 0x99,
    A_PRESSED = 0x1E,
    A_RELEASED = 0x9E,
    S_PRESSED = 0x1F,
    S_RELEASED = 0x9F,
    D_PRESSED = 0x20,
    D_RELEASED = 0xA0,
    F_PRESSED = 0x21,
    F_RELEASED = 0xA1,
    G_PRESSED = 0x22,
    G_RELEASED = 0xA2,
    H_PRESSED = 0x23,
    H_RELEASED = 0xA3,
    J_PRESSED = 0x24,
    J_RELEASED = 0xA4,
    K_PRESSED = 0x25,
    K_RELEASED = 0xA5,
    L_PRESSED = 0x26,
    L_RELEASED = 0xA6,
    Z_PRESSED = 0x2C,
    Z_RELEASED = 0xAC,
    X_PRESSED = 0x2D,
    X_RELEASED = 0xAD,
    C_PRESSED = 0x2E,
    C_RELEASED = 0xAE,
    V_PRESSED = 0x2F,
    V_RELEASED = 0xAF,
    B_PRESSED = 0x30,
    B_RELEASED = 0xB0,
    N_PRESSED = 0x31,
    N_RELEASED = 0xB1,
    M_PRESSED = 0x32,
    M_RELEASED = 0xB2,

    ZERO_PRESSED = 0x29,
    ONE_PRESSED = 0x2,
    NINE_PRESSED = 0xA,

    POINT_PRESSED = 0x34,
    POINT_RELEASED = 0xB4,

    SLASH_RELEASED = 0xB5,

    BACKSPACE_PRESSED = 0xE,
    BACKSPACE_RELEASED = 0x8E,
    SPACE_PRESSED = 0x39,
    SPACE_RELEASED = 0xB9,
    ENTER_PRESSED = 0x1C,
    ENTER_RELEASED = 0x9C,
};

// Circular scancode->ascii buffer. Producer is kbd_irq, consumer is kbd_getc.
// Bounded so a burst of key events can never write past it (the old code grew
// keyloc without limit and scribbled past a 256-byte heap buffer, corrupting
// the next allocation).
#define KBD_BUF_SIZE 256
static uint8_t keybuf[KBD_BUF_SIZE];
static uint32_t kbd_head = 0; // next write slot
static uint32_t kbd_tail = 0; // next read slot

// QWERTY (US) layout tables, indexed by scancode offset within each row.
static char* _qwertyuiop = "qwertyuiop"; // 0x10-0x19
static char* _asdfghjkl = "asdfghjkl";   // 0x1E-0x26
static char* _zxcvbnm = "zxcvbnm";       // 0x2C-0x32
static char* _num = "123456789";

static uint8_t to_ascii(uint8_t key)
{
    if(key == 0x1C) return '\n';
    if(key == 0x39) return ' ';
    if(key == 0xE) return '\b';
    if(key == POINT_RELEASED) return '.';
    if(key == SLASH_RELEASED) return '/';
    if(key == ZERO_PRESSED) return '0';
    if(key >= ONE_PRESSED && key <= NINE_PRESSED)
        return _num[key - ONE_PRESSED];
    // 0x10-0x19 is q..p. 0x1A/0x1B ('['/']') are NOT in the string; including
    // them read past "qwertyuiop" into the neighbouring rodata literal.
    if(key >= 0x10 && key <= 0x19)
    {
        return _qwertyuiop[key - 0x10];
    }
    else if(key >= 0x1E && key <= 0x26)
    {
        return _asdfghjkl[key - 0x1E];
    }
    else if(key >= 0x2C && key <= 0x32)
    {
        return _zxcvbnm[key - 0x2C];
    }
    return 0;
}

void kbd_irq(void)
{
    uint8_t code = inb(0x60);
    uint8_t c = to_ascii(code);
    if(c == 0) {
        return; // key release or unmapped key
    }

    uint32_t next = (kbd_head + 1) % KBD_BUF_SIZE;
    if(next != kbd_tail) { // drop the key if the buffer is full
        keybuf[kbd_head] = c;
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