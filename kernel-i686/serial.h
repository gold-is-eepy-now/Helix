/* serial.h — COM1 output for the i686 kernel (polled, 115200-style defaults from QEMU). */
#ifndef HELIX_I686_SERIAL_H
#define HELIX_I686_SERIAL_H

#include <stdint.h>

void serial_putc(char c);
void serial_puts(const char *s);
void serial_put_hex32(uint32_t value);

#endif /* HELIX_I686_SERIAL_H */
