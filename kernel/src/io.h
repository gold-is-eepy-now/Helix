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
    __asm__ volatile("cli" : : : "memory");
}

/* Unmask maskable interrupts. */
static inline void cpu_enable_interrupts(void) {
    __asm__ volatile("sti" : : : "memory");
}

/* Read the RFLAGS register (bit 9 is the interrupt-enable flag, IF). */
static inline uint64_t read_rflags(void) {
    uint64_t value;
    __asm__ volatile("pushfq; popq %0" : "=r"(value));
    return value;
}

/* Control registers. CR2 holds the faulting address of the last page fault;
 * CR3 holds the physical address of the top-level page table (PML4). */
static inline uint64_t read_cr2(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

static inline uint64_t read_cr3(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static inline void write_cr3(uint64_t value) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

/* Invalidate the TLB entry for one virtual address after changing its
 * mapping. Without this the CPU may keep using a stale translation. */
static inline void invlpg(uint64_t addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) descriptor_ptr_t;

static inline uint16_t read_cs(void) {
    uint16_t value;
    __asm__ volatile("mov %%cs, %0" : "=r"(value));
    return value;
}

static inline uint16_t read_ss(void) {
    uint16_t value;
    __asm__ volatile("mov %%ss, %0" : "=r"(value));
    return value;
}

static inline void sidt(descriptor_ptr_t *out) {
    __asm__ volatile("sidt %0" : "=m"(*out));
}

/* Model-specific registers (used to inspect the local APIC). */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)));
}

#endif /* HELIX_IO_H */
