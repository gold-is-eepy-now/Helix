/* pic.h — the 8259 Programmable Interrupt Controller. */
#ifndef HELIX_PIC_H
#define HELIX_PIC_H

#include <stdint.h>

/* Remap the PIC so hardware IRQs arrive as vectors 32-47 instead of the
 * default 8-15, which collide with CPU exceptions. */
void pic_remap(void);

/* Mask (disable) or unmask (enable) one IRQ line. */
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);

/* Acknowledge an IRQ so more can be delivered. */
void pic_send_eoi(uint8_t irq);

#endif /* HELIX_PIC_H */
