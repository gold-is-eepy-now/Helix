/* cpu.h — CPU identification via the CPUID instruction. */
#ifndef HELIX_CPU_H
#define HELIX_CPU_H

#include <stdbool.h>
#include <stddef.h>

/* Fill `out` (at least 13 bytes) with the CPU vendor string, e.g. "GenuineIntel". */
void cpu_vendor(char *out);

/* Fill `out` (at least 49 bytes) with the CPU brand string, e.g.
 * "Intel(R) Core(TM) i7-...". Falls back to a generic string if the CPU does
 * not report one. */
void cpu_brand(char *out);

/* Enable EFER.NXE when the processor supports the NX page-table bit. */
bool cpu_enable_nx(void);

#endif /* HELIX_CPU_H */
