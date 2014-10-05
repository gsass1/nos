#include <io.h>
#include <kernel.h>
#include <string.h>
#include <serial.h>

MODULE("SERI");

#define PORT 0x3f8

static int serial_initialized = 0;

void serial_init(void)
{
    mprintf(LOGLEVEL_DEFAULT, "Initializing on COM1\n");

    outb(PORT + 1, 0x00);    // Disable all interrupts
    outb(PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(PORT + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
    outb(PORT + 1, 0x00);    //                  (hi byte)
    outb(PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
    outb(PORT + 1, 0x01);    // Enable interrupts

    serial_initialized = 1;
    serial_write_str("Serial is enabled\n");
}

int serial_received(void)
{
    return inb(PORT + 5) & 1;
}

char serial_read(void)
{
    while(serial_received() == 0);

    return inb(PORT);
}

int serial_transmit_empty(void)
{
    return inb(PORT + 5) & 0x20;
}

void serial_write_c(char a)
{
    while(serial_transmit_empty() == 0);

    outb(PORT, a);
}

void serial_write_str(const char *str)
{
    if(!serial_initialized) {
        return;
    }

    unsigned int i;

    for(i = 0; i < strlen(str); i++) {
        serial_write_c(str[i]);
    }
}