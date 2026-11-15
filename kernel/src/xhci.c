/* xhci.c — read-only discovery of the xHCI host controller.
 *
 * Register offsets and fields, from the Linux xhci headers (xhci-caps.h, xhci.h) and the
 * OSDev xHCI reference:
 *   capability: CAPLENGTH 0x00 (byte), HCIVERSION 0x02 (16 bits), HCSPARAMS1 0x04,
 *     HCSPARAMS2 0x08, HCCPARAMS1 0x10, DBOFF 0x14 (mask ~3), RTSOFF 0x18 (mask ~0x1f).
 *     HCS_MAX_SLOTS bits 0-7; HCS_MAX_INTRS bits 8-18; HCS_MAX_PORTS bits 24-31.
 *   operational (base + CAPLENGTH): USBCMD 0x00, USBSTS 0x04, PAGESIZE 0x08.
 *   ports (operational + 0x400, stride 0x10): PORTSC. PORT_CONNECT bit 0, PORT_POWER bit 9.
 *
 * Read-only: nothing is written. Bits we would need to write (Run/Stop, HCRST) are not
 * used here, because their values are not confirmed in the sources read. */
#include "xhci.h"

#include <stddef.h>

#include "klog.h"
#include "pci.h"
#include "vmm.h"

#define XHCI_CLASS      0x0Cu
#define XHCI_SUBCLASS   0x03u
#define XHCI_PROG_IF    0x30u

#define PCI_COMMAND     0x04u
#define PCI_BAR0        0x10u
#define PCI_BAR1        0x14u

#define XHCI_MMIO_VIRT  0xFFFFA00003000000ULL
#define XHCI_MMIO_PAGES 16u           /* 64 KiB; mapping does not touch the device */
#define PTE_PCD         (1ULL << 4)

#define CAP_CAPLENGTH   0x00u
#define CAP_HCIVERSION  0x02u
#define CAP_HCSPARAMS1  0x04u
#define CAP_HCCPARAMS1  0x10u

#define OP_USBSTS       0x04u
#define OP_PAGESIZE     0x08u
#define OP_PORT_BASE    0x400u
#define OP_PORT_STRIDE  0x10u

#define PORT_CONNECT    (1u << 0)
#define PORT_POWER      (1u << 9)

static volatile uint8_t *g_mmio;
static xhci_info_t g_info;

static inline uint32_t rd32(uint32_t off) { return *(volatile uint32_t *)(g_mmio + off); }
static inline uint16_t rd16(uint32_t off) { return *(volatile uint16_t *)(g_mmio + off); }
static inline uint8_t rd8(uint32_t off)   { return *(volatile uint8_t *)(g_mmio + off); }

bool xhci_init(void) {
    if (g_info.found) return true;

    pci_function_t f;
    if (!pci_find_class(XHCI_CLASS, XHCI_SUBCLASS, XHCI_PROG_IF, 0, &f)) {
        klog_note(KLOG_INPUT, "[xhci] no xHCI controller found\n");
        return false;
    }
    g_info.found = true;
    g_info.bus = f.bus;
    g_info.device = f.device;
    g_info.function = f.function;

    uint32_t bar0 = pci_config_read32(f.bus, f.device, f.function, PCI_BAR0);
    if ((bar0 & 1u) != 0) {
        klog_note(KLOG_INPUT, "[xhci] BAR0 is an I/O BAR; refusing\n");
        return false;
    }
    uint64_t phys = (uint64_t)(bar0 & ~0xFu);
    uint32_t type = (bar0 >> 1) & 3u;
    if (type == 2u) {
        phys |= (uint64_t)pci_config_read32(f.bus, f.device, f.function, PCI_BAR1) << 32;
    } else if (type != 0u) {
        klog_note(KLOG_INPUT, "[xhci] BAR0 has a reserved type; refusing\n");
        return false;
    }
    if (phys == 0) {
        klog_note(KLOG_INPUT, "[xhci] BAR0 is zero; firmware did not assign it\n");
        return false;
    }
    g_info.bar_phys = phys;

    /* Memory decoding only. Read-only stage: no bus mastering. */
    uint32_t cmd = pci_config_read32(f.bus, f.device, f.function, PCI_COMMAND);
    pci_config_write32(f.bus, f.device, f.function, PCI_COMMAND, cmd | 0x2u);

    for (uint32_t p = 0; p < XHCI_MMIO_PAGES; p++) {
        if (!vmm_map(XHCI_MMIO_VIRT + (uint64_t)p * 0x1000ULL,
                     (phys & ~0xFFFULL) + (uint64_t)p * 0x1000ULL,
                     VMM_PRESENT | VMM_WRITE | VMM_NX | PTE_PCD)) {
            klog_note(KLOG_INPUT, "[xhci] could not map the registers\n");
            return false;
        }
    }
    g_mmio = (volatile uint8_t *)(uintptr_t)(XHCI_MMIO_VIRT + (phys & 0xFFFULL));

    g_info.hci_version = rd16(CAP_HCIVERSION);
    g_info.cap_length = rd8(CAP_CAPLENGTH);
    g_info.hcsparams1 = rd32(CAP_HCSPARAMS1);
    g_info.hccparams1 = rd32(CAP_HCCPARAMS1);
    g_info.max_slots = (uint16_t)(g_info.hcsparams1 & 0xFFu);
    g_info.max_intrs = (uint16_t)((g_info.hcsparams1 >> 8) & 0x7FFu);
    g_info.max_ports = (uint8_t)((g_info.hcsparams1 >> 24) & 0xFFu);

    /* Operational registers sit at CAPLENGTH, a single byte, so they are always inside the
     * mapped window. A value below 0x20 would overlap the capability registers, so refuse. */
    if (g_info.cap_length < 0x20u) {
        klog_note(KLOG_INPUT, "[xhci] capability length is out of range; refusing\n");
        return false;
    }
    volatile uint8_t *op = g_mmio + g_info.cap_length;
    g_info.usbsts = *(volatile uint32_t *)(op + OP_USBSTS);
    g_info.pagesize = *(volatile uint32_t *)(op + OP_PAGESIZE);

    uint32_t limit = g_info.max_ports;
    if (limit > 32u) limit = 32u;   /* bounded: the port register block is 0x10 bytes each */
    for (uint32_t p = 0; p < limit; p++) {
        uint32_t off = g_info.cap_length + OP_PORT_BASE + p * OP_PORT_STRIDE;
        if (off + 4u > XHCI_MMIO_PAGES * 0x1000u) break;
        uint32_t portsc = rd32(off);
        if (portsc & PORT_CONNECT) g_info.ports_connected++;
        if (portsc & PORT_POWER) g_info.ports_powered++;
    }
    klog_note(KLOG_INPUT, "[xhci] controller found and read back (read-only)\n");
    return true;
}

void xhci_get_info(xhci_info_t *out) {
    *out = g_info;
}
