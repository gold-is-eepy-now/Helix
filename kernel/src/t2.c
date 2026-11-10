/* t2.c — Apple T2 (iBridge) BCE mailbox driver.
 *
 * Clean-room implementation from freely available protocol descriptions.
 *
 * Boot order matters: the driver probe runs during driver_init_all() and
 * only touches PCI config space, while the mailbox lives in MMIO space and
 * can only be mapped once the kernel page tables exist. That is why the
 * work is split into t2_register() / t2_start().
 */

#include "t2.h"

#include "capability.h"
#include "console.h"
#include "driver.h"
#include "object.h"
#include "pci.h"
#include "pmm.h"
#include "time.h"
#include "t2_protocol.h"
#include "vmm.h"

/* Distinct from the USB host controller window (0xFFFFA00000000000). */
#define T2_MMIO_VIRT 0xFFFFB00000000000ULL
/* Page-level cache disable: mailbox registers are device memory. */
#define T2_PTE_PCD (1ULL << 4)
/* Bound the poll loop even if the TSC is not calibrated. */
#define T2_MBOX_MAX_SPINS 2000000u

static t2_state_t g_t2;
static driver_t g_t2_driver;

/* Read a PCI base address register. Returns 0 for I/O BARs or unassigned
 * memory BARs. 64-bit BARs carry their high half in the next dword. */
static uint64_t read_bar(const pci_function_t *fn, unsigned index) {
    uint8_t offset = (uint8_t)(0x10u + index * 4u);
    uint32_t low = pci_config_read32(fn->bus, fn->device, fn->function, offset);
    if ((low & 0x1u) != 0) return 0; /* I/O space BAR */
    uint64_t base = (uint64_t)(low & ~0xFu);
    if ((low & 0x6u) == 0x4u) { /* 64-bit memory BAR */
        uint32_t high = pci_config_read32(fn->bus, fn->device, fn->function,
                                          (uint8_t)(offset + 4u));
        base |= (uint64_t)high << 32;
    }
    return base;
}

/* Enable memory space and bus mastering for one function. */
static void enable_bus_master(const pci_function_t *fn) {
    uint32_t command = pci_config_read32(fn->bus, fn->device, fn->function, 0x04);
    command |= 0x6u; /* memory space + bus master */
    pci_config_write32(fn->bus, fn->device, fn->function, 0x04, command);
}

static void t2_probe(void) {
    pci_function_t fn;
    if (!pci_find_device(T2_PCI_VENDOR_APPLE, T2_PCI_DEVICE_BCE, 0, &fn)) {
        return; /* no T2 on this machine */
    }

    g_t2.present = true;
    g_t2.bus = fn.bus;
    g_t2.device = fn.device;
    g_t2.function = fn.function;
    g_t2.vendor_id = fn.vendor_id;
    g_t2.device_id = fn.device_id;
    g_t2.dma_phys = read_bar(&fn, 2);     /* BAR2: DMA ring registers */
    g_t2.mailbox_phys = read_bar(&fn, 4); /* BAR4: mailbox registers */

    enable_bus_master(&fn);
    /* The BCE DMA rings are programmed through function 0 of the same slot,
     * which has to be a bus master as well or the rings stay dead. */
    if (fn.function != 0) {
        pci_function_t slot0 = fn;
        slot0.function = 0;
        enable_bus_master(&slot0);
    }

    console_write("[t2] BCE mailbox function found, BAR4=");
    console_write_hex(g_t2.mailbox_phys);
    console_write(" BAR2=");
    console_write_hex(g_t2.dma_phys);
    console_write("\n");
}

void t2_register(void) {
    g_t2_driver.name = "t2";
    g_t2_driver.init = t2_probe;
    g_t2_driver.probe = NULL;
    g_t2_driver.remove = NULL;
    driver_register(&g_t2_driver);
}

void t2_start(void) {
    if (!g_t2.present) return;

    if (g_t2.mailbox_phys == 0) {
        console_write("[t2] BCE mailbox BAR4 is not assigned\n");
        return;
    }

    uint64_t page_offset = g_t2.mailbox_phys & ((uint64_t)PAGE_SIZE - 1u);
    uint64_t page_base = g_t2.mailbox_phys & ~((uint64_t)PAGE_SIZE - 1u);
    if (!vmm_map(T2_MMIO_VIRT, page_base,
                 VMM_PRESENT | VMM_WRITE | VMM_NX | T2_PTE_PCD)) {
        console_write("[t2] could not map BCE mailbox registers\n");
        return;
    }

    volatile uint32_t *regs =
        (volatile uint32_t *)(T2_MMIO_VIRT + page_offset);
    t2_mailbox_t mailbox;
    t2_mailbox_init(&mailbox, regs);

    t2_mailbox_send(&mailbox,
                    T2_MB_MSG(T2_MB_SET_FW_PROTOCOL_VERSION,
                              T2_FW_PROTOCOL_VERSION));

    uint64_t deadline = time_uptime_ms() + T2_MBOX_TIMEOUT_MS;
    uint64_t spins = 0;
    uint64_t reply = 0;
    bool answered = false;
    while (!answered) {
        answered = t2_mailbox_take_reply(&mailbox, &reply);
        if (answered) break;
        if (time_uptime_ms() >= deadline) break;
        if (++spins > T2_MBOX_MAX_SPINS) break;
        __asm__ volatile("pause" ::: "memory");
    }

    if (!answered) {
        console_write("[t2] BCE firmware handshake timed out\n");
        return;
    }
    if (!t2_fw_handshake_valid(reply)) {
        console_write("[t2] BCE firmware rejected the protocol handshake\n");
        return;
    }

    g_t2.firmware_ready = true;
    g_t2.firmware_version = (uint32_t)T2_MB_VALUE(reply);
    console_write("[t2] BCE firmware handshake ok, protocol ");
    console_write_hex(g_t2.firmware_version);
    console_write("\n");

    /* Publish the device through the object manager and hand out a
     * revocable Device.Control capability for SEP services. */
    object_t *device = device_create(&g_t2_driver, 0, NULL);
    if (device == NULL) {
        console_write("[t2] could not create the T2 device object\n");
        return;
    }
    g_t2.device_object = device;

    handle_table_t *table = object_kernel_handles();
    handle_t source = handle_create(table, device,
                                    OBJECT_RIGHT_READ | OBJECT_RIGHT_DUPLICATE |
                                        OBJECT_RIGHT_INSPECT | OBJECT_RIGHT_CONTROL);
    if (source != 0) {
        g_t2.control_capability = capability_create(table, source,
                                                     OBJECT_RIGHT_CONTROL);
        handle_close(table, source);
    }
    if (g_t2.control_capability == 0) {
        console_write("[t2] could not create the T2 control capability\n");
    }
}

const t2_state_t *t2_state(void) {
    return &g_t2;
}

void t2_print_status(void) {
    if (!g_t2.present) {
        console_write("T2 security SoC: not present\n");
        return;
    }

    console_write("T2 security SoC: present\n");
    console_write("  mailbox function: ");
    console_write_hex(g_t2.vendor_id);
    console_write(":");
    console_write_hex(g_t2.device_id);
    console_write(" at ");
    console_write_dec(g_t2.bus);
    console_write(":");
    console_write_dec(g_t2.device);
    console_write(".");
    console_write_dec(g_t2.function);
    console_write("\n");
    console_write("  BAR4 mailbox registers: ");
    console_write_hex(g_t2.mailbox_phys);
    console_write("\n  BAR2 DMA ring registers: ");
    console_write_hex(g_t2.dma_phys);
    console_write("\n  firmware: ");
    if (g_t2.firmware_ready) {
        console_write("ready, protocol ");
        console_write_hex(g_t2.firmware_version);
        console_write("\n");
    } else {
        console_write("no handshake\n");
    }
    console_write("  device object: ");
    console_write_hex((uint64_t)(uintptr_t)g_t2.device_object);
    console_write("\n  Device.Control capability: ");
    console_write_hex(g_t2.control_capability);
    console_write("\n");
}
