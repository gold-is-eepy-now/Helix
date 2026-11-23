/* main.c — i686 kernel, stage I0: check the Multiboot magic, print over COM1, halt.
 *
 * Why COM1 first: serial is the simplest output that works before any framebuffer or
 * memory code exists, and QEMU's -serial stdio shows it directly. */
#include <stdint.h>

#define COM1 0x3F8
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Wait until the transmit holding register is empty (line status bit 5), then send. */
static void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (uint8_t)c);
}

static void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

void kmain(uint32_t multiboot_magic, uint32_t multiboot_info) {
    (void)multiboot_info;               /* I0 does not parse the info block yet */
    if (multiboot_magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        serial_puts("HELIX i686: not loaded by a Multiboot loader\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }
    serial_puts("HELIX OS i686\n");
    serial_puts("Boot successful.\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
