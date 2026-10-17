/* apic.h — local APIC helpers (only what we need for now). */
#ifndef HELIX_APIC_H
#define HELIX_APIC_H

#include <stdbool.h>
#include <stdint.h>

/* If the local APIC is enabled, the 8259 PIC's output is routed through the
 * APIC's LINT0 pin. This makes sure that path is open: it unmasks LINT0 in
 * ExtINT mode and drops the task priority to 0. Returns true if the LAPIC was
 * enabled and configured, false if it is disabled (PIC goes straight to the
 * CPU, which needs no help). */
bool apic_allow_pic(uint64_t hhdm_offset);

/* True if the local APIC is currently enabled. */
bool apic_enabled(void);

#endif /* HELIX_APIC_H */
