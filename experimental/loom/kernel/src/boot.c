/* Kernel entry. Limine loads us in 64-bit long mode with paging on, the kernel mapped at the
 * top of the address space, and a higher-half direct map (HHDM) of all physical memory. This
 * file asks Limine for what it needs and prints what it got. */
#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "loom_kernel.h"

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0,
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

void kpanic(const char *msg) {
    serial_puts("LOOM PANIC: ");
    serial_puts(msg);
    serial_puts("\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

static uint64_t usable_bytes(const struct limine_memmap_response *m) {
    uint64_t total = 0;
    for (uint64_t i = 0; i < m->entry_count; i++) {
        if (m->entries[i]->type == LIMINE_MEMMAP_USABLE) total += m->entries[i]->length;
    }
    return total;
}

void kmain(void) {
    serial_init();
    serial_puts("LOOM kernel 0.1 (milestone: boot)\n");

    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) kpanic("Limine base revision not supported");

    struct limine_hhdm_response *hhdm = hhdm_request.response;
    if (hhdm == NULL) kpanic("no HHDM response");
    serial_puts("hhdm offset: ");
    serial_puthex64(hhdm->offset);
    serial_puts("\n");

    struct limine_memmap_response *mm = memmap_request.response;
    if (mm == NULL) kpanic("no memory map response");
    serial_puts("memory map entries: ");
    serial_putu64(mm->entry_count);
    serial_puts("\nusable memory bytes: ");
    serial_putu64(usable_bytes(mm));
    serial_puts("\n");

    serial_puts("LOOM boot OK\n");

    bool demo_ok = loom_demo_run();
    serial_puts(demo_ok ? "LOOM demo: PASS\n" : "LOOM demo: FAIL\n");
    for (;;) __asm__ volatile ("cli; hlt");
}
