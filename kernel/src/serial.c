/* serial.c — minimal 16550 UART driver for the COM1 port (I/O port 0x3F8).
 *
 * This is deliberately the simplest possible device driver: it never uses
 * interrupts, it just polls. That is slow but it cannot lose output, and it
 * works before the interrupt subsystem exists. It now also does input, which
 * is how an interactive shell can be typed into over a serial terminal.
 */
#include "serial.h"

#include "io.h"

#define COM1 0x3F8u

void serial_init(void) {
    outb(COM1 + 1, 0x00); /* Disable UART interrupts.            */
    outb(COM1 + 3, 0x80); /* Set DLAB to program the baud rate.  */
    outb(COM1 + 0, 0x03); /* Divisor low byte  (3 -> 38400 baud).*/
    outb(COM1 + 1, 0x00); /* Divisor high byte.                  */
    outb(COM1 + 3, 0x03); /* 8 data bits, no parity, 1 stop bit. */
    outb(COM1 + 2, 0xC7); /* Enable + clear FIFO, 14-byte level. */
    outb(COM1 + 4, 0x0B); /* RTS/DSR set, IRQs still disabled.   */
}

/* Bit 5 of the Line Status Register is set while the transmit buffer is
 * empty and ready to accept the next byte. */
static bool serial_tx_ready(void) {
    return (inb(COM1 + 5) & 0x20) != 0;
}

/* Bit 0 of the Line Status Register is set while a received byte is waiting. */
bool serial_rx_ready(void) {
    return (inb(COM1 + 5) & 0x01) != 0;
}

void serial_putc(char c) {
    while (!serial_tx_ready()) {
        /* spin */
    }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    for (; *s != '\0'; s++) {
        serial_putc(*s);
    }
}

int serial_read(void) {
    if (!serial_rx_ready()) {
        return -1;
    }
    return (int)inb(COM1);
}
