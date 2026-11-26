/* pic.h — the 8259 interrupt controllers (master and slave, the PIC pair). */
#ifndef HELIX_I686_PIC_H
#define HELIX_I686_PIC_H

#include <stdint.h>

/* Move IRQ0..15 to vectors 32..47. Their power-on vectors (8..15) collide with CPU
 * exceptions such as the double fault (8). Leaves only IRQ0 (the timer) unmasked. */
void pic_init(void);

/* End-of-interrupt for IRQ `irq` (0..15): a slave IRQ also needs the master notified. */
void pic_eoi(uint8_t irq);

#endif /* HELIX_I686_PIC_H */
