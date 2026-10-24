/* xhci.c — first polled xHCI controller bring-up and Enable Slot command.
 *
 * This deliberately stops before addressing a USB device or creating an
 * endpoint. It establishes the controller DMA rings and proves command-ring
 * completion; it never sends storage commands or writes to media.
 */
#include "xhci.h"

#include "console.h"
#include "io.h"
#include "kstring.h"
#include "pci.h"
#include "pmm.h"

#define PCI_CONFIG_ADDRESS 0x0CF8u
#define PCI_CONFIG_DATA    0x0CFcu
#define XHCI_TRB_COUNT     256u
#define XHCI_TRB_SIZE      16u
#define XHCI_TRB_TYPE(t)   ((uint32_t)(t) << 10)
#define XHCI_TRB_CYCLE     (1u << 0)
#define XHCI_TRB_TC        (1u << 1)
#define XHCI_TRB_LINK      6u
#define XHCI_TRB_ENABLE_SLOT 9u
#define XHCI_TRB_COMMAND_COMPLETION 33u
#define XHCI_COMPLETION_SUCCESS 1u
#define XHCI_USBCMD_RUN     (1u << 0)
#define XHCI_USBCMD_RESET   (1u << 1)
#define XHCI_USBSTS_HALTED  (1u << 0)
#define XHCI_USBSTS_CNR     (1u << 11)
#define XHCI_PORT_CCS       (1u << 0)
#define XHCI_PORT_PED       (1u << 1)
#define XHCI_PORT_PR        (1u << 4)
#define XHCI_PORT_PP        (1u << 9)
#define XHCI_PORT_SPEED(s)  (((s) >> 10) & 0xFu)
#define XHCI_PORT_WPR       (1u << 31)
#define XHCI_POLL_LIMIT     12000000u
#define XHCI_MMIO_BAR_MASK  (~0x3ULL)

typedef struct {
    uint32_t parameter_low;
    uint32_t parameter_high;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

typedef struct {
    uint64_t address;
    uint32_t size;
    uint32_t reserved;
} xhci_erst_entry_t;

typedef struct {
    bool ready;
    uint8_t slot_id;
    uint8_t connected_port;
    uint8_t device_speed;
    uint32_t last_completion;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t configuration_value;
    uint8_t interface_number;
    uint8_t bulk_in_address;
    uint8_t bulk_out_address;
    uint8_t bulk_in_dci;
    uint8_t bulk_out_dci;
    uint16_t bulk_in_packet_size;
    uint16_t bulk_out_packet_size;
    uint32_t block_size;
    uint8_t *bulk_data;      /* contiguous, 64 KiB-aligned multi-sector buffer */
    uint64_t bulk_data_phys;
    uint64_t block_count;
    uint32_t next_tag;
    uint64_t mmio;
    uint64_t doorbell;
    uint64_t runtime;
    uint64_t dcbaa_phys;
    uint64_t command_ring_phys;
    uint64_t event_ring_phys;
    uint64_t erst_phys;
    uint64_t input_context_phys;
    uint64_t output_context_phys;
    uint64_t ep0_ring_phys;
    uint64_t descriptor_phys;
    uint64_t bulk_in_ring_phys;
    uint64_t bulk_out_ring_phys;
    uint64_t *dcbaa;
    uint8_t *input_context;
    uint8_t *output_context;
    xhci_trb_t *command_ring;
    xhci_trb_t *event_ring;
    xhci_trb_t *ep0_ring;
    xhci_trb_t *bulk_in_ring;
    xhci_trb_t *bulk_out_ring;
    uint8_t *descriptor;
    uint16_t event_index;
    uint16_t ep0_index;
    uint16_t bulk_in_index;
    uint16_t bulk_out_index;
    uint8_t event_cycle;
    uint8_t ep0_cycle;
    uint8_t bulk_in_cycle;
    uint8_t bulk_out_cycle;
    uint16_t context_size;
    uint16_t endpoint0_packet_size;
    uint32_t command_index;
    uint8_t command_cycle;
    uint32_t max_slots;
    bool slot_enabled;
    bool device_addressed;
    bool descriptor_read;
    bool mass_storage_ready;
} xhci_state_t;

static xhci_state_t g_xhci;
static uint64_t g_hhdm;

static volatile uint32_t *reg32(uint64_t base, uint32_t offset) {
    return (volatile uint32_t *)(uintptr_t)(base + offset);
}

static volatile uint64_t *reg64(uint64_t base, uint32_t offset) {
    return (volatile uint64_t *)(uintptr_t)(base + offset);
}

static uint64_t pci_config_address(const pci_usb_controller_t *controller,
                                   uint8_t offset) {
    return 0x80000000u | ((uint32_t)controller->bus << 16) |
           ((uint32_t)controller->device << 11) |
           ((uint32_t)controller->function << 8) | (offset & 0xFCu);
}

static void enable_pci_bus_master(const pci_usb_controller_t *controller) {
    outl(PCI_CONFIG_ADDRESS,
         (uint32_t)pci_config_address(controller, 0x04));
    uint32_t command_status = inl(PCI_CONFIG_DATA);
    uint16_t command = (uint16_t)command_status | (1u << 1) | (1u << 2);
    outl(PCI_CONFIG_ADDRESS,
         (uint32_t)pci_config_address(controller, 0x04));
    /* Do not echo the write-one-to-clear status half. */
    outl(PCI_CONFIG_DATA, command);
}

static void *dma_page_alloc(uint64_t *physical_out) {
    uintptr_t physical = pmm_alloc_frame();
    if (physical == 0) return NULL;
    void *virtual = (void *)(uintptr_t)(physical + g_hhdm);
    memset(virtual, 0, PAGE_SIZE);
    *physical_out = physical;
    return virtual;
}

static bool wait_mask32(uint64_t base, uint32_t offset, uint32_t mask,
                        uint32_t expected) {
    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; i++) {
        if ((*reg32(base, offset) & mask) == expected) return true;
        __asm__ volatile("pause");
    }
    return false;
}

static bool event_next(xhci_trb_t *event) {
    volatile xhci_trb_t *source = &g_xhci.event_ring[g_xhci.event_index];
    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; i++) {
        uint32_t control = source->control;
        if ((control & XHCI_TRB_CYCLE) == g_xhci.event_cycle) {
            __sync_synchronize();
            event->parameter_low = source->parameter_low;
            event->parameter_high = source->parameter_high;
            event->status = source->status;
            event->control = control;
            g_xhci.event_index++;
            if (g_xhci.event_index == XHCI_TRB_COUNT) {
                g_xhci.event_index = 0;
                g_xhci.event_cycle ^= 1u;
            }
            uint64_t dequeue = g_xhci.event_ring_phys +
                (uint64_t)g_xhci.event_index * XHCI_TRB_SIZE;
            *reg64(g_xhci.runtime, 0x38) = dequeue | (1u << 3);
            __sync_synchronize();
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

static bool post_command(uint32_t parameter_low, uint32_t parameter_high,
                         uint32_t status, uint32_t control,
                         uint8_t *slot_out) {
    xhci_trb_t *command = &g_xhci.command_ring[g_xhci.command_index];
    command->parameter_low = parameter_low;
    command->parameter_high = parameter_high;
    command->status = status;
    command->control = control | g_xhci.command_cycle;
    g_xhci.command_index++;
    if (g_xhci.command_index == XHCI_TRB_COUNT - 1u) {
        g_xhci.command_index = 0;
        g_xhci.command_cycle ^= 1u;
    }
    __sync_synchronize();
    *reg32(g_xhci.doorbell, 0) = 0;
    __sync_synchronize();

    for (uint32_t i = 0; i < XHCI_TRB_COUNT; i++) {
        xhci_trb_t event;
        if (!event_next(&event)) return false;
        uint32_t type = (event.control >> 10) & 0x3Fu;
        if (type != XHCI_TRB_COMMAND_COMPLETION) continue;
        uint32_t completion = event.status >> 24;
        g_xhci.last_completion = completion;
        if (completion != XHCI_COMPLETION_SUCCESS) return false;
        if (slot_out != NULL) *slot_out = (uint8_t)(event.control >> 24);
        return true;
    }
    return false;
}

static bool command_enable_slot(uint8_t *slot_out) {
    if (!post_command(0, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_ENABLE_SLOT), slot_out))
        return false;
    g_xhci.slot_enabled = *slot_out != 0;
    return g_xhci.slot_enabled;
}

static bool reset_connected_port(const pci_usb_controller_t *controller,
                                 uint8_t *port_out) {
    uint64_t operational = g_xhci.mmio + controller->xhci_capability_length;
    for (uint16_t index = 0; index < controller->xhci_max_ports; index++) {
        uint32_t offset = 0x400u + (uint32_t)index * 0x10u;
        volatile uint32_t *portsc = reg32(operational, offset);
        uint32_t status = *portsc;
        if ((status & XHCI_PORT_CCS) == 0) continue;
        uint32_t speed = XHCI_PORT_SPEED(status);
        g_xhci.device_speed = (uint8_t)speed;
        /* Port reset is a protocol-level reset, not a media write. QEMU was
         * launched with guest-reset=false so it will not reset the host USB
         * device itself. */
        uint32_t request = (status & XHCI_PORT_PP) | XHCI_PORT_PP;
        request |= speed == 4u ? XHCI_PORT_WPR : XHCI_PORT_PR;
        *portsc = request;
        __sync_synchronize();
        uint32_t reset_mask = speed == 4u ? XHCI_PORT_WPR : XHCI_PORT_PR;
        for (uint32_t poll = 0; poll < XHCI_POLL_LIMIT; poll++) {
            status = *portsc;
            if ((status & reset_mask) == 0) {
                if ((status & XHCI_PORT_PED) != 0) {
                    *port_out = (uint8_t)(index + 1u);
                    return true;
                }
                /* Give the controller a short chance to set PED after reset. */
                for (uint32_t settle = 0; settle < 10000u; settle++)
                    __asm__ volatile("pause");
                status = *portsc;
                if ((status & XHCI_PORT_PED) != 0) {
                    *port_out = (uint8_t)(index + 1u);
                    return true;
                }
                return false;
            }
            __asm__ volatile("pause");
        }
        return false;
    }
    return true; /* No attached device; controller can still be ready. */
}

static void set_context_word(uint8_t *context, uint16_t context_size,
                             uint16_t index, uint16_t dword, uint32_t value) {
    uint32_t *words = (uint32_t *)(void *)(context +
        (uint64_t)context_size * index);
    words[dword] = value;
}

static bool wait_transfer_completion(void) {
    for (uint32_t i = 0; i < XHCI_TRB_COUNT; i++) {
        xhci_trb_t event;
        if (!event_next(&event)) return false;
        uint32_t type = (event.control >> 10) & 0x3Fu;
        if (type != 32u) continue; /* Transfer Event */
        uint32_t completion = event.status >> 24;
        g_xhci.last_completion = completion;
        return completion == XHCI_COMPLETION_SUCCESS &&
               (uint8_t)(event.control >> 24) == g_xhci.slot_id;
    }
    return false;
}

static void link_transfer_ring(xhci_trb_t *ring, uint64_t ring_phys,
                               uint16_t index, uint8_t cycle) {
    ring[index].parameter_low = (uint32_t)ring_phys;
    ring[index].parameter_high = (uint32_t)(ring_phys >> 32);
    ring[index].status = 0;
    ring[index].control = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC | cycle;
}

static bool ring_reserve(xhci_trb_t *ring, uint64_t ring_phys,
                         uint16_t *index, uint8_t *cycle, uint16_t count) {
    if (count == 0 || count >= XHCI_TRB_COUNT) return false;
    if ((uint32_t)*index + count > XHCI_TRB_COUNT - 1u) {
        link_transfer_ring(ring, ring_phys, *index, *cycle);
        *index = 0;
        *cycle ^= 1u;
        __sync_synchronize();
    }
    return true;
}

static bool endpoint0_control(uint64_t setup_packet, uint16_t setup_trt,
                              uint8_t *data, uint64_t data_phys,
                              uint16_t data_length, bool data_in) {
    uint16_t needed = data_length != 0 ? 3u : 2u;
    if (!ring_reserve(g_xhci.ep0_ring, g_xhci.ep0_ring_phys,
                      &g_xhci.ep0_index, &g_xhci.ep0_cycle, needed))
        return false;
    uint16_t start = g_xhci.ep0_index;
    xhci_trb_t *setup = &g_xhci.ep0_ring[start];
    setup->parameter_low = (uint32_t)setup_packet;
    setup->parameter_high = (uint32_t)(setup_packet >> 32);
    setup->status = 8u;
    setup->control = XHCI_TRB_TYPE(2u) | g_xhci.ep0_cycle | (1u << 6) |
                     ((uint32_t)setup_trt << 16) | (data_length ? (1u << 4) : 0);
    if (data_length != 0) {
        xhci_trb_t *data_trb = &g_xhci.ep0_ring[start + 1u];
        data_trb->parameter_low = (uint32_t)data_phys;
        data_trb->parameter_high = (uint32_t)(data_phys >> 32);
        data_trb->status = data_length;
        data_trb->control = XHCI_TRB_TYPE(3u) | g_xhci.ep0_cycle |
                            (1u << 4) | (data_in ? (1u << 16) : 0);
    }
    xhci_trb_t *status = &g_xhci.ep0_ring[start + (data_length ? 2u : 1u)];
    status->control = XHCI_TRB_TYPE(4u) | g_xhci.ep0_cycle | (1u << 5) |
                      (data_length == 0 || !data_in ? (1u << 16) : 0);
    g_xhci.ep0_index += needed;
    __sync_synchronize();
    *reg32(g_xhci.doorbell, (uint32_t)g_xhci.slot_id * 4u) = 1u;
    __sync_synchronize();
    (void)data;
    return wait_transfer_completion();
}

static uint32_t read_le32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint32_t read_be32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static void write_le32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void write_be32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static bool bulk_transfer(uint8_t dci, bool input, uint8_t *buffer,
                          uint64_t buffer_phys, uint32_t length) {
    xhci_trb_t *ring = input ? g_xhci.bulk_in_ring : g_xhci.bulk_out_ring;
    uint64_t ring_phys = input ? g_xhci.bulk_in_ring_phys :
                                 g_xhci.bulk_out_ring_phys;
    uint16_t *index = input ? &g_xhci.bulk_in_index : &g_xhci.bulk_out_index;
    uint8_t *cycle = input ? &g_xhci.bulk_in_cycle : &g_xhci.bulk_out_cycle;
    if (ring == NULL || buffer == NULL || length == 0 ||
        !ring_reserve(ring, ring_phys, index, cycle, 1u)) return false;
    xhci_trb_t *trb = &ring[*index];
    trb->parameter_low = (uint32_t)buffer_phys;
    trb->parameter_high = (uint32_t)(buffer_phys >> 32);
    trb->status = length;
    trb->control = XHCI_TRB_TYPE(1u) | *cycle | (1u << 5);
    (*index)++;
    __sync_synchronize();
    *reg32(g_xhci.doorbell, (uint32_t)g_xhci.slot_id * 4u) = dci;
    __sync_synchronize();
    return wait_transfer_completion();
}

static void init_transfer_ring(xhci_trb_t *ring, uint64_t ring_phys) {
    link_transfer_ring(ring, ring_phys, XHCI_TRB_COUNT - 1u, 1u);
}

static bool parse_mass_storage_config(void) {
    /* Fetch the nine-byte configuration header, then the complete descriptor
     * chain into a DMA-visible page. */
    uint64_t setup = 0x80u | (6u << 8) | (0x0200u << 16) | (9uLL << 48);
    if (!endpoint0_control(setup, 3u, g_xhci.descriptor,
                           g_xhci.descriptor_phys, 9u, true)) return false;
    uint16_t total = (uint16_t)g_xhci.descriptor[2] |
                     ((uint16_t)g_xhci.descriptor[3] << 8);
    if (g_xhci.descriptor[1] != 2u || total < 9u || total > PAGE_SIZE)
        return false;
    uint8_t configurations = g_xhci.descriptor[17];
    if (configurations == 0) configurations = 1;
    uint64_t descriptor_setup = 0x80u | (6u << 8) | (0x0200u << 16) |
                                ((uint64_t)total << 48);
    if (!endpoint0_control(descriptor_setup, 3u, g_xhci.descriptor,
                           g_xhci.descriptor_phys, total, true)) return false;
    if (g_xhci.descriptor[1] != 2u) return false;
    g_xhci.configuration_value = g_xhci.descriptor[5];

    bool match_interface = false;
    bool have_in = false;
    bool have_out = false;
    for (uint16_t offset = 0; offset + 2u <= total;) {
        uint8_t length = g_xhci.descriptor[offset];
        uint8_t type = g_xhci.descriptor[offset + 1u];
        if (length < 2u || offset + length > total) return false;
        if (type == 4u && length >= 9u) {
            match_interface = g_xhci.descriptor[offset + 5u] == 8u &&
                g_xhci.descriptor[offset + 6u] == 6u &&
                g_xhci.descriptor[offset + 7u] == 0x50u;
            if (match_interface)
                g_xhci.interface_number = g_xhci.descriptor[offset + 2u];
        } else if (type == 5u && length >= 7u && match_interface &&
                   (g_xhci.descriptor[offset + 3u] & 3u) == 2u) {
            uint8_t address = g_xhci.descriptor[offset + 2u];
            uint16_t packet = (uint16_t)g_xhci.descriptor[offset + 4u] |
                              ((uint16_t)g_xhci.descriptor[offset + 5u] << 8);
            packet &= 0x07FFu;
            if ((address & 0x80u) != 0 && !have_in) {
                g_xhci.bulk_in_address = address;
                g_xhci.bulk_in_packet_size = packet;
                have_in = packet != 0;
            } else if ((address & 0x80u) == 0 && !have_out) {
                g_xhci.bulk_out_address = address;
                g_xhci.bulk_out_packet_size = packet;
                have_out = packet != 0;
            }
        }
        offset += length;
    }
    if (!have_in || !have_out) return false;

    uint64_t set_config = (9u << 8) |
                          ((uint64_t)g_xhci.configuration_value << 16);
    if (!endpoint0_control(set_config, 0, NULL, 0, 0, false)) return false;

    g_xhci.bulk_in_dci = (uint8_t)(((g_xhci.bulk_in_address & 0x0Fu) << 1) | 1u);
    g_xhci.bulk_out_dci = (uint8_t)((g_xhci.bulk_out_address & 0x0Fu) << 1);
    if (g_xhci.bulk_in_dci > 31u || g_xhci.bulk_out_dci > 31u) return false;

    g_xhci.bulk_in_ring = dma_page_alloc(&g_xhci.bulk_in_ring_phys);
    g_xhci.bulk_out_ring = dma_page_alloc(&g_xhci.bulk_out_ring_phys);
    if (g_xhci.bulk_in_ring == NULL || g_xhci.bulk_out_ring == NULL)
        return false;
    g_xhci.bulk_in_cycle = g_xhci.bulk_out_cycle = 1u;
    init_transfer_ring(g_xhci.bulk_in_ring, g_xhci.bulk_in_ring_phys);
    init_transfer_ring(g_xhci.bulk_out_ring, g_xhci.bulk_out_ring_phys);

    memset(g_xhci.input_context, 0, PAGE_SIZE);
    uint32_t context_entries = g_xhci.bulk_in_dci > g_xhci.bulk_out_dci
        ? g_xhci.bulk_in_dci : g_xhci.bulk_out_dci;
    set_context_word(g_xhci.input_context, g_xhci.context_size, 0, 1,
        1u | (1u << g_xhci.bulk_in_dci) | (1u << g_xhci.bulk_out_dci));
    set_context_word(g_xhci.input_context, g_xhci.context_size, 1, 0,
        ((uint32_t)g_xhci.device_speed << 20) | (context_entries << 27));
    set_context_word(g_xhci.input_context, g_xhci.context_size, 1, 1,
        (uint32_t)g_xhci.connected_port << 16);

    uint8_t dci_list[2] = {g_xhci.bulk_out_dci, g_xhci.bulk_in_dci};
    for (size_t i = 0; i < 2; i++) {
        uint8_t dci = dci_list[i];
        bool input = dci == g_xhci.bulk_in_dci;
        uint64_t ring_phys = input ? g_xhci.bulk_in_ring_phys :
                                     g_xhci.bulk_out_ring_phys;
        uint16_t packet = input ? g_xhci.bulk_in_packet_size :
                                  g_xhci.bulk_out_packet_size;
        uint32_t ep_type = input ? 6u : 2u;
        uint16_t context_index = (uint16_t)(dci + 1u);
        set_context_word(g_xhci.input_context, g_xhci.context_size,
            context_index, 1, (3u << 1) | (ep_type << 3) |
            ((uint32_t)packet << 16));
        set_context_word(g_xhci.input_context, g_xhci.context_size,
            context_index, 2, (uint32_t)ring_phys | 1u);
        set_context_word(g_xhci.input_context, g_xhci.context_size,
            context_index, 3, (uint32_t)(ring_phys >> 32));
        set_context_word(g_xhci.input_context, g_xhci.context_size,
            context_index, 4, 1024u);
    }
    uint32_t configure = XHCI_TRB_TYPE(12u) |
                         ((uint32_t)g_xhci.slot_id << 24);
    return post_command((uint32_t)g_xhci.input_context_phys,
                        (uint32_t)(g_xhci.input_context_phys >> 32), 0,
                        configure, NULL);
}

/* One command moves at most 32 KiB. The buffer is aligned to 64 KiB so the
 * single TRB that describes it can never cross a 64 KiB boundary, which the
 * xHCI specification forbids. */
#define XHCI_BULK_MAX_BYTES (32u * 1024u)
#define XHCI_BULK_ALIGN (64u * 1024u)

static bool ensure_bulk_data(void) {
    if (g_xhci.bulk_data != NULL) return true;
    size_t frames = (XHCI_BULK_MAX_BYTES + XHCI_BULK_ALIGN) / PAGE_SIZE;
    uintptr_t base = pmm_alloc_frames(frames);
    if (base == 0) return false;
    uintptr_t aligned = (base + XHCI_BULK_ALIGN - 1u) &
                        ~(uintptr_t)(XHCI_BULK_ALIGN - 1u);
    memset((void *)(base + g_hhdm), 0, frames * PAGE_SIZE);
    g_xhci.bulk_data = (uint8_t *)(aligned + g_hhdm);
    g_xhci.bulk_data_phys = aligned;
    return true;
}

static bool bot_command(const uint8_t *cdb, uint8_t cdb_length,
                        bool data_in, uint32_t transfer_length,
                        uint8_t *data, uint32_t data_length) {
    if (g_xhci.descriptor == NULL || data_length > XHCI_BULK_MAX_BYTES)
        return false;
    bool large = data_length > PAGE_SIZE - 256u;
    if (large && !ensure_bulk_data()) return false;
    if (cdb == NULL || cdb_length == 0 || cdb_length > 16u) return false;
    uint8_t *cbw = g_xhci.descriptor;
    uint8_t *data_buffer = large ? g_xhci.bulk_data : g_xhci.descriptor + 64u;
    uint64_t data_phys = large ? g_xhci.bulk_data_phys
                               : g_xhci.descriptor_phys + 64u;
    uint8_t *csw = g_xhci.descriptor + 192u;
    uint32_t tag = ++g_xhci.next_tag;
    memset(cbw, 0, 31u);
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;
    write_le32(cbw + 4u, tag);
    write_le32(cbw + 8u, transfer_length);
    cbw[12] = data_in ? 0x80u : 0;
    cbw[13] = 0;
    cbw[14] = cdb_length;
    memcpy(cbw + 15u, cdb, cdb_length);
    if (!bulk_transfer(g_xhci.bulk_out_dci, false, cbw,
                       g_xhci.descriptor_phys, 31u)) return false;
    if (data_length != 0) {
        memset(data_buffer, 0, data_length);
        bool transferred = data_in
            ? bulk_transfer(g_xhci.bulk_in_dci, true, data_buffer,
                            data_phys, data_length)
            : (data != NULL &&
               (memcpy(data_buffer, data, data_length),
                bulk_transfer(g_xhci.bulk_out_dci, false, data_buffer,
                              data_phys, data_length)));
        if (!transferred)
            return false;
        if (data_in && data != NULL) memcpy(data, data_buffer, data_length);
    }
    memset(csw, 0, 13u);
    if (!bulk_transfer(g_xhci.bulk_in_dci, true, csw,
                       g_xhci.descriptor_phys + 192u, 13u)) return false;
    return csw[0] == 0x55 && csw[1] == 0x53 && csw[2] == 0x42 &&
           csw[3] == 0x53 && read_le32(csw + 4u) == tag && csw[12] == 0;
}

static bool mass_storage_read_capacity(void) {
    uint8_t capacity[8] = {0};
    uint8_t cdb[10] = {0};
    cdb[0] = 0x25u; /* READ CAPACITY(10) */
    if (!bot_command(cdb, sizeof(cdb), true, sizeof(capacity), capacity,
                     sizeof(capacity)))
        return false;
    uint32_t last_lba = read_be32(capacity);
    uint32_t block_size = read_be32(capacity + 4u);
    if (block_size == 0 || block_size > PAGE_SIZE) return false;
    g_xhci.block_size = block_size;
    g_xhci.block_count = (uint64_t)last_lba + 1u;
    g_xhci.mass_storage_ready = true;
    return true;
}

/* length: a whole number of blocks, at most XHCI_BULK_MAX_BYTES. */
static bool transfer_args_ok(uint64_t lba, const void *buffer, size_t length,
                             uint32_t *blocks) {
    if (!g_xhci.mass_storage_ready || buffer == NULL || length == 0 ||
        g_xhci.block_size == 0 || length % g_xhci.block_size != 0 ||
        length > XHCI_BULK_MAX_BYTES) return false;
    *blocks = (uint32_t)(length / g_xhci.block_size);
    return lba < g_xhci.block_count &&
           *blocks <= g_xhci.block_count - lba && lba + *blocks - 1u <= UINT32_MAX;
}

bool xhci_read_sector(uint64_t lba, void *buffer, size_t length) {
    uint32_t blocks;
    if (!transfer_args_ok(lba, buffer, length, &blocks)) return false;
    uint8_t cdb[10] = {0};
    cdb[0] = 0x28u; /* READ(10) */
    write_be32(cdb + 2u, (uint32_t)lba);
    cdb[7] = (uint8_t)(blocks >> 8);
    cdb[8] = (uint8_t)blocks;
    return bot_command(cdb, sizeof(cdb), true, (uint32_t)length,
                       buffer, (uint32_t)length);
}
bool xhci_write_sector(uint64_t lba, const void *buffer, size_t length) {
    uint32_t blocks;
    if (!transfer_args_ok(lba, buffer, length, &blocks)) return false;
    uint8_t cdb[10] = {0};
    cdb[0] = 0x2au; /* WRITE(10) */
    write_be32(cdb + 2u, (uint32_t)lba);
    cdb[7] = (uint8_t)(blocks >> 8);
    cdb[8] = (uint8_t)blocks;
    return bot_command(cdb, sizeof(cdb), false, (uint32_t)length,
                       (uint8_t *)(uintptr_t)buffer, (uint32_t)length);
}
/* SYNCHRONIZE CACHE(10): ask the device to commit its write cache. */
bool xhci_flush(void) {
    if (!g_xhci.mass_storage_ready) return false;
    uint8_t cdb[10] = {0};
    cdb[0] = 0x35u;
    return bot_command(cdb, sizeof(cdb), false, 0, NULL, 0);
}
uint32_t xhci_sector_size(void) { return g_xhci.block_size; }
uint64_t xhci_sector_count(void) { return g_xhci.block_count; }
bool xhci_storage_ready(void) { return g_xhci.mass_storage_ready; }

static bool address_and_read_descriptor(const pci_usb_controller_t *controller) {
    uint8_t *dcbaa_page = (uint8_t *)g_xhci.dcbaa;
    g_xhci.output_context = dma_page_alloc(&g_xhci.output_context_phys);
    g_xhci.input_context = dma_page_alloc(&g_xhci.input_context_phys);
    g_xhci.ep0_ring = dma_page_alloc(&g_xhci.ep0_ring_phys);
    g_xhci.descriptor = dma_page_alloc(&g_xhci.descriptor_phys);
    if (g_xhci.output_context == NULL || g_xhci.input_context == NULL ||
        g_xhci.ep0_ring == NULL || g_xhci.descriptor == NULL) return false;

    uint32_t hccparams1 = *reg32(g_xhci.mmio, 0x10);
    g_xhci.context_size = (hccparams1 & (1u << 2)) != 0 ? 64u : 32u;
    uint64_t *dcbaa = (uint64_t *)(void *)dcbaa_page;
    dcbaa[g_xhci.slot_id] = g_xhci.output_context_phys;
    g_xhci.endpoint0_packet_size = g_xhci.device_speed >= 4u ? 512u :
                                   g_xhci.device_speed == 3u ? 64u : 8u;

    /* Input Control Context adds Slot Context and default control EP0. */
    set_context_word(g_xhci.input_context, g_xhci.context_size, 0, 1, 0x3u);
    set_context_word(g_xhci.input_context, g_xhci.context_size, 1, 0,
                     ((uint32_t)g_xhci.device_speed << 20) | (1u << 27));
    set_context_word(g_xhci.input_context, g_xhci.context_size, 1, 1,
                     (uint32_t)g_xhci.connected_port << 16);
    set_context_word(g_xhci.input_context, g_xhci.context_size, 2, 1,
                     (3u << 1) | (4u << 3) |
                     ((uint32_t)g_xhci.endpoint0_packet_size << 16));
    set_context_word(g_xhci.input_context, g_xhci.context_size, 2, 2,
                     (uint32_t)g_xhci.ep0_ring_phys | 1u);
    set_context_word(g_xhci.input_context, g_xhci.context_size, 2, 3,
                     (uint32_t)(g_xhci.ep0_ring_phys >> 32));
    set_context_word(g_xhci.input_context, g_xhci.context_size, 2, 4, 8u);

    uint32_t address_control = XHCI_TRB_TYPE(11u) |
        ((uint32_t)g_xhci.slot_id << 24);
    if (!post_command((uint32_t)g_xhci.input_context_phys,
                      (uint32_t)(g_xhci.input_context_phys >> 32), 0,
                      address_control, NULL)) return false;
    g_xhci.device_addressed = true;

    /* Standard GET_DESCRIPTOR(Device), 18 bytes, through EP0. */
    memset(g_xhci.descriptor, 0, PAGE_SIZE);
    uint64_t setup = 0x80u | (6u << 8) | (0x0100u << 16) |
                     (18uLL << 48);
    if (!endpoint0_control(setup, 3u, g_xhci.descriptor,
                           g_xhci.descriptor_phys, 18u, true)) return false;
    if (g_xhci.descriptor[0] < 18u || g_xhci.descriptor[1] != 1u)
        return false;
    g_xhci.vendor_id = (uint16_t)g_xhci.descriptor[8] |
                       ((uint16_t)g_xhci.descriptor[9] << 8);
    g_xhci.product_id = (uint16_t)g_xhci.descriptor[10] |
                        ((uint16_t)g_xhci.descriptor[11] << 8);
    g_xhci.device_class = g_xhci.descriptor[4];
    g_xhci.device_subclass = g_xhci.descriptor[5];
    g_xhci.device_protocol = g_xhci.descriptor[6];
    g_xhci.descriptor_read = true;
    (void)controller;
    return true;
}

static bool start_controller(const pci_usb_controller_t *controller) {
    if (!controller->xhci_capabilities_read || g_hhdm == 0) return false;
    memset(&g_xhci, 0, sizeof(g_xhci));
    g_xhci.mmio = controller->xhci_mmio_virt;

    uint32_t dboff = *reg32(g_xhci.mmio, 0x14) & XHCI_MMIO_BAR_MASK;
    uint32_t rtsoff = *reg32(g_xhci.mmio, 0x18) & ~0x1Fu;
    g_xhci.doorbell = g_xhci.mmio + dboff;
    g_xhci.runtime = g_xhci.mmio + rtsoff;

    enable_pci_bus_master(controller);

    uint32_t cap_length = controller->xhci_capability_length;
    uint32_t usbcmd = *reg32(g_xhci.mmio, cap_length);
    if ((usbcmd & XHCI_USBCMD_RUN) != 0) {
        *reg32(g_xhci.mmio, cap_length) = usbcmd & ~XHCI_USBCMD_RUN;
        if (!wait_mask32(g_xhci.mmio, cap_length + 4,
                         XHCI_USBSTS_HALTED, XHCI_USBSTS_HALTED)) return false;
    }
    if (!wait_mask32(g_xhci.mmio, cap_length + 4, XHCI_USBSTS_CNR, 0))
        return false;
    *reg32(g_xhci.mmio, cap_length) = XHCI_USBCMD_RESET;
    if (!wait_mask32(g_xhci.mmio, cap_length, XHCI_USBCMD_RESET, 0) ||
        !wait_mask32(g_xhci.mmio, cap_length + 4, XHCI_USBSTS_CNR, 0))
        return false;

    g_xhci.dcbaa = dma_page_alloc(&g_xhci.dcbaa_phys);
    g_xhci.command_ring = dma_page_alloc(&g_xhci.command_ring_phys);
    g_xhci.event_ring = dma_page_alloc(&g_xhci.event_ring_phys);
    xhci_erst_entry_t *erst = dma_page_alloc(&g_xhci.erst_phys);
    if (g_xhci.dcbaa == NULL || g_xhci.command_ring == NULL ||
        g_xhci.event_ring == NULL || erst == NULL) return false;

    g_xhci.max_slots = controller->xhci_max_slots;
    if (g_xhci.max_slots == 0 || g_xhci.max_slots > 255u) return false;
    g_xhci.event_cycle = 1;
    g_xhci.command_cycle = 1;
    g_xhci.ep0_cycle = 1;

    /* Last TRB links the command ring back to its beginning. */
    xhci_trb_t *link = &g_xhci.command_ring[XHCI_TRB_COUNT - 1u];
    link->parameter_low = (uint32_t)g_xhci.command_ring_phys;
    link->parameter_high = (uint32_t)(g_xhci.command_ring_phys >> 32);
    link->status = 0;
    link->control = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC |
                    XHCI_TRB_CYCLE;

    erst[0].address = g_xhci.event_ring_phys;
    erst[0].size = XHCI_TRB_COUNT;
    *reg64(g_xhci.mmio, cap_length + 0x30) = g_xhci.dcbaa_phys;
    *reg64(g_xhci.mmio, cap_length + 0x18) =
        g_xhci.command_ring_phys | XHCI_TRB_CYCLE;

    /* Polled event handling: leave interrupt delivery disabled. */
    *reg32(g_xhci.runtime, 0x20) = 0;
    *reg32(g_xhci.runtime, 0x28) = 1;
    *reg64(g_xhci.runtime, 0x30) = g_xhci.erst_phys;
    *reg64(g_xhci.runtime, 0x38) = g_xhci.event_ring_phys;
    *reg32(g_xhci.mmio, cap_length + 0x38) = (uint32_t)g_xhci.max_slots;

    *reg32(g_xhci.mmio, cap_length) = XHCI_USBCMD_RUN;
    if (!wait_mask32(g_xhci.mmio, cap_length + 4,
                     XHCI_USBSTS_HALTED, 0) ||
        !wait_mask32(g_xhci.mmio, cap_length + 4, XHCI_USBSTS_CNR, 0))
        return false;

    g_xhci.ready = true;
    (void)reset_connected_port(controller, &g_xhci.connected_port);
    if (g_xhci.connected_port != 0 &&
        command_enable_slot(&g_xhci.slot_id))
        if (address_and_read_descriptor(controller) &&
            parse_mass_storage_config())
            (void)mass_storage_read_capacity();
    return true;
}

bool xhci_init(uint64_t hhdm_offset) {
    if (g_xhci.ready) return true;
    if (hhdm_offset == 0) return false;
    g_hhdm = hhdm_offset;
    for (size_t i = 0; i < pci_usb_controller_count(); i++) {
        pci_usb_controller_t controller;
        if (!pci_usb_controller_get(i, &controller) ||
            controller.programming_interface != 0x30 ||
            !controller.xhci_capabilities_read) continue;
        return start_controller(&controller);
    }
    return false;
}

bool xhci_ready(void) { return g_xhci.ready; }

void xhci_print_status(void) {
    if (!g_xhci.ready) {
        console_write("xHCI controller: not started\n");
        return;
    }
    console_write("xHCI controller: running, polled DMA rings active\n");
    if (g_xhci.connected_port != 0) {
        console_write("  reset port: ");
        console_write_dec(g_xhci.connected_port);
        console_write(g_xhci.slot_enabled ? "; slot enabled" :
                      "; slot not enabled");
        console_write(g_xhci.device_addressed ? "; addressed" :
                      "; not addressed");
        console_write(g_xhci.descriptor_read ? "; device descriptor read\n" :
                      "; descriptor unavailable\n");
    }
    if (g_xhci.descriptor_read) {
        console_write("  USB device: VID=");
        console_write_hex(g_xhci.vendor_id);
        console_write(" PID=");
        console_write_hex(g_xhci.product_id);
        console_write(" class=");
        console_write_hex(g_xhci.device_class);
        console_write(" subclass=");
        console_write_hex(g_xhci.device_subclass);
        console_write(" protocol=");
        console_write_hex(g_xhci.device_protocol);
        console_putc('\n');
        if (g_xhci.mass_storage_ready) {
            console_write("  USB mass storage: BOT transport ready; ");
            console_write_dec(g_xhci.block_count);
            console_write(" blocks x ");
            console_write_dec(g_xhci.block_size);
            console_write(" bytes\n");
        }
    } else {
        console_write("USB device enumeration: no descriptor available\n");
    }
}
