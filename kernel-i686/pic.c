/* pic.c — remapping and acknowledging the 8259 PIC pair. The command sequence (ICW1-4)
 * is the standard one from the 8259 datasheet. */
#include "pic.h"

#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

void pic_init(void) {
    outb(PIC1_CMD, 0x11);  io_wait();    /* ICW1: start init, expect ICW4 */
    outb(PIC2_CMD, 0x11);  io_wait();
    outb(PIC1_DATA, 0x20); io_wait();    /* ICW2: master vectors start at 32 */
    outb(PIC2_DATA, 0x28); io_wait();    /* ICW2: slave vectors start at 40 */
    outb(PIC1_DATA, 0x04); io_wait();    /* ICW3: slave attached to master IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();    /* ICW3: slave identity 2 */
    outb(PIC1_DATA, 0x01); io_wait();    /* ICW4: 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, 0xFE); io_wait();    /* masks: bit 0 clear = IRQ0 (timer) enabled */
    outb(PIC2_DATA, 0xFF);               /* all slave IRQs masked */
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}
