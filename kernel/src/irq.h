/* irq.h — handlers for the PIC interrupt lines. IRQ 0 (the timer) is handled directly by
 * idt.c; any other line calls the handler registered here, then the PIC is acknowledged.
 *
 * Why a table: the ACPI SCI shares one line with the firmware, and a driver can own a line
 * the same way. The IDT only knows vectors, so the mapping from line to handler lives here. */
#ifndef HELIX_IRQ_H
#define HELIX_IRQ_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*irq_handler_t)(void *ctx);

/* Returns false if the line is out of range, IRQ 0 (reserved for the timer), or taken. */
bool irq_register(uint8_t irq, irq_handler_t fn, void *ctx);
void irq_unregister(uint8_t irq);

/* Called by idt.c for each hardware interrupt, before the PIC is acknowledged. */
void irq_dispatch(uint8_t irq);

#endif /* HELIX_IRQ_H */
