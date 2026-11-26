/* io.h — x86 port I/O. `in`/`out` instructions talk to devices such as the serial port,
 * the PIC and the PIT. They are not memory accesses, so they need inline assembly. */
#ifndef HELIX_I686_IO_H
#define HELIX_I686_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* A write to port 0x80 takes about a microsecond on the ISA bus: a cheap delay between
 * the steps of a PIC or PIT setup that the chip needs time to digest. */
static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif /* HELIX_I686_IO_H */
