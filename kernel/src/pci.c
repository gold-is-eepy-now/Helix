/* pci.c — minimal PCI configuration-space scan for USB host controllers. */
#include "pci.h"

#include "io.h"
#include "console.h"
#include "sync.h"
#include "vmm.h"
#include "ehci.h"
#include "xhci.h"

#define PCI_CONFIG_ADDRESS 0x0CF8u
#define PCI_CONFIG_DATA    0x0CFcu
#define PCI_BUS_LIMIT      1u /* qemu-xhci and current Helix devices are on bus 0 */
#define PCI_DEVICE_LIMIT   32u
#define PCI_FUNCTION_LIMIT 8u
#define XHCI_MMIO_MAP_PAGES 3u
#define EHCI_MMIO_MAP_PAGES 4u
#define AHCI_MMIO_MAP_PAGES 4u
#define USB_CLASS          0x0Cu
#define USB_SUBCLASS       0x03u
#define SATA_CLASS         0x01u
#define SATA_SUBCLASS      0x06u
#define AHCI_PROGIF        0x01u

static pci_usb_controller_t g_usb_controllers[PCI_DEVICE_LIMIT];
static size_t g_usb_controller_count;
static pci_ahci_controller_t g_ahci_controllers[PCI_DEVICE_LIMIT];
static size_t g_ahci_controller_count;
static bool g_initialized;
static uint64_t g_hhdm_offset;

static uint32_t pci_read32(uint8_t bus, uint8_t device, uint8_t function,
                           uint8_t offset) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) | ((uint32_t)function << 8) |
        ((uint32_t)offset & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write32(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, uint32_t value) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) | ((uint32_t)function << 8) |
        ((uint32_t)offset & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static bool xhci_read_capabilities(pci_usb_controller_t *controller) {
    uint32_t bar0 = controller->bar0;
    if ((bar0 & 1u) != 0) return false; /* xHCI requires memory BAR. */
    uint32_t bar_type = (bar0 >> 1) & 3u;
    uint64_t bar_phys = (uint64_t)(bar0 & ~0xFu);
    if (bar_type == 2u) {
        uint32_t bar_high = pci_read32(controller->bus, controller->device,
                                       controller->function, 0x14);
        bar_phys |= (uint64_t)bar_high << 32;
    } else if (bar_type != 0u) {
        return false;
    }
    if (bar_phys == 0 || bar_phys > 0x000FFFFFFFFFF000ULL) return false;

    /* Enable PCI memory decoding while preserving the command bits. The upper
     * status half is written as zero to avoid clearing write-one-to-clear bits.
     */
    uint32_t command_status = pci_read32(controller->bus, controller->device,
                                         controller->function, 0x04);
    uint16_t command = (uint16_t)command_status | (1u << 1);
    pci_write32(controller->bus, controller->device, controller->function,
                0x04, command);

    uint64_t page_phys = bar_phys & ~0xFFFULL;
    uint64_t page_virt = page_phys + g_hhdm_offset;
    if (page_virt < page_phys) return false;
    for (uint64_t page = 0; page < XHCI_MMIO_MAP_PAGES; page++) {
        uint64_t offset = page * 0x1000ULL;
        if (!vmm_map(page_virt + offset, page_phys + offset,
                     VMM_PRESENT | VMM_WRITE | VMM_NX)) return false;
    }

    volatile uint8_t *cap = (volatile uint8_t *)(uintptr_t)
        (page_virt + (bar_phys & 0xFFFULL));
    /* Read the capability header as one aligned dword. Some controllers do
     * not implement byte/word MMIO transactions consistently. */
    uint32_t cap_header = *(volatile uint32_t *)cap;
    uint8_t cap_length = (uint8_t)cap_header;
    if (cap_length < 0x20u) return false;
    uint32_t structural = *(volatile uint32_t *)(cap + 0x04);
    controller->bar0_phys = bar_phys;
    controller->xhci_mmio_virt = page_virt + (bar_phys & 0xFFFULL);
    controller->xhci_capability_length = cap_length;
    controller->xhci_version = (uint16_t)(cap_header >> 16);
    controller->xhci_max_slots = (uint8_t)(structural & 0xFFu);
    controller->xhci_max_ports = (uint8_t)(structural >> 24);
    controller->xhci_capabilities_read = true;
    return true;
}

static bool ehci_read_capabilities(pci_usb_controller_t *controller) {
    uint32_t bar0 = controller->bar0;
    if ((bar0 & 1u) != 0 || ((bar0 >> 1) & 3u) == 3u) return false;
    uint64_t phys = (uint64_t)(bar0 & ~0xFu);
    if (((bar0 >> 1) & 3u) == 2u) {
        uint32_t high = pci_read32(controller->bus, controller->device,
                                   controller->function, 0x14);
        phys |= (uint64_t)high << 32;
    }
    if (phys == 0 || phys > 0x000FFFFFFFFFF000ULL) return false;
    uint32_t command_status = pci_read32(controller->bus, controller->device,
                                         controller->function, 0x04);
    pci_write32(controller->bus, controller->device, controller->function,
                0x04, (uint16_t)command_status | (1u << 1) | (1u << 2));
    uint64_t page_phys = phys & ~0xFFFULL;
    uint64_t page_virt = page_phys + g_hhdm_offset;
    if (page_virt < page_phys) return false;
    for (uint64_t i = 0; i < EHCI_MMIO_MAP_PAGES; i++)
        if (!vmm_map(page_virt + i * 0x1000ULL,
                     page_phys + i * 0x1000ULL,
                     VMM_PRESENT | VMM_WRITE | VMM_NX)) return false;
    volatile uint8_t *cap = (volatile uint8_t *)(uintptr_t)
        (page_virt + (phys & 0xFFFULL));
    uint8_t length = cap[0];
    if (length < 0x10u) return false;
    controller->bar0_phys = phys;
    controller->ehci_mmio_virt = page_virt + (phys & 0xFFFULL);
    controller->ehci_capability_length = length;
    uint32_t structural = *(volatile uint32_t *)(cap + 4);
    controller->ehci_max_ports = (uint8_t)(structural & 0x0Fu);
    controller->ehci_port_power_control = (structural & (1u << 4)) != 0;
    controller->ehci_capabilities_read = controller->ehci_max_ports != 0;
    return controller->ehci_capabilities_read;
}

static bool ahci_map_bar(pci_ahci_controller_t *controller) {
    uint32_t bar = controller->bar5;
    if (bar == 0 || bar == 0xffffffffu || (bar & 1u) != 0 ||
        ((bar >> 1) & 3u) != 0) return false;
    uint64_t phys = (uint64_t)(bar & ~0xFu);
    if (phys == 0 || phys > 0x000FFFFFFFFFF000ULL) return false;

    uint32_t command_status = pci_read32(controller->bus, controller->device,
                                         controller->function, 0x04);
    uint16_t command = (uint16_t)command_status | (1u << 1) | (1u << 2);
    pci_write32(controller->bus, controller->device, controller->function,
                0x04, command);

    uint64_t virt = phys + g_hhdm_offset;
    if (virt < phys) return false;
    for (uint64_t i = 0; i < AHCI_MMIO_MAP_PAGES; i++)
        if (!vmm_map(virt + i * 0x1000u, phys + i * 0x1000u,
                     VMM_PRESENT | VMM_WRITE | VMM_NX)) return false;
    controller->abar_phys = phys;
    controller->abar_virt = virt;
    return true;
}

void pci_init(uint64_t hhdm_offset) {
    IRQ_GUARD;
    if (g_initialized) return;
    g_hhdm_offset = hhdm_offset;
    g_usb_controller_count = 0;
    g_ahci_controller_count = 0;
    for (uint8_t bus = 0; bus < PCI_BUS_LIMIT; bus++) {
        for (uint8_t device = 0; device < PCI_DEVICE_LIMIT; device++) {
            uint32_t identity0 = pci_read32(bus, device, 0, 0x00);
            if ((identity0 & 0xFFFFu) == 0xFFFFu) continue;
            uint32_t header0 = pci_read32(bus, device, 0, 0x0C);
            uint8_t functions = ((header0 >> 16) & 0x80u) ?
                PCI_FUNCTION_LIMIT : 1u;
            for (uint8_t function = 0; function < functions; function++) {
                uint32_t identity = function == 0 ? identity0 :
                    pci_read32(bus, device, function, 0x00);
                if ((identity & 0xFFFFu) == 0xFFFFu) continue;
                uint32_t class_info = pci_read32(bus, device, function, 0x08);
                uint8_t class_code = (uint8_t)(class_info >> 24);
                uint8_t subclass = (uint8_t)(class_info >> 16);
                uint8_t prog_if = (uint8_t)(class_info >> 8);
                if (class_code == SATA_CLASS && subclass == SATA_SUBCLASS &&
                    prog_if == AHCI_PROGIF &&
                    g_ahci_controller_count < PCI_DEVICE_LIMIT) {
                    uint32_t bar5 = pci_read32(bus, device, function, 0x24);
                    pci_ahci_controller_t *ahci =
                        &g_ahci_controllers[g_ahci_controller_count++];
                    *ahci = (pci_ahci_controller_t){
                        .bus = bus, .device = device, .function = function,
                        .vendor_id = (uint16_t)identity,
                        .device_id = (uint16_t)(identity >> 16),
                        .bar5 = bar5,
                    };
                    (void)ahci_map_bar(ahci);
                    continue;
                }
                if (class_code != USB_CLASS || subclass != USB_SUBCLASS ||
                    g_usb_controller_count == PCI_DEVICE_LIMIT) continue;
                uint32_t bar0 = pci_read32(bus, device, function, 0x10);
                g_usb_controllers[g_usb_controller_count++] =
                    (pci_usb_controller_t){
                        .bus = bus,
                        .device = device,
                        .function = function,
                        .programming_interface = (uint8_t)(class_info >> 8),
                        .vendor_id = (uint16_t)identity,
                        .device_id = (uint16_t)(identity >> 16),
                        .bar0 = bar0,
                    };
                pci_usb_controller_t *found =
                    &g_usb_controllers[g_usb_controller_count - 1];
                if (found->programming_interface == 0x30)
                    (void)xhci_read_capabilities(found);
                else if (found->programming_interface == 0x20)
                    (void)ehci_read_capabilities(found);
            }
        }
    }
    g_initialized = true;
}

size_t pci_ahci_controller_count(void) {
    IRQ_GUARD;
    return g_ahci_controller_count;
}

bool pci_ahci_controller_get(size_t index,
                             pci_ahci_controller_t *controller) {
    IRQ_GUARD;
    if (!g_initialized || controller == NULL ||
        index >= g_ahci_controller_count) return false;
    *controller = g_ahci_controllers[index];
    return true;
}

void pci_print_ahci_controllers(void) {
    size_t count = pci_ahci_controller_count();
    console_write("AHCI SATA controllers on PCI bus 0: ");
    console_write_dec(count);
    console_putc('\n');
    for (size_t i = 0; i < count; i++) {
        pci_ahci_controller_t controller;
        if (!pci_ahci_controller_get(i, &controller)) continue;
        console_write("  AHCI at ");
        console_write_dec(controller.bus);
        console_putc(':');
        console_write_dec(controller.device);
        console_putc('.');
        console_write_dec(controller.function);
        console_write(" vendor=");
        console_write_hex(controller.vendor_id);
        console_write(" device=");
        console_write_hex(controller.device_id);
        console_write(" ABAR=");
        console_write_hex(controller.abar_phys);
        console_putc('\n');
    }
}

size_t pci_usb_controller_count(void) {
    IRQ_GUARD;
    return g_usb_controller_count;
}

bool pci_usb_controller_get(size_t index, pci_usb_controller_t *controller) {
    IRQ_GUARD;
    if (!g_initialized || controller == NULL ||
        index >= g_usb_controller_count) return false;
    *controller = g_usb_controllers[index];
    return true;
}

void pci_print_usb_controllers(void) {
    size_t count = pci_usb_controller_count();
    console_write("USB host controllers on PCI bus 0: ");
    console_write_dec(count);
    console_putc('\n');
    for (size_t i = 0; i < count; i++) {
        pci_usb_controller_t controller;
        if (!pci_usb_controller_get(i, &controller)) continue;
        console_write("  ");
        console_write(controller.programming_interface == 0x30 ? "xHCI" :
                      controller.programming_interface == 0x20 ? "EHCI" :
                      controller.programming_interface == 0x10 ? "OHCI" :
                      controller.programming_interface == 0x00 ? "UHCI" :
                      "USB host");
        console_write(" at ");
        console_write_dec(controller.bus);
        console_putc(':');
        console_write_dec(controller.device);
        console_putc('.');
        console_write_dec(controller.function);
        console_write(" vendor=");
        console_write_hex(controller.vendor_id);
        console_write(" device=");
        console_write_hex(controller.device_id);
        console_putc('\n');
        if (controller.programming_interface == 0x30) {
            if (controller.xhci_capabilities_read) {
                console_write("    xHCI version=");
                console_write_hex(controller.xhci_version);
                console_write(" slots=");
                console_write_dec(controller.xhci_max_slots);
                console_write(" ports=");
                console_write_dec(controller.xhci_max_ports);
                console_write(" BAR=");
                console_write_hex(controller.bar0_phys);
                console_putc('\n');
                volatile uint8_t *operational = (volatile uint8_t *)(uintptr_t)
                    (controller.xhci_mmio_virt +
                     controller.xhci_capability_length);
                console_write("    root ports:");
                for (uint16_t port = 0; port < controller.xhci_max_ports;
                     port++) {
                    volatile uint32_t *portsc = (volatile uint32_t *)(
                        operational + 0x400u + (uint64_t)port * 0x10u);
                    uint32_t status = *portsc;
                    console_write(" ");
                    console_write_dec((uint64_t)port + 1u);
                    console_putc('=');
                    if ((status & 1u) == 0) {
                        console_write("empty");
                    } else {
                        console_write("connected(speed=");
                        console_write_dec((status >> 10) & 0xFu);
                        console_putc(')');
                    }
                }
                console_putc('\n');
            } else {
                console_write("    xHCI capability registers unavailable\n");
            }
        } else if (controller.programming_interface == 0x20) {
            if (controller.ehci_capabilities_read) {
                console_write("    EHCI version capability length=");
                console_write_dec(controller.ehci_capability_length);
                console_write(" ports=");
                console_write_dec(controller.ehci_max_ports);
                console_write(" per-port-power=");
                console_write(controller.ehci_port_power_control ? "yes" : "no");
                console_write(" BAR=");
                console_write_hex(controller.bar0_phys);
                console_putc('\n');
            } else console_write("    EHCI capability registers unavailable\n");
        }
    }
    pci_print_ahci_controllers();
    xhci_print_status();
    ehci_print_status();
}
