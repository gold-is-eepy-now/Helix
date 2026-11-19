/* irq.c — the per-line handler table behind irq.h. */
#include "irq.h"

#include <stddef.h>   /* NULL */

#define IRQ_LINES 16u

typedef struct {
    irq_handler_t fn;
    void *ctx;
} irq_slot_t;

static irq_slot_t g_irq[IRQ_LINES];

bool irq_register(uint8_t irq, irq_handler_t fn, void *ctx) {
    if (irq == 0 || irq >= IRQ_LINES || fn == NULL || g_irq[irq].fn != NULL) return false;
    g_irq[irq].fn = fn;
    g_irq[irq].ctx = ctx;
    return true;
}

void irq_unregister(uint8_t irq) {
    if (irq == 0 || irq >= IRQ_LINES) return;
    g_irq[irq].fn = NULL;
    g_irq[irq].ctx = NULL;
}

void irq_dispatch(uint8_t irq) {
    if (irq >= IRQ_LINES || g_irq[irq].fn == NULL) return;
    g_irq[irq].fn(g_irq[irq].ctx);
}
