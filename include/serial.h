#ifndef __SERIAL_H__
#define __SERIAL_H__

void serial_init(void);
char serial_read(void);
int serial_transmit_empty(void);
void serial_write_c(char a);
void serial_write_str(const char *str);

#endif