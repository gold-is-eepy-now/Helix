/* cpu.c — CPUID wrappers. CPUID is how user and kernel code ask the CPU about
 * itself: vendor, brand name, feature bits, cache sizes, and more. Each "leaf"
 * is selected by writing a number to EAX (and sometimes ECX) and executing
 * CPUID; the answer comes back in EAX/EBX/ECX/EDX. */
#include "cpu.h"

#include <stdint.h>
#include <stdbool.h>

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

void cpu_vendor(char *out) {
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    /* The vendor string is EBX, EDX, ECX in that order (12 bytes). */
    const uint32_t regs[3] = {b, d, c};
    size_t pos = 0;
    for (int r = 0; r < 3; r++) {
        for (int byte = 0; byte < 4; byte++) {
            out[pos++] = (char)(regs[r] >> (8 * byte));
        }
    }
    out[pos] = '\0';
}

void cpu_brand(char *out) {
    uint32_t max_ext = 0, b, c, d;
    cpuid(0x80000000u, &max_ext, &b, &c, &d);
    if (max_ext < 0x80000004u) {
        const char *fallback = "x86_64 CPU";
        size_t i = 0;
        for (; fallback[i] != '\0'; i++) {
            out[i] = fallback[i];
        }
        out[i] = '\0';
        return;
    }

    size_t pos = 0;
    for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
        uint32_t a, bb, cc, dd;
        cpuid(leaf, &a, &bb, &cc, &dd);
        const uint32_t regs[4] = {a, bb, cc, dd};
        for (int r = 0; r < 4; r++) {
            for (int byte = 0; byte < 4; byte++) {
                out[pos++] = (char)(regs[r] >> (8 * byte));
            }
        }
    }
    out[48] = '\0';

    /* The brand string is often padded with leading spaces; trim them. */
    size_t lead = 0;
    while (out[lead] == ' ') {
        lead++;
    }
    if (lead > 0) {
        size_t i = 0;
        for (; out[lead + i] != '\0'; i++) {
            out[i] = out[lead + i];
        }
        out[i] = '\0';
    }
}

bool cpu_enable_nx(void) {
    uint32_t max_ext, b, c, d;
    cpuid(0x80000000u, &max_ext, &b, &c, &d);
    if (max_ext < 0x80000001u) return false;
    cpuid(0x80000001u, &max_ext, &b, &c, &d);
    if ((d & (1u << 20)) == 0) return false;

    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080u));
    low |= 1u << 11; /* EFER.NXE */
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(0xC0000080u) : "memory");
    return true;
}
