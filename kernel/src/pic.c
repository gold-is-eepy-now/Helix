/* pic.c — 8259 PIC initialisation.
 *
 * By default the master PIC delivers IRQ0-7 as vectors 8-15, which overlap the
 * CPU exceptions. The classic fix is to "remap" the PICs to vectors 32-47 by
 * sending them a short initialisation sequence (ICW1-ICW4).
 */
#include "pic.h"

#include "io.h"

#define PIC1_COMMAND 0x20u
#define PIC1_DATA    0x21u
#define PIC2_COMMAND 0xA0u
#define PIC2_DATA    0xA1u
#define PIC_EOI      0x20u

void pic_remap(void) {
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_COMMAND, 0x11); io_wait(); /* ICW1: start init, expect ICW4 */
    outb(PIC2_COMMAND, 0x11); io_wait();
    outb(PIC1_DATA, 0x20);    io_wait(); /* ICW2: master vector offset 32  */
    outb(PIC2_DATA, 0x28);    io_wait(); /* ICW2: slave vector offset 40   */
    outb(PIC1_DATA, 0x04);    io_wait(); /* ICW3: slave on master IRQ2     */
    outb(PIC2_DATA, 0x02);    io_wait(); /* ICW3: slave cascade identity   */
    outb(PIC1_DATA, 0x01);    io_wait(); /* ICW4: 8086 mode                */
    outb(PIC2_DATA, 0x01);    io_wait();

    outb(PIC1_DATA, mask1);   /* restore the saved masks */
    outb(PIC2_DATA, mask2);
}

void pic_set_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(irq & 7u);
    outb(port, (uint8_t)(inb(port) | (1u << bit)));
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(irq & 7u);
    outb(port, (uint8_t)(inb(port) & ~(1u << bit)));
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}
