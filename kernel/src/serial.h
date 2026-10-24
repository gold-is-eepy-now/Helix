/* serial.h — 16550 UART serial console and input (COM1). */
#ifndef HELIX_SERIAL_H
#define HELIX_SERIAL_H

#include <stdbool.h>
#include <stdint.h>

/* Bring COM1 up: 38400 baud, 8N1, FIFO enabled, no interrupts. */
void serial_init(void);
/* True when the COM1 UART passed its hardware presence probe. */
bool serial_available(void);

/* Send one character, blocking until the transmit register is free. */
void serial_putc(char c);
/* Non-blocking byte output; false if the transmit register is not ready. */
bool serial_try_putc(char c);

/* Send a NUL-terminated string. */
void serial_write(const char *s);

/* True if a received byte is waiting. */
bool serial_rx_ready(void);

/* Read one byte, or -1 if none is waiting. Never blocks. */
int serial_read(void);

#endif /* HELIX_SERIAL_H */
