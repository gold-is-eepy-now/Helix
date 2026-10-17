/* io.h — x86 port I/O and tiny CPU helpers.
 *
 * The `in`/`out` instructions are how x86 talks to legacy devices (serial,
 * PS/2 keyboard, the PIT). They cannot be expressed in C, so they live here as
 * inline assembly, shared by every driver.
 */
#ifndef HELIX_IO_H
#define HELIX_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Write to an unused port to waste a few cycles; some old devices need a
 * small delay between consecutive I/O accesses. */
static inline void io_wait(void) {
    outb(0x80, 0);
}

/* Park the CPU until the next interrupt. */
static inline void cpu_halt(void) {
    __asm__ volatile("hlt");
}

/* Mask maskable interrupts. */
static inline void cpu_disable_interrupts(void) {
    __asm__ volatile("cli");
}

#endif /* HELIX_IO_H */
