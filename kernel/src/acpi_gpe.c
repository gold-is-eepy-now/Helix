/* acpi_gpe.c — ACPI events through the SCI. See acpi_gpe.h and docs/16-acpi.md.
 *
 * Register layout used here (ACPI 6, section 4.8): a block of N bytes holds N/2 status bytes
 * followed by N/2 enable bytes. Status bits are cleared by writing 1 to them. */
#include "acpi_gpe.h"

#include <stddef.h>   /* NULL */

#include "acpi.h"
#include "io.h"
#include "irq.h"
#include "panic.h"   /* kprintf */
#include "pic.h"

#define PM1_STS_PWRBTN 0x0100u   /* power button status and enable, bit 8 */
#define PM1_CNT_SCI_EN 0x0001u   /* PM1a control, bit 0: ACPI mode is on */
#define EFLAGS_IF      0x0200u

typedef struct {
    acpi_gpe_handler_t fn;
    void *device;
    void *ctx;
    bool level;
} gpe_slot_t;

static gpe_slot_t g_slots[ACPI_GPE_SLOTS];
static acpi_pm_t g_pm;
static bool g_ready;
static uint32_t g_gpe_count;     /* GPEs in block 0: 8 per enable byte, capped at the slots */

static uint16_t gpe_enable_port(void) { return (uint16_t)(g_pm.gpe0 + g_pm.gpe0_len / 2); }
static uint16_t pm1_enable_port(void) { return (uint16_t)(g_pm.pm1a_evt + g_pm.pm1_evt_len / 2); }

/* Enable bytes are read, changed and written back. The SCI handler can run in the middle of a
 * thread's update, so interrupts are off for the read-modify-write. */
static void set_enable_bit(uint32_t gpe, bool on) {
    uint64_t flags = read_rflags();
    cpu_disable_interrupts();
    uint16_t port = (uint16_t)(gpe_enable_port() + gpe / 8);
    uint8_t bit = (uint8_t)(1u << (gpe % 8));
    uint8_t v = inb(port);
    outb(port, on ? (uint8_t)(v | bit) : (uint8_t)(v & (uint8_t)~bit));
    if (flags & EFLAGS_IF) cpu_enable_interrupts();
}

static void dispatch(uint32_t gpe) {
    gpe_slot_t *s = &g_slots[gpe];
    if (s->fn == NULL) {
        /* Nothing is listening. Turn it off so it does not keep interrupting. */
        set_enable_bit(gpe, false);
        return;
    }
    /* ACPI rule: a level GPE stays off until its driver calls acpi_gpe_finish. */
    if (s->level) set_enable_bit(gpe, false);
    s->fn(s->device, gpe, s->ctx);
}

/* The SCI handler. It runs with interrupts off, from the interrupt gate. */
static void sci_handler(void *ctx) {
    (void)ctx;
    uint16_t pm1 = (uint16_t)(inw(g_pm.pm1a_evt) & inw(pm1_enable_port()));
    if (pm1 & PM1_STS_PWRBTN) {
        outw(g_pm.pm1a_evt, PM1_STS_PWRBTN);   /* clear it before anything else */
        kprintf("[acpi] power button pressed\n");
    }
    for (uint32_t b = 0; b < g_gpe_count / 8; b++) {
        uint16_t status_port = (uint16_t)(g_pm.gpe0 + b);
        uint8_t fired = (uint8_t)(inb(status_port) & inb((uint16_t)(gpe_enable_port() + b)));
        if (fired == 0) continue;
        outb(status_port, fired);              /* clear only the bits that fired */
        for (uint32_t bit = 0; bit < 8; bit++) {
            if (fired & (1u << bit)) dispatch(b * 8u + bit);
        }
    }
}

bool acpi_gpe_init(void) {
    if (g_ready) return true;
    acpi_pm_t pm;
    if (!acpi_get_pm(&pm)) {
        kprintf("[acpi] FADT has no PM register blocks; GPEs unavailable\n");
        return false;
    }
    if (pm.sci_irq == 0 || pm.sci_irq >= 16) {
        kprintf("[acpi] SCI on interrupt %u is not a PIC line; GPEs unavailable\n", (unsigned)pm.sci_irq);
        return false;
    }
    if (pm.pm1_evt_len < 4) {
        kprintf("[acpi] PM1 event block is too short; GPEs unavailable\n");
        return false;
    }
    /* Without SCI_EN the firmware is still in legacy mode: events raise no SCI, and writing the
     * registers would be unsafe. Refuse rather than guess. */
    g_pm = pm;
    if (!(inw(pm.pm1a_cnt) & PM1_CNT_SCI_EN)) {
        kprintf("[acpi] SCI_EN is clear (not in ACPI mode); GPEs unavailable\n");
        return false;
    }

    g_gpe_count = pm.gpe0 ? (uint32_t)(pm.gpe0_len / 2u) * 8u : 0u;
    if (g_gpe_count > ACPI_GPE_SLOTS) g_gpe_count = ACPI_GPE_SLOTS;

    /* Start from a known state: clear anything the firmware left pending, disable every GPE,
     * and enable only the power button. */
    outw(pm.pm1a_evt, 0xFFFFu);
    outw(pm1_enable_port(), PM1_STS_PWRBTN);
    for (uint32_t b = 0; b < g_gpe_count / 8; b++) {
        outb((uint16_t)(pm.gpe0 + b), 0xFFu);
        outb((uint16_t)(gpe_enable_port() + b), 0);
    }

    /* The handler is in place before the line is unmasked, so no event can arrive without it. */
    if (!irq_register((uint8_t)pm.sci_irq, sci_handler, NULL)) {
        kprintf("[acpi] SCI interrupt %u already has a handler\n", (unsigned)pm.sci_irq);
        return false;
    }
    pic_set_level_triggered((uint8_t)pm.sci_irq);
    pic_clear_mask(2);                         /* the slave PIC cascades through IRQ 2 */
    pic_clear_mask((uint8_t)pm.sci_irq);
    g_ready = true;
    kprintf("[acpi] SCI on IRQ %u, PM1a 0x%x, GPE0 0x%x (%u GPEs)\n", (unsigned)pm.sci_irq,
            (unsigned)pm.pm1a_evt, (unsigned)pm.gpe0, (unsigned)g_gpe_count);
    return true;
}

bool acpi_gpe_ready(void) { return g_ready; }

uint32_t acpi_gpe_install(uint32_t gpe, bool level, acpi_gpe_handler_t fn, void *device, void *ctx) {
    if (!g_ready) return GPE_ERR_NOT_READY;
    if (fn == NULL || gpe >= g_gpe_count) return GPE_ERR_RANGE;
    if (g_slots[gpe].fn != NULL) return GPE_ERR_EXISTS;
    g_slots[gpe].fn = fn;
    g_slots[gpe].device = device;
    g_slots[gpe].ctx = ctx;
    g_slots[gpe].level = level;
    return GPE_OK;
}

uint32_t acpi_gpe_remove(uint32_t gpe, acpi_gpe_handler_t fn) {
    if (!g_ready) return GPE_ERR_NOT_READY;
    if (gpe >= g_gpe_count) return GPE_ERR_RANGE;
    if (g_slots[gpe].fn == NULL || g_slots[gpe].fn != fn) return GPE_ERR_NO_HANDLER;
    set_enable_bit(gpe, false);
    g_slots[gpe].fn = NULL;
    g_slots[gpe].device = NULL;
    g_slots[gpe].ctx = NULL;
    g_slots[gpe].level = false;
    return GPE_OK;
}

uint32_t acpi_gpe_enable(uint32_t gpe, bool on) {
    if (!g_ready) return GPE_ERR_NOT_READY;
    if (gpe >= g_gpe_count) return GPE_ERR_RANGE;
    set_enable_bit(gpe, on);
    return GPE_OK;
}

uint32_t acpi_gpe_finish(uint32_t gpe) {
    if (!g_ready) return GPE_ERR_NOT_READY;
    if (gpe >= g_gpe_count) return GPE_ERR_RANGE;
    if (g_slots[gpe].fn != NULL && g_slots[gpe].level) set_enable_bit(gpe, true);
    return GPE_OK;
}
