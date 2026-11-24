/* nx.c — x86_64: enable the no-execute bit (EFER.NXE, bit 11 of the MSR at 0xC0000080).
 *
 * Why an MSR write: page-table entries carry an NX bit, but the CPU ignores it until
 * EFER.NXE is set. The CPUID check that says NX exists is in cpu.c (generic). */
#include <stdint.h>

#define MSR_EFER      0xC0000080u
#define EFER_NXE      (1u << 11)

void arch_enable_nxe(void) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(MSR_EFER));
    low |= EFER_NXE;
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(MSR_EFER) : "memory");
}
