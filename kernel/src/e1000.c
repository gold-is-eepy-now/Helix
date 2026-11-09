/* e1000.c — Intel 82540EM driver (see e1000.h and docs/10-network.md).
 *
 * How the card works, in the order this file uses it:
 *   1. PCI config space tells us where its registers are (BAR0). We enable the
 *      device's memory decoding and bus mastering, since it does DMA.
 *   2. The registers are read and written through MMIO. BAR0 is mapped into a
 *      fixed kernel window with the cache-disable bit, so reads and writes of
 *      device registers are never served from the cache.
 *   3. Two descriptor rings live in memory the card reaches by physical address:
 *      a receive ring (the card writes frames into buffers we gave it) and a
 *      transmit ring (we queue frames, the card reads them out).
 *   4. A ring slot belongs to the card until it sets the DD (descriptor done)
 *      bit in the status byte. Software moves the tail register to hand slots
 *      over, and the head register belongs to the card.
 */
#include "e1000.h"

#include <stddef.h>

#include "klog.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"

#define E1000_VENDOR    0x8086u
#define E1000_DEVICE    0x100Eu
#define E1000_MMIO_VIRT 0xFFFFA00001000000ULL
#define E1000_MMIO_PAGES 32u          /* the register window is 128 KiB */
#define PTE_PCD         (1ULL << 4)   /* page-level cache disable */
#define PCI_COMMAND     0x04u
#define PCI_BAR0        0x10u
#define PCI_CMD_MEMORY  (1u << 1)
#define PCI_CMD_BUSMASTER (1u << 2)

/* Register offsets (bytes from BAR0). */
#define REG_CTRL   0x0000u
#define REG_STATUS 0x0008u
#define REG_EERD   0x0014u
#define REG_IMC    0x00D8u
#define REG_RCTL   0x0100u
#define REG_TCTL   0x0400u
#define REG_TIPG   0x0410u
#define REG_RDBAL  0x2800u
#define REG_RDBAH  0x2804u
#define REG_RDLEN  0x2808u
#define REG_RDH    0x2810u
#define REG_RDT    0x2818u
#define REG_TDBAL  0x3800u
#define REG_TDBAH  0x3804u
#define REG_TDLEN  0x3808u
#define REG_TDH    0x3810u
#define REG_TDT    0x3818u
#define REG_MTA    0x5200u            /* 128 words, the multicast hash table */
#define REG_RAL0   0x5400u
#define REG_RAH0   0x5404u

#define CTRL_RST    (1u << 26)
#define CTRL_SLU    (1u << 6)         /* set link up */
#define STATUS_LU   (1u << 1)         /* link is up */
#define RCTL_EN     (1u << 1)
#define RCTL_BAM    (1u << 15)        /* accept broadcast frames */
#define RCTL_SECRC  (1u << 26)        /* strip the Ethernet CRC */
#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)         /* pad short frames */
#define TCTL_CT     (0x0Fu << 4)      /* collision threshold (standard value) */
#define TCTL_COLD   (0x40u << 12)     /* collision distance, full duplex */
#define RAH_AV      (1u << 31)        /* the address in RAH/RAL is valid */
#define EERD_START  0x1u
#define EERD_DONE   (1u << 4)

/* Legacy transmit descriptor command bits. */
#define TXD_CMD_EOP  (1u << 0)        /* last descriptor of the frame */
#define TXD_CMD_IFCS (1u << 1)        /* the card appends the FCS */
#define TXD_CMD_RS   (1u << 3)        /* report status: set DD when sent */
#define DESC_DD      (1u << 0)

#define RX_COUNT   32u
#define TX_COUNT   8u
#define MAX_FRAME  1518u              /* largest Ethernet frame, FCS excluded */
#define SPIN_LIMIT 1000000u

/* Legacy descriptor layouts, 16 bytes each, as the card defines them. */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} e1000_tx_desc_t;

_Static_assert(sizeof(e1000_rx_desc_t) == 16, "rx descriptor must be 16 bytes");
_Static_assert(sizeof(e1000_tx_desc_t) == 16, "tx descriptor must be 16 bytes");

static bool g_present;
static bool g_link;
static volatile uint8_t *g_regs;
static uint64_t g_hhdm;
static uint8_t g_mac[6];

static volatile e1000_rx_desc_t *g_rx_ring;
static volatile e1000_tx_desc_t *g_tx_ring;
static uint8_t *g_rx_buf[RX_COUNT];   /* virtual addresses of the frame buffers */
static uint8_t *g_tx_buf[TX_COUNT];
static uint32_t g_rx_next;            /* next receive slot software will look at */
static uint32_t g_tx_tail;            /* next transmit slot software will fill */
static uint32_t g_rx_frames;
static uint32_t g_tx_frames;

static uint32_t reg_read(uint32_t off) {
    return *(volatile uint32_t *)(g_regs + off);
}

static void reg_write(uint32_t off, uint32_t value) {
    *(volatile uint32_t *)(g_regs + off) = value;
}

/* Read one 16-bit word from the EEPROM. Used only when RAL0 is empty. */
static uint16_t eeprom_read(uint8_t word) {
    reg_write(REG_EERD, EERD_START | ((uint32_t)word << 8));
    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
        uint32_t v = reg_read(REG_EERD);
        if (v & EERD_DONE) return (uint16_t)(v >> 16);
    }
    return 0;
}

/* Allocate one physical frame, zero it, and return its kernel virtual address.
 * The physical address goes back through *phys_out for the device. */
static uint8_t *frame_alloc(uint64_t *phys_out) {
    uintptr_t phys = pmm_alloc_frame();
    if (phys == 0) return NULL;
    uint8_t *virt = (uint8_t *)(uintptr_t)(phys + g_hhdm);
    for (uint32_t i = 0; i < 4096u; i++) virt[i] = 0;
    if (phys_out) *phys_out = (uint64_t)phys;
    return virt;
}

bool e1000_init(void) {
    g_present = false;
    pci_function_t f;
    bool found = false;
    for (size_t i = 0; pci_find_class(0x02, 0x00, 0x00, i, &f); i++) {
        if (f.vendor_id == E1000_VENDOR && f.device_id == E1000_DEVICE) {
            found = true;
            break;
        }
    }
    if (!found) {
        klog_note(KLOG_INPUT, "[net] no e1000 network card found\n");
        return false;
    }

    uint32_t cmd = pci_config_read32(f.bus, f.device, f.function, PCI_COMMAND);
    pci_config_write32(f.bus, f.device, f.function, PCI_COMMAND,
                       cmd | PCI_CMD_MEMORY | PCI_CMD_BUSMASTER);

    uint32_t bar = pci_config_read32(f.bus, f.device, f.function, PCI_BAR0);
    if ((bar & 1u) != 0 || ((bar >> 1) & 3u) != 0) {
        klog_note(KLOG_INPUT, "[net] e1000 BAR0 is not a 32-bit memory BAR\n");
        return false;
    }
    uint64_t phys = (uint64_t)(bar & ~0xFu);
    g_hhdm = pmm_hhdm_offset();
    for (uint64_t page = 0; page < E1000_MMIO_PAGES; page++) {
        if (!vmm_map(E1000_MMIO_VIRT + page * 4096u, phys + page * 4096u,
                     VMM_PRESENT | VMM_WRITE | VMM_NX | PTE_PCD)) {
            klog_note(KLOG_INPUT, "[net] e1000: could not map its registers\n");
            return false;
        }
    }
    g_regs = (volatile uint8_t *)(uintptr_t)E1000_MMIO_VIRT;

    /* Reset, then wait for the card to clear the reset bit. */
    reg_write(REG_IMC, 0xFFFFFFFFu);      /* no interrupts: we poll */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    for (uint32_t i = 0; i < SPIN_LIMIT && (reg_read(REG_CTRL) & CTRL_RST); i++) {}
    reg_write(REG_IMC, 0xFFFFFFFFu);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU);

    /* MAC address: the receive-address register when the card filled it, else
     * the EEPROM. QEMU fills the register. */
    uint32_t ral = reg_read(REG_RAL0);
    uint32_t rah = reg_read(REG_RAH0);
    if ((rah & RAH_AV) == 0 && ral == 0) {
        uint16_t w0 = eeprom_read(0), w1 = eeprom_read(1), w2 = eeprom_read(2);
        ral = (uint32_t)w0 | ((uint32_t)w1 << 16);
        rah = (uint32_t)w2 | RAH_AV;
        reg_write(REG_RAL0, ral);
        reg_write(REG_RAH0, rah);
    }
    g_mac[0] = (uint8_t)(ral);
    g_mac[1] = (uint8_t)(ral >> 8);
    g_mac[2] = (uint8_t)(ral >> 16);
    g_mac[3] = (uint8_t)(ral >> 24);
    g_mac[4] = (uint8_t)(rah);
    g_mac[5] = (uint8_t)(rah >> 8);

    for (uint32_t i = 0; i < 128u; i++) reg_write(REG_MTA + i * 4u, 0);

    /* Receive ring: each slot points at its own 4 KiB frame buffer. */
    uint64_t ring_phys = 0;
    g_rx_ring = (volatile e1000_rx_desc_t *)frame_alloc(&ring_phys);
    if (g_rx_ring == NULL) return false;
    for (uint32_t i = 0; i < RX_COUNT; i++) {
        uint64_t buf_phys = 0;
        g_rx_buf[i] = frame_alloc(&buf_phys);
        if (g_rx_buf[i] == NULL) return false;
        g_rx_ring[i].addr = buf_phys;
        g_rx_ring[i].status = 0;
    }
    reg_write(REG_RDBAL, (uint32_t)ring_phys);
    reg_write(REG_RDBAH, (uint32_t)(ring_phys >> 32));
    reg_write(REG_RDLEN, RX_COUNT * sizeof(e1000_rx_desc_t));
    reg_write(REG_RDH, 0);
    /* Tail one behind head: the card owns the slots from head up to tail. */
    reg_write(REG_RDT, RX_COUNT - 1);
    g_rx_next = 0;
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    /* Transmit ring: every slot starts free (DD set, nothing queued). */
    uint64_t tx_phys = 0;
    g_tx_ring = (volatile e1000_tx_desc_t *)frame_alloc(&tx_phys);
    if (g_tx_ring == NULL) return false;
    for (uint32_t i = 0; i < TX_COUNT; i++) {
        uint64_t buf_phys = 0;
        g_tx_buf[i] = frame_alloc(&buf_phys);
        if (g_tx_buf[i] == NULL) return false;
        g_tx_ring[i].addr = buf_phys;
        g_tx_ring[i].status = DESC_DD;
    }
    reg_write(REG_TDBAL, (uint32_t)tx_phys);
    reg_write(REG_TDBAH, (uint32_t)(tx_phys >> 32));
    reg_write(REG_TDLEN, TX_COUNT * sizeof(e1000_tx_desc_t));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    g_tx_tail = 0;
    reg_write(REG_TIPG, 0x0060200Au);     /* the inter-packet gap from the manual */
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);

    g_link = (reg_read(REG_STATUS) & STATUS_LU) != 0;
    g_present = true;
    klog_note(KLOG_INPUT, "[net] e1000 ready: MAC %02x:%02x:%02x:%02x:%02x:%02x link %s\n",
              g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5],
              g_link ? "up" : "down");
    return true;
}

bool e1000_present(void) { return g_present; }

bool e1000_link_up(void) {
    if (!g_present) return false;
    g_link = (reg_read(REG_STATUS) & STATUS_LU) != 0;
    return g_link;
}

void e1000_mac(uint8_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = g_mac[i];
}

bool e1000_send(const uint8_t *frame, uint16_t length) {
    if (!g_present || frame == NULL || length == 0 || length > MAX_FRAME) return false;
    volatile e1000_tx_desc_t *d = &g_tx_ring[g_tx_tail];
    /* The slot is free once the card has set DD on it. Wait a bounded time. */
    uint32_t spins = 0;
    while ((d->status & DESC_DD) == 0) {
        if (++spins > SPIN_LIMIT) return false;
    }
    uint8_t *buf = g_tx_buf[g_tx_tail];
    for (uint16_t i = 0; i < length; i++) buf[i] = frame[i];
    d->length = length;
    d->cso = 0;
    d->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    d->status = 0;
    d->css = 0;
    d->special = 0;
    g_tx_tail = (g_tx_tail + 1) % TX_COUNT;
    reg_write(REG_TDT, g_tx_tail);
    g_tx_frames++;
    return true;
}

uint16_t e1000_receive(uint8_t *out, uint16_t capacity) {
    if (!g_present || out == NULL) return 0;
    volatile e1000_rx_desc_t *d = &g_rx_ring[g_rx_next];
    if ((d->status & DESC_DD) == 0) return 0;

    uint16_t length = d->length;
    uint16_t copied = 0;
    if (d->errors == 0 && length <= capacity) {
        uint8_t *buf = g_rx_buf[g_rx_next];
        for (uint16_t i = 0; i < length; i++) out[i] = buf[i];
        copied = length;
        g_rx_frames++;
    }
    /* Hand the slot back: the card may now reuse it. */
    d->status = 0;
    reg_write(REG_RDT, g_rx_next);
    g_rx_next = (g_rx_next + 1) % RX_COUNT;
    return copied;
}

uint32_t e1000_rx_count(void) { return g_rx_frames; }
uint32_t e1000_tx_count(void) { return g_tx_frames; }

/* Stage-1 check, see docs/10-network.md. Sends a broadcast ARP request asking who
 * has the QEMU gateway 10.0.2.2, then waits for the ARP reply that QEMU's user-mode
 * network sends back. Proves transmit, receive and DMA end to end. */
bool e1000_selftest(void) {
    if (!g_present) return false;
    static uint8_t frame[64];
    static uint8_t rx[MAX_FRAME];
    static const uint8_t our_ip[4] = {10, 0, 2, 15};
    static const uint8_t gateway_ip[4] = {10, 0, 2, 2};
    uint8_t *p = frame;

    /* Ethernet header: broadcast destination, our source, ARP ethertype 0x0806. */
    for (int i = 0; i < 6; i++) *p++ = 0xFF;
    for (int i = 0; i < 6; i++) *p++ = g_mac[i];
    *p++ = 0x08; *p++ = 0x06;
    /* ARP: Ethernet/IPv4, request. */
    *p++ = 0x00; *p++ = 0x01;             /* hardware type: Ethernet */
    *p++ = 0x08; *p++ = 0x00;             /* protocol type: IPv4 */
    *p++ = 6; *p++ = 4;                   /* address lengths */
    *p++ = 0x00; *p++ = 0x01;             /* operation: request */
    for (int i = 0; i < 6; i++) *p++ = g_mac[i];       /* sender hardware */
    for (int i = 0; i < 4; i++) *p++ = our_ip[i];      /* sender protocol */
    for (int i = 0; i < 6; i++) *p++ = 0x00;           /* target hardware: unknown */
    for (int i = 0; i < 4; i++) *p++ = gateway_ip[i];  /* target protocol */

    if (!e1000_send(frame, 42)) {
        klog_note(KLOG_INPUT, "[net] selftest: transmit failed\n");
        return false;
    }

    /* Poll for the reply: an ARP frame, operation 2, sent by 10.0.2.2. */
    for (uint32_t spins = 0; spins < 50000000u; spins++) {
        uint16_t n = e1000_receive(rx, MAX_FRAME);
        if (n < 42) continue;
        if (rx[12] != 0x08 || rx[13] != 0x06) continue;
        if (rx[20] != 0x00 || rx[21] != 0x02) continue;
        if (rx[28] != gateway_ip[0] || rx[29] != gateway_ip[1] ||
            rx[30] != gateway_ip[2] || rx[31] != gateway_ip[3]) continue;
        klog_note(KLOG_INPUT, "[net] selftest: ARP reply from 10.0.2.2, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                  rx[22], rx[23], rx[24], rx[25], rx[26], rx[27]);
        return true;
    }
    klog_note(KLOG_INPUT, "[net] selftest: no ARP reply from 10.0.2.2\n");
    return false;
}
