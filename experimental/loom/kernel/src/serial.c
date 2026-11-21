/* Serial output on COM1 (16550 UART at port 0x3F8). Polled, no interrupts.
 * QEMU's -serial stdio shows these bytes on the host terminal. */
#include <stdint.h>
#include "loom_kernel.h"

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* no interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8 data bits, no parity, 1 stop */
    outb(COM1 + 2, 0xC7);   /* FIFO on, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B);   /* RTS/DSR set */
}

void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) { }   /* wait for transmit holding register empty */
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_putu64(uint64_t v) {
    char buf[21];
    int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) serial_putc(buf[--i]);
}

void serial_puthex64(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    serial_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) serial_putc(digits[(v >> shift) & 0xF]);
}
