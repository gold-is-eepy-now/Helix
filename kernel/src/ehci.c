/* ehci.c — single-device, polled EHCI Mass Storage BOT support. */
#include "ehci.h"

#include "console.h"
#include "io.h"
#include "kstring.h"
#include "pci.h"
#include "pmm.h"
#include "time.h"

#define PCI_ADDR 0x0cf8u
#define PCI_DATA 0x0cfcu
#define EHCI_POLL_LIMIT 12000000u
#define EHCI_QTD_ACTIVE (1u << 7)
#define EHCI_QTD_HALTED (1u << 6)
#define EHCI_QTD_ERRORS 0x7cu
#define EHCI_LINK_TERMINATE 1u
#define EHCI_LINK_QH 2u
#define EHCI_MAX_TRANSFER 8192u
#define EHCI_PORT_POWER (1u << 12)
#define EHCI_PORT_RESET (1u << 8)
#define EHCI_PORT_ENABLE (1u << 2)
#define EHCI_PORT_CONNECT (1u << 0)

typedef struct __attribute__((aligned(32))) {
    uint32_t next, alternate, token, buffer[5];
} ehci_qtd_t;

typedef struct __attribute__((aligned(32))) {
    uint32_t horizontal, endpoint, capabilities, current;
    uint32_t overlay[8];
    uint32_t reserved[8];
} ehci_qh_t;

typedef struct {
    bool ready, storage_ready;
    uint64_t hhdm, mmio, operational;
    uint32_t *ports;
    ehci_qh_t *qh;
    ehci_qtd_t *qtd;
    uint8_t *buffer;
    uint32_t qh_phys, qtd_phys, buffer_phys;
    uint32_t port_count, connected_port;
    bool port_power_control;
    uint32_t address, max_packet, configuration, interface;
    uint32_t bulk_in, bulk_out, bulk_in_packet, bulk_out_packet;
    uint32_t in_toggle, out_toggle, tag, block_size;
    uint64_t block_count;
} ehci_state_t;

static ehci_state_t g_ehci;

static ehci_qtd_t *qh_overlay(void) {
    return (ehci_qtd_t *)(void *)((uint8_t *)g_ehci.qh + 16u);
}

static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) |
        ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xfcu);
    outl(PCI_ADDR, address); return inl(PCI_DATA);
}

static void pci_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                      uint32_t value) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) |
        ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xfcu);
    outl(PCI_ADDR, address); outl(PCI_DATA, value);
}

static volatile uint32_t *reg(uint32_t offset) {
    return (volatile uint32_t *)(uintptr_t)(g_ehci.mmio + offset);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static void put_le32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
    p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 24);
}
static void put_be32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}

static bool wait_reg(uint32_t offset, uint32_t mask, uint32_t want) {
    for (uint32_t i = 0; i < EHCI_POLL_LIMIT; i++) {
        if ((*reg(offset) & mask) == want) return true;
        __asm__ volatile("pause");
    }
    return false;
}

static void delay_ms(uint64_t milliseconds) {
    uint64_t deadline = time_uptime_ms() + milliseconds;
    while (time_uptime_ms() < deadline) __asm__ volatile("pause");
}

static void *dma_page(uint32_t *phys_out) {
    uintptr_t phys = pmm_alloc_frame();
    if (phys == 0 || phys > UINT32_MAX) {
        if (phys != 0) pmm_free_frame(phys);
        return NULL;
    }
    memset((void *)(uintptr_t)(phys + g_ehci.hhdm), 0, PAGE_SIZE);
    *phys_out = (uint32_t)phys;
    return (void *)(uintptr_t)(phys + g_ehci.hhdm);
}

static void qtd_prepare(ehci_qtd_t *qtd, uint32_t phys, uint32_t next,
                        uint8_t pid, uint32_t bytes, uint32_t toggle,
                        bool ioc) {
    qtd->next = next;
    qtd->alternate = EHCI_LINK_TERMINATE;
    qtd->token = EHCI_QTD_ACTIVE | (3u << 10) | ((uint32_t)pid << 8) |
                 ((bytes & 0x7fffu) << 16) | (toggle ? (1u << 31) : 0) |
                 (ioc ? (1u << 15) : 0);
    for (uint32_t i = 0; i < 5; i++) qtd->buffer[i] = 0;
    if (bytes != 0) {
        qtd->buffer[0] = phys;
        for (uint32_t i = 1; i < 5; i++)
            qtd->buffer[i] = (phys & ~0xfffu) + i * 0x1000u;
    }
}

static bool control_chain(uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index,
                          uint8_t *data, uint16_t length, bool input) {
    uint8_t *setup = g_ehci.buffer;
    setup[0] = request_type; setup[1] = request;
    setup[2] = (uint8_t)value; setup[3] = (uint8_t)(value >> 8);
    setup[4] = (uint8_t)index; setup[5] = (uint8_t)(index >> 8);
    setup[6] = (uint8_t)length; setup[7] = (uint8_t)(length >> 8);
    if (length != 0 && data != NULL && !input) memcpy(g_ehci.buffer + 64u, data, length);
    uint32_t base = g_ehci.qtd_phys;
    ehci_qtd_t *q = g_ehci.qtd;
    uint32_t status_i = length != 0 ? 2u : 1u;
    qtd_prepare(&q[0], g_ehci.buffer_phys, base + sizeof(*q), 2u, 8u, 0, false);
    if (length != 0) {
        qtd_prepare(&q[1], g_ehci.buffer_phys + 64u,
                    base + 2u * sizeof(*q), input ? 1u : 0u,
                    length, 1u, false);
        if (input) q[1].alternate = base + 2u * sizeof(*q);
    }
    qtd_prepare(&q[status_i], 0, EHCI_LINK_TERMINATE,
                input ? 0u : 1u, 0, 1u, true);
    __sync_synchronize();
    ehci_qtd_t *overlay = qh_overlay();
    overlay->next = base;
    overlay->alternate = EHCI_LINK_TERMINATE;
    overlay->token = 0;
    __sync_synchronize();
    for (uint32_t i = 0; i < EHCI_POLL_LIMIT; i++) {
        if ((q[status_i].token & EHCI_QTD_ACTIVE) == 0) {
            if ((q[status_i].token & EHCI_QTD_ERRORS) != 0) {
                console_write("[ehci] control status error ");
                console_write_hex(q[0].token); console_putc(' ');
                console_write_hex(length ? q[1].token : 0);
                console_putc(' '); console_write_hex(q[status_i].token);
                console_putc('\n'); return false;
            }
            if (input && length != 0 && data != NULL)
                memcpy(data, g_ehci.buffer + 64u, length);
            return (q[0].token & EHCI_QTD_ERRORS) == 0 &&
                (length == 0 || (q[1].token & EHCI_QTD_ERRORS) == 0);
        }
        __asm__ volatile("pause");
    }
    console_write("[ehci] control timeout ");
    console_write_hex(q[0].token); console_putc(' ');
    console_write_hex(length ? q[1].token : 0); console_putc(' ');
    console_write_hex(q[status_i].token); console_putc(' ');
    console_write_hex(qh_overlay()->token); console_putc('\n');
    return false;
}

static bool bulk_transfer(bool input, uint8_t *data, uint32_t length) {
    if (length == 0 || length > EHCI_MAX_TRANSFER) return false;
    uint32_t offset = 64u;
    memcpy(g_ehci.buffer + offset, data, input ? 0 : length);
    uint32_t ep = input ? g_ehci.bulk_in : g_ehci.bulk_out;
    uint32_t packet = input ? g_ehci.bulk_in_packet : g_ehci.bulk_out_packet;
    uint32_t toggle = input ? g_ehci.in_toggle : g_ehci.out_toggle;
    qtd_prepare(g_ehci.qtd, g_ehci.buffer_phys + offset,
                EHCI_LINK_TERMINATE, input ? 1u : 0u,
                length, toggle, true);
    g_ehci.qh->endpoint = (g_ehci.address & 0x7fu) |
        ((ep & 0x0fu) << 8) | (2u << 12) | (1u << 14) | (1u << 15) |
        (packet << 16);
    ehci_qtd_t *overlay = qh_overlay();
    g_ehci.qh->current = 0;
    overlay->next = g_ehci.qtd_phys;
    overlay->alternate = EHCI_LINK_TERMINATE;
    overlay->token = 0;
    __sync_synchronize();
    bool completed = false;
    for (uint32_t i = 0; i < EHCI_POLL_LIMIT; i++) {
        if ((g_ehci.qtd->token & EHCI_QTD_ACTIVE) == 0) {
            completed = (g_ehci.qtd->token & EHCI_QTD_ERRORS) == 0;
            break;
        }
        __asm__ volatile("pause");
    }
    if (!completed) {
        console_write("[ehci] bulk transfer error token=");
        console_write_hex(g_ehci.qtd->token); console_putc('\n');
        return false;
    }
    if (input) memcpy(data, g_ehci.buffer + offset, length);
    uint32_t packets = (length + packet - 1u) / packet;
    if (input) g_ehci.in_toggle ^= packets & 1u;
    else g_ehci.out_toggle ^= packets & 1u;
    return true;
}

static bool bot(const uint8_t *cdb, uint8_t cdb_len, bool input,
                uint32_t data_length, uint8_t *data) {
    uint8_t *cbw = g_ehci.buffer;
    uint8_t *csw = g_ehci.buffer + 1024u;
    memset(cbw, 0, 31u);
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;
    put_le32(cbw + 4, ++g_ehci.tag);
    put_le32(cbw + 8, data_length);
    cbw[12] = input ? 0x80u : 0;
    cbw[14] = cdb_len;
    memcpy(cbw + 15, cdb, cdb_len);
    if (!bulk_transfer(false, cbw, 31u)) { console_write("[ehci] BOT CBW failed\n"); return false; }
    if (data_length != 0 && !bulk_transfer(input, data, data_length)) { console_write("[ehci] BOT data stage failed\n"); return false; }
    if (!bulk_transfer(true, csw, 13u)) { console_write("[ehci] BOT CSW transfer failed\n"); return false; }
    if (!(csw[0] == 0x55 && csw[1] == 0x53 && csw[2] == 0x42 &&
          csw[3] == 0x53 && le32(csw + 4) == g_ehci.tag && csw[12] == 0)) {
        console_write("[ehci] BOT CSW invalid: ");
        for (uint32_t i = 0; i < 13; i++) { console_write_hex(csw[i]); console_putc(' '); }
        console_putc('\n'); return false;
    }
    return true;
}

static bool configure_mass_storage(void) {
    uint8_t *d = g_ehci.buffer + 64u;
    if (!control_chain(0x80, 6, 0x0200, 0, d, 9, true)) { console_write("[ehci] config header failed\n"); return false; }
    uint16_t total = (uint16_t)d[2] | ((uint16_t)d[3] << 8);
    if (d[1] != 2 || total < 9 || total > 2048) return false;
    if (!control_chain(0x80, 6, 0x0200, 0, d, total, true) || d[1] != 2) { console_write("[ehci] config descriptor failed\n"); return false; }
    g_ehci.configuration = d[5];
    bool match = false, have_in = false, have_out = false;
    for (uint16_t off = 0; off + 2 <= total;) {
        uint8_t n = d[off], type = d[off + 1];
        if (n < 2 || off + n > total) return false;
        if (type == 4 && n >= 9) {
            match = d[off + 5] == 8 && d[off + 6] == 6 && d[off + 7] == 0x50;
            if (match) g_ehci.interface = d[off + 2];
        } else if (type == 5 && n >= 7 && match && (d[off + 3] & 3u) == 2) {
            uint8_t addr = d[off + 2];
            uint16_t packet = (uint16_t)d[off + 4] | ((uint16_t)d[off + 5] << 8);
            packet &= 0x7ffu;
            if ((addr & 0x80u) && !have_in) {
                g_ehci.bulk_in = addr; g_ehci.bulk_in_packet = packet; have_in = packet != 0;
            } else if (!(addr & 0x80u) && !have_out) {
                g_ehci.bulk_out = addr; g_ehci.bulk_out_packet = packet; have_out = packet != 0;
            }
        }
        off += n;
    }
    if (!have_in || !have_out) { console_write("[ehci] bulk endpoints missing\n"); return false; }
    if (!control_chain(0, 9, (uint16_t)g_ehci.configuration, 0, NULL, 0, false)) { console_write("[ehci] set configuration failed\n"); return false; }
    uint8_t cdb[10] = {0}, capacity[8] = {0};
    cdb[0] = 0x25;
    if (!bot(cdb, 10, true, sizeof(capacity), capacity)) { console_write("[ehci] read capacity failed\n"); return false; }
    uint32_t last = be32(capacity), block = be32(capacity + 4);
    if (block == 0 || block > PAGE_SIZE) return false;
    g_ehci.block_size = block; g_ehci.block_count = (uint64_t)last + 1u;
    g_ehci.storage_ready = true;
    return true;
}

static bool enumerate_device(void) {
    uint8_t *d = g_ehci.buffer + 64u;
    g_ehci.qh->endpoint = (2u << 12) | (1u << 14) | (1u << 15) | (64u << 16);
    if (!control_chain(0x80, 6, 0x0100, 0, d, 18, true) ||
        d[0] < 18 || d[1] != 1) { console_write("[ehci] device descriptor failed\n"); return false; }
    uint32_t packet = d[7];
    if (packet < 8 || packet > 64) { console_write("[ehci] invalid EP0 max packet\n"); return false; }
    g_ehci.max_packet = packet;
    if (!control_chain(0, 5, 1, 0, NULL, 0, false)) { console_write("[ehci] set address failed\n"); return false; }
    /* USB requires the device to finish its address transition before the
     * next setup packet. Use the calibrated boot-time clock; IRQ0 is not yet
     * unmasked while controller initialization runs. */
    delay_ms(2u);
    g_ehci.address = 1;
    g_ehci.qh->endpoint = 1u | (2u << 12) | (1u << 14) |
                          (1u << 15) | (g_ehci.max_packet << 16);
    return configure_mass_storage();
}

static bool legacy_handoff(const pci_usb_controller_t *c) {
    uint32_t hcc = *(volatile uint32_t *)(uintptr_t)(c->ehci_mmio_virt + 8u);
    uint8_t eecp = (uint8_t)((hcc >> 8) & 0xffu);
    for (uint32_t count = 0; eecp >= 0x40 && count < 48; count++) {
        uint32_t cap = pci_read(c->bus, c->device, c->function, eecp);
        uint8_t id = (uint8_t)cap;
        uint8_t next = (uint8_t)(cap >> 8);
        if (id == 1) {
            uint32_t sem = cap | (1u << 24);
            pci_write(c->bus, c->device, c->function, eecp, sem);
            for (uint32_t i = 0; i < EHCI_POLL_LIMIT; i++) {
                if ((pci_read(c->bus, c->device, c->function, eecp) &
                     (1u << 16)) == 0) break;
                __asm__ volatile("pause");
            }
            return (pci_read(c->bus, c->device, c->function, eecp) &
                    (1u << 16)) == 0;
        }
        if (next == 0 || next <= eecp) break;
        eecp = next;
    }
    return true;
}

static bool scan_connected_ports(void) {
    if (!g_ehci.ready || g_ehci.storage_ready) return g_ehci.storage_ready;
    for (uint32_t i = 0; i < g_ehci.port_count; i++) {
        uint32_t offset = (uint32_t)(g_ehci.operational - g_ehci.mmio) +
                          0x44u + i * 4u;
        uint32_t status = *reg(offset);
        uint32_t power = g_ehci.port_power_control ? EHCI_PORT_POWER : 0;
        if (g_ehci.port_power_control && (status & EHCI_PORT_POWER) == 0) {
            /* ICH8/VirtualBox EHCI ports may be individually power-gated.
             * Apply power before reset; preserving an initially-clear PP bit
             * leaves an attached USB device electrically unavailable. */
            *reg(offset) = EHCI_PORT_POWER;
            delay_ms(20u);
            status = *reg(offset);
        }
        if ((status & EHCI_PORT_CONNECT) == 0) continue;

        /* If firmware or a recent hotplug already enabled this port, leave
         * its device state intact and enumerate it at the current address. */
        if ((status & EHCI_PORT_ENABLE) == 0) {
            /* A passed-through device can still be completing a host-side
             * reset after connection. Wait for the port to settle before
             * asserting reset again or routing it to an OHCI companion. */
            uint64_t settle_deadline = time_uptime_ms() + 1000u;
            do {
                status = *reg(offset);
                if ((status & EHCI_PORT_CONNECT) == 0 ||
                    (status & EHCI_PORT_ENABLE) != 0 ||
                    ((status >> 10) & 3u) != 0) break;
                __asm__ volatile("pause");
            } while (time_uptime_ms() < settle_deadline);
        }

        if ((status & EHCI_PORT_CONNECT) == 0) continue;
        if ((status & EHCI_PORT_ENABLE) == 0) {
            /* EHCI leaves PR set until software clears it after the USB reset
             * interval. */
            *reg(offset) = power | EHCI_PORT_RESET;
            delay_ms(50u);
            *reg(offset) = power;
            uint64_t reset_deadline = time_uptime_ms() + 250u;
            do {
                status = *reg(offset);
                if ((status & EHCI_PORT_RESET) == 0) break;
                __asm__ volatile("pause");
            } while (time_uptime_ms() < reset_deadline);
            if ((status & EHCI_PORT_RESET) != 0) {
                console_write("[ehci] port "); console_write_dec(i + 1u);
                console_write(" reset did not complete (PORTSC=");
                console_write_hex(status); console_write(")\n");
                continue;
            }
            delay_ms(10u);
            status = *reg(offset);
        }

        if ((status & EHCI_PORT_CONNECT) == 0) {
            uint64_t reconnect_deadline = time_uptime_ms() + 1000u;
            do {
                status = *reg(offset);
                if ((status & EHCI_PORT_CONNECT) != 0) break;
                __asm__ volatile("pause");
            } while (time_uptime_ms() < reconnect_deadline);
        }
        if ((status & EHCI_PORT_CONNECT) == 0) {
            console_write("[ehci] port "); console_write_dec(i + 1u);
            console_write(" disconnected during reset (PORTSC=");
            console_write_hex(status); console_write(")\n");
            continue;
        }
        if ((status & EHCI_PORT_ENABLE) == 0) {
            console_write("[ehci] port "); console_write_dec(i + 1u);
            console_write(" did not enable after reset (PORTSC=");
            console_write_hex(status); console_write("); handing to companion\n");
            *reg(offset) = power | (1u << 13);
            continue;
        }
        g_ehci.connected_port = i + 1u;
        if (enumerate_device()) return true;
        /* The current prototype supports one connected mass-storage device. */
        break;
    }
    return g_ehci.storage_ready;
}

static bool start_controller(const pci_usb_controller_t *c) {
    if (!c->ehci_capabilities_read || g_ehci.hhdm == 0 ||
        !legacy_handoff(c)) return false;
    g_ehci.mmio = c->ehci_mmio_virt;
    g_ehci.operational = g_ehci.mmio + c->ehci_capability_length;
    uint32_t command = *reg(c->ehci_capability_length);
    if ((command & 1u) != 0) {
        *reg(c->ehci_capability_length) = command & ~1u;
        if (!wait_reg(c->ehci_capability_length + 4u, 1u << 12, 1u << 12)) return false;
    }
    *reg(c->ehci_capability_length) = 1u << 1;
    if (!wait_reg(c->ehci_capability_length, 1u << 1, 0)) return false;
    g_ehci.port_count = c->ehci_max_ports;
    g_ehci.port_power_control = c->ehci_port_power_control;
    if (g_ehci.port_count == 0 || g_ehci.port_count > 15) return false;
    g_ehci.qh = dma_page(&g_ehci.qh_phys);
    g_ehci.qtd = dma_page(&g_ehci.qtd_phys);
    g_ehci.buffer = dma_page(&g_ehci.buffer_phys);
    if (!g_ehci.qh || !g_ehci.qtd || !g_ehci.buffer) return false;
    g_ehci.qh->horizontal = g_ehci.qh_phys | EHCI_LINK_QH;
    g_ehci.qh->endpoint = (2u << 12) | (1u << 14) | (1u << 15) | (64u << 16);
    g_ehci.qh->capabilities = 0;
    qh_overlay()->next = EHCI_LINK_TERMINATE;
    qh_overlay()->alternate = EHCI_LINK_TERMINATE;
    *reg(c->ehci_capability_length + 0x20u) = 0;
    *reg(c->ehci_capability_length + 0x40u) = 1u;
    *reg(c->ehci_capability_length + 0x18u) = g_ehci.qh_phys;
    *reg(c->ehci_capability_length + 0x00u) = (1u << 5) | 1u;
    if (!wait_reg(c->ehci_capability_length + 4u, 1u << 12, 0) ||
        !wait_reg(c->ehci_capability_length + 4u, 1u << 15, 1u << 15)) return false;
    g_ehci.ready = true;
    (void)scan_connected_ports();
    return true;
}

bool ehci_rescan_storage(void) {
    if (!g_ehci.ready) return false;
    if (g_ehci.storage_ready) return true;
    return scan_connected_ports();
}

bool ehci_init(uint64_t hhdm_offset) {
    if (g_ehci.ready) return true;
    memset(&g_ehci, 0, sizeof(g_ehci));
    g_ehci.hhdm = hhdm_offset;
    for (size_t i = 0; i < pci_usb_controller_count(); i++) {
        pci_usb_controller_t c;
        if (pci_usb_controller_get(i, &c) && c.programming_interface == 0x20 &&
            c.ehci_capabilities_read) return start_controller(&c);
    }
    return false;
}

bool ehci_ready(void) { return g_ehci.ready; }
bool ehci_storage_ready(void) { return g_ehci.storage_ready; }
uint32_t ehci_sector_size(void) { return g_ehci.block_size; }
uint64_t ehci_sector_count(void) { return g_ehci.block_count; }

static bool transfer_ok(uint64_t lba, const void *buf, size_t len,
                        uint32_t *blocks) {
    if (!g_ehci.storage_ready || !buf || len == 0 || len > EHCI_MAX_TRANSFER ||
        len % g_ehci.block_size != 0) return false;
    *blocks = (uint32_t)(len / g_ehci.block_size);
    return lba < g_ehci.block_count && *blocks <= g_ehci.block_count - lba &&
           lba + *blocks - 1u <= UINT32_MAX;
}

bool ehci_read_sector(uint64_t lba, void *buffer, size_t length) {
    uint32_t blocks;
    if (!transfer_ok(lba, buffer, length, &blocks)) return false;
    uint8_t cdb[10] = {0}; cdb[0] = 0x28;
    put_be32(cdb + 2, (uint32_t)lba); cdb[7] = (uint8_t)(blocks >> 8); cdb[8] = (uint8_t)blocks;
    return bot(cdb, 10, true, (uint32_t)length, buffer);
}

bool ehci_write_sector(uint64_t lba, const void *buffer, size_t length) {
    uint32_t blocks;
    if (!transfer_ok(lba, buffer, length, &blocks)) return false;
    uint8_t cdb[10] = {0}; cdb[0] = 0x2a;
    put_be32(cdb + 2, (uint32_t)lba); cdb[7] = (uint8_t)(blocks >> 8); cdb[8] = (uint8_t)blocks;
    return bot(cdb, 10, false, (uint32_t)length, (uint8_t *)(uintptr_t)buffer);
}

bool ehci_flush(void) {
    if (!g_ehci.storage_ready) return false;
    uint8_t cdb[10] = {0}; cdb[0] = 0x35;
    return bot(cdb, 10, false, 0, NULL);
}

void ehci_print_status(void) {
    if (!g_ehci.ready) { console_write("EHCI controller: not started\n"); return; }
    if (!g_ehci.storage_ready) (void)ehci_rescan_storage();
    console_write("EHCI controller: running, polled async schedule active\n");
    if (g_ehci.connected_port != 0) {
        console_write("  connected high-speed port: ");
        console_write_dec(g_ehci.connected_port); console_putc('\n');
    }
    if (g_ehci.storage_ready) {
        console_write("  USB mass storage: BOT transport ready; ");
        console_write_dec(g_ehci.block_count); console_write(" blocks x ");
        console_write_dec(g_ehci.block_size); console_write(" bytes\n");
    } else console_write("  USB mass storage: unavailable\n");
}
