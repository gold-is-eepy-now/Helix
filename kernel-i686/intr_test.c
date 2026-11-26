/* intr_test.c — interrupt bring-up and its two self-tests. */
#include "intr_test.h"

#include "idt.h"
#include "pic.h"
#include "timer.h"

void interrupts_init(uint32_t hz) {
    idt_init();
    pic_init();
    pit_init(hz);
}

bool breakpoint_selftest(void) {
    uint32_t before = idt_breakpoint_count();
    __asm__ volatile("int3");
    return idt_breakpoint_count() == before + 1;
}

bool timer_selftest(void) {
    uint32_t start = timer_ticks();
    /* hlt sleeps until the next interrupt; the loop bound stops a missing timer from
     * hanging the boot. At 100 Hz, 10 ticks take 100 ms. */
    __asm__ volatile("sti");
    for (uint32_t i = 0; i < 1000 && timer_ticks() - start < 10; i++) {
        __asm__ volatile("hlt");
    }
    __asm__ volatile("cli");
    return timer_ticks() - start >= 10;
}
