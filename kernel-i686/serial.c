/* serial.c — polled COM1 output. Polling is enough until interrupts carry serial input. */
#include "serial.h"

#include "io.h"

#define COM1 0x3F8

/* Wait until the transmit holding register is empty (line status bit 5), then send. */
void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_put_hex32(uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        serial_putc(digits[(value >> shift) & 0xF]);
    }
}
