/* apic.c — open the LAPIC path so 8259 PIC interrupts reach the CPU.
 *
 * On a machine where the firmware enabled the local APIC, the 8259 PIC's INT
 * line is wired to the LAPIC's LINT0 pin, which is delivered in "ExtINT" mode.
 * If LINT0 is masked, or the LAPIC's task-priority register is raised above
 * the interrupt's priority, the PIC's interrupts are silently dropped — which
 * is exactly the "PIT counts but IRQ0 never arrives" symptom.
 *
 * We do the minimum: enable the APIC, set LINT0 to ExtINT and unmasked, and
 * clear the TPR. We are not yet using APIC timers or the IOAPIC; the 8259 is
 * still the interrupt controller.
 */
#include "apic.h"

#include "io.h"
#include "vmm.h"

#define MSR_APIC_BASE 0x1Bu

#define LAPIC_TPR   0x080u
#define LAPIC_SVR   0x0F0u
#define LAPIC_LINT0 0x350u

#define MSR_APIC_BASE_ENABLE (1ULL << 11)

bool apic_enabled(void) {
    return (rdmsr(MSR_APIC_BASE) & MSR_APIC_BASE_ENABLE) != 0;
}

bool apic_allow_pic(uint64_t hhdm_offset) {
    uint64_t base_msr = rdmsr(MSR_APIC_BASE);
    if ((base_msr & MSR_APIC_BASE_ENABLE) == 0) {
        return false;
    }

    uint64_t apic_phys = base_msr & 0x000FFFFFFFFFF000ULL;
    uint64_t apic_virt = apic_phys + hhdm_offset;

    /* The higher-half direct map covers RAM, but not device MMIO like the
     * LAPIC, so map that one page ourselves before touching it. */
    vmm_map(apic_virt, apic_phys, VMM_PRESENT | VMM_WRITE | VMM_NX);

    volatile uint32_t *regs = (volatile uint32_t *)apic_virt;

    regs[LAPIC_SVR / 4] = 0x1FFu;        /* enable APIC, spurious vector 0xFF */
    regs[LAPIC_TPR / 4] = 0;             /* allow every interrupt priority    */
    regs[LAPIC_LINT0 / 4] = 0x00000700u; /* LINT0: ExtINT delivery, unmasked  */

    return true;
}
