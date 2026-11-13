/* usb_host.c — USB 1.1 host controller (UHCI) with root ports, hubs and HID.
 *
 * Why UHCI and not EHCI or xHCI:
 *   Intel's 2009/2010 chipsets (the iMac 11,2 among them) have EHCI (USB 2.0)
 *   controllers but no xHCI. Each EHCI controller has UHCI "companions" that
 *   own every port which is not high-speed. A keyboard, a mouse, and most hubs
 *   are full- or low-speed, so they are only visible through UHCI. We therefore
 *   put every EHCI controller into reset, which gives all ports to the
 *   companions, and drive the ports from UHCI.
 *
 * What the driver does, in order:
 *   1. Resets each EHCI controller so its ports belong to the companions.
 *   2. Starts each UHCI controller (I/O-port registers, frame list, schedule).
 *   3. Resets each root port that has a device and gives it an address.
 *   4. Reads the device and configuration descriptors. A hub gets its ports
 *      powered, reset and enumerated, recursively. A HID boot keyboard or mouse
 *      gets SET_CONFIGURATION, boot protocol, and an interrupt IN endpoint.
 *   5. usb_host_poll() reads the interrupt endpoints and hands the reports to
 *      usb_hid.
 *
 * How UHCI works, in one paragraph: the controller walks a 1024-entry frame list
 * once per millisecond. Each entry points at a chain of queue heads (QH). A QH
 * holds a chain of transfer descriptors (TD), and each TD asks for one packet.
 * The controller writes the result back into the TD's status word. Software
 * builds TDs, links them, and then looks at their status words. There is no
 * interrupt in this driver; it polls.
 */
#include "usb_host.h"

#include "console.h"
#include "io.h"
#include "kstring.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "time.h"
#include "usb_hid.h"

/* ------------------------------------------------------------- constants */

#define PCI_CLASS_SERIAL_BUS 0x0Cu
#define PCI_SUBCLASS_USB     0x03u
#define PCI_PROGIF_UHCI      0x00u
#define PCI_PROGIF_EHCI      0x20u

/* UHCI registers, I/O space at BAR4. */
#define UHCI_USBCMD   0x00u
#define UHCI_USBSTS   0x02u
#define UHCI_USBINTR  0x04u
#define UHCI_FRNUM    0x06u
#define UHCI_FRBASE   0x08u
#define UHCI_SOFMOD   0x0Cu
#define UHCI_PORTSC(n) (0x10u + 2u * (n))

#define CMD_RS      0x0001u
#define CMD_HCRESET 0x0002u
#define CMD_GRESET  0x0004u
#define CMD_MAXP    0x0080u          /* 64-byte maximum packet */
#define STS_W1C     0x003Fu          /* every status bit is write-1-to-clear */
#define STS_HALTED  0x0020u

#define PORT_CONNECT 0x0001u
#define PORT_CSC     0x0002u         /* connect status change, write 1 to clear */
#define PORT_ENABLE  0x0004u
#define PORT_PEC     0x0008u         /* enable change, write 1 to clear */
#define PORT_LSDA    0x0100u         /* attached device is low-speed */
#define PORT_RESET   0x0200u
#define PORT_ALWAYS1 0x0080u         /* reserved bit, must be written as 1 */

/* Link pointers. */
#define LINK_T  0x1u                 /* terminate */
#define LINK_QH 0x2u                 /* points at a queue head, not a TD */

/* TD control/status word. */
#define TD_ACTIVE   (1u << 23)
#define TD_STALLED  (1u << 22)
#define TD_BUFERR   (1u << 21)
#define TD_BABBLE   (1u << 20)
#define TD_NAK      (1u << 19)
#define TD_CRCTO    (1u << 18)
#define TD_BITSTUFF (1u << 17)
#define TD_ERRORS   (TD_STALLED | TD_BUFERR | TD_BABBLE | TD_CRCTO | TD_BITSTUFF)
#define TD_IOC      (1u << 24)
#define TD_LS       (1u << 26)       /* low-speed device */
#define TD_CERR3    (3u << 27)       /* retry up to three times */

#define PID_OUT   0xE1u
#define PID_IN    0x69u
#define PID_SETUP 0x2Du

#define USB_MAX_HC       8u          /* Ibex Peak: 2 EHCI, each with 3 UHCI companions */
#define USB_MAX_DEVICES  16u
#define USB_MAX_DEPTH    5u          /* hubs nested this many levels deep */
#define TD_POOL          64u         /* TDs available for control transfers */
#define CTL_MAX_LEN      255u        /* largest control data stage we allow */

/* Standard requests and descriptor types. */
#define REQ_GET_STATUS        0x00
#define REQ_CLEAR_FEATURE     0x01
#define REQ_SET_FEATURE       0x03
#define REQ_SET_ADDRESS       0x05
#define REQ_GET_DESCRIPTOR    0x06
#define REQ_SET_CONFIGURATION 0x09
#define REQ_HID_SET_IDLE      0x0A
#define REQ_HID_SET_PROTOCOL  0x0B
#define DESC_DEVICE 1
#define DESC_CONFIG 2
#define DESC_INTERFACE 4
#define DESC_ENDPOINT 5
#define DESC_HUB 0x29
#define CLASS_HUB 9
#define CLASS_HID 3
#define HID_SUBCLASS_BOOT 1
#define HID_PROTOCOL_KEYBOARD 1
#define HID_PROTOCOL_MOUSE 2

/* Hub port features. */
#define HUB_PORT_RESET        4
#define HUB_PORT_POWER        8
#define HUB_C_PORT_CONNECTION 16
#define HUB_C_PORT_RESET      20

#define KIND_NONE     0u
#define KIND_KEYBOARD 1u
#define KIND_MOUSE    2u

/* ------------------------------------------------------------- structures */

/* Layouts fixed by the UHCI specification (all little-endian, 16-byte aligned). */
typedef struct {
    uint32_t link;
    uint32_t status;
    uint32_t token;
    uint32_t buffer;
} uhci_td_t;

typedef struct {
    uint32_t horizontal;
    uint32_t element;
} uhci_qh_t;

typedef struct uhci_hc uhci_hc_t;

/* One interrupt IN endpoint of a HID boot interface. A device can have several
 * (a keyboard interface and a mouse-protocol interface on the same device), so
 * each one gets its own queue head, TD and report buffer. */
typedef struct {
    uint8_t kind;             /* KIND_* from the interface's protocol byte */
    uint8_t interface;
    uint8_t endpoint;         /* endpoint number, 1..15 */
    uint8_t packet;           /* wMaxPacketSize, clamped to 8..64 */
    bool armed;
    bool toggle;              /* DATA0/DATA1 for the next transfer */
    uint8_t errors;
    uint32_t reports;         /* reports delivered to usb_hid */
    uint32_t last_status;     /* TD status word at the last completion */
    uint8_t last[8];          /* first bytes of the last report, for diagnosis */
    uhci_qh_t *qh;
    uint32_t qh_phys;
    uhci_td_t *td;
    uint32_t td_phys;
    uint8_t *buf;
    uint32_t buf_phys;
} usb_ep_t;

#define USB_MAX_EPS 4u

/* Where a device sits: the root port number, then one hub port per level. */
typedef struct {
    uint8_t len;
    uint8_t ports[USB_MAX_DEPTH + 2u];
} usb_path_t;

typedef struct {
    uhci_hc_t *hc;
    uint8_t address;          /* 0 until SET_ADDRESS succeeds */
    bool low_speed;
    uint8_t max_packet0;
    bool is_hub;
    uint8_t ep_count;
    uint16_t vid;             /* idVendor from the device descriptor */
    uint16_t pid;             /* idProduct */
    uint8_t dev_class;        /* bDeviceClass */
    usb_path_t path;
    usb_ep_t eps[USB_MAX_EPS];
} usb_device_t;

struct uhci_hc {
    uint8_t bus, device, function;
    uint16_t io;              /* I/O-port base from BAR4 */
    uint32_t *frame_list;     /* 1024 entries */
    uint32_t frame_phys;
    uhci_qh_t *qh_int;        /* interrupt polling head; links to qh_ctl */
    uint32_t qh_int_phys;
    uhci_qh_t *qh_ctl;        /* control transfers */
    uint32_t qh_ctl_phys;
    uhci_td_t *tds;           /* pool for control transfers */
    uint32_t tds_phys;
    uint8_t *ctl_buf;         /* setup packet at +0, data stage at +64 */
    uint32_t ctl_buf_phys;
    uint8_t next_address;     /* next USB address to hand out (1..127) */
};

/* ------------------------------------------------------------- state */

static uint64_t g_hhdm;
static uhci_hc_t g_hc[USB_MAX_HC];
static unsigned g_hc_count;
static usb_device_t g_dev[USB_MAX_DEVICES];
static unsigned g_dev_count;
static bool g_polling;

/* Virtual window for EHCI register pages. Limine's direct map covers RAM, not
 * the PCI memory hole where the firmware put the EHCI BAR, so we map it
 * ourselves: one 4 KiB page per controller, uncacheable, non-executable. */
#define EHCI_MMIO_VIRT 0xFFFFA00000000000ULL
#define PTE_PCD        (1ULL << 4)            /* page-level cache disable */

/* ------------------------------------------------------------- helpers */

static uint16_t hc_r16(const uhci_hc_t *hc, uint16_t off) { return inw((uint16_t)(hc->io + off)); }
static void hc_w16(const uhci_hc_t *hc, uint16_t off, uint16_t v) { outw((uint16_t)(hc->io + off), v); }
static void hc_w32(const uhci_hc_t *hc, uint16_t off, uint32_t v) { outl((uint16_t)(hc->io + off), v); }
static void hc_w8(const uhci_hc_t *hc, uint16_t off, uint8_t v) { outb((uint16_t)(hc->io + off), v); }

static uint32_t mmio_r32(uintptr_t addr) { return *(volatile uint32_t *)addr; }
static void mmio_w32(uintptr_t addr, uint32_t v) { *(volatile uint32_t *)addr = v; }

static void delay_ms(uint64_t ms) {
    uint64_t until = time_uptime_ms() + ms;
    while (time_uptime_ms() < until) __asm__ volatile("pause" ::: "memory");
}

/* A zeroed page the controller can read: it must be below 4 GiB, because the
 * controller's pointers are 32 bits wide. */
static void *dma_page(uint32_t *phys_out) {
    uintptr_t phys = pmm_alloc_frame();
    if (phys == 0) return NULL;
    if (phys > 0xFFFFF000u) {
        pmm_free_frame(phys);
        return NULL;
    }
    memset((void *)(phys + g_hhdm), 0, PAGE_SIZE);
    *phys_out = (uint32_t)phys;
    return (void *)(phys + g_hhdm);
}

/* Fill an 8-byte SETUP packet (USB 2.0 section 9.3). */
static void make_setup(uint8_t out[8], uint8_t request_type, uint8_t request, uint16_t value,
                       uint16_t index, uint16_t length) {
    out[0] = request_type;
    out[1] = request;
    out[2] = (uint8_t)value;
    out[3] = (uint8_t)(value >> 8);
    out[4] = (uint8_t)index;
    out[5] = (uint8_t)(index >> 8);
    out[6] = (uint8_t)length;
    out[7] = (uint8_t)(length >> 8);
}

/* ------------------------------------------------------------- EHCI handoff */

/* Put one EHCI controller into reset. A reset controller routes every port to
 * its UHCI companions, which is what we want: see the top of this file. */
static void ehci_release(const pci_function_t *f, unsigned slot) {
    uint32_t bar = pci_config_read32(f->bus, f->device, f->function, 0x10);
    if ((bar & 1u) != 0 || (bar & ~0xFu) == 0) return;   /* not a memory BAR */
    uint64_t phys = (uint64_t)(bar & ~0xFu);
    uint64_t page = EHCI_MMIO_VIRT + (uint64_t)slot * PAGE_SIZE;
    uint64_t old_phys = 0, old_flags = 0;
    if (vmm_get_mapping(page, &old_phys, &old_flags)) return;   /* window already used */
    if (!vmm_map(page, phys, VMM_PRESENT | VMM_WRITE | VMM_NX | PTE_PCD)) {
        console_write("[usb] ehci: could not map registers\n");
        return;
    }
    uintptr_t cap = (uintptr_t)(page + (phys & (PAGE_SIZE - 1u)));
    uint8_t cap_length = *(volatile uint8_t *)cap;
    uint32_t hcsparams = mmio_r32(cap + 4u);
    uint32_t hccparams = mmio_r32(cap + 8u);
    uintptr_t op = cap + cap_length;
    unsigned ports = hcsparams & 0xFu;
    uint8_t eecp = (uint8_t)((hccparams >> 8) & 0xFFu);

    /* BIOS handoff: the firmware may own the controller (USB legacy support). */
    if (eecp >= 0x40u) {
        uint32_t cap0 = pci_config_read32(f->bus, f->device, f->function, eecp);
        if ((cap0 & 0xFFu) == 0x01u) {
            pci_config_write32(f->bus, f->device, f->function, eecp, cap0 | (1u << 24));
            uint64_t until = time_uptime_ms() + 1000u;
            while ((pci_config_read32(f->bus, f->device, f->function, eecp) & (1u << 16)) != 0 &&
                   time_uptime_ms() < until)
                __asm__ volatile("pause" ::: "memory");
            pci_config_write32(f->bus, f->device, f->function, (uint8_t)(eecp + 4u), 0);
        }
    }

    mmio_w32(op, mmio_r32(op) & ~1u);                    /* USBCMD.RS = 0 */
    uint64_t until = time_uptime_ms() + 100u;
    while ((mmio_r32(op + 4u) & (1u << 12)) == 0 && time_uptime_ms() < until)
        __asm__ volatile("pause" ::: "memory");          /* wait for HCHalted */
    mmio_w32(op, mmio_r32(op) | 2u);                     /* USBCMD.HCRESET */
    until = time_uptime_ms() + 250u;
    while ((mmio_r32(op) & 2u) != 0 && time_uptime_ms() < until)
        __asm__ volatile("pause" ::: "memory");

    console_write("[usb] ehci at ");
    console_write_dec(f->bus);
    console_write(":");
    console_write_dec(f->device);
    console_write(": reset, ");
    console_write_dec(ports);
    console_write(" ports now belong to the companion controllers\n");
}

/* ------------------------------------------------------------- UHCI start-up */

static bool uhci_start(uhci_hc_t *hc, const pci_function_t *f) {
    hc->bus = f->bus;
    hc->device = f->device;
    hc->function = f->function;

    uint32_t bar4 = pci_config_read32(f->bus, f->device, f->function, 0x20);
    if ((bar4 & 1u) == 0) return false;                  /* must be I/O space */
    hc->io = (uint16_t)(bar4 & 0xFFE0u);

    /* PCI command: enable I/O decoding and bus mastering (the controller reads
     * our frame list from RAM). Status bits are write-1-to-clear, so zero them. */
    uint32_t command = pci_config_read32(f->bus, f->device, f->function, 0x04);
    pci_config_write32(f->bus, f->device, f->function, 0x04, (command & 0xFFFFu) | 0x5u);
    /* USBLEGSUP: clear the firmware's PIRQ/SMI routing so it stops touching us. */
    pci_config_write32(f->bus, f->device, f->function, 0xC0, 0x2000u);

    uint32_t frame_phys = 0, qh_phys = 0, buf_phys = 0;
    uint32_t *frame = dma_page(&frame_phys);
    uint8_t *qh_page = dma_page(&qh_phys);
    uint8_t *buf = dma_page(&buf_phys);
    if (frame == NULL || qh_page == NULL || buf == NULL) return false;
    hc->frame_list = frame;
    hc->frame_phys = frame_phys;
    hc->qh_int = (uhci_qh_t *)(void *)qh_page;
    hc->qh_int_phys = qh_phys;
    hc->qh_ctl = (uhci_qh_t *)(void *)(qh_page + 16);
    hc->qh_ctl_phys = qh_phys + 16u;
    hc->tds = (uhci_td_t *)(void *)(qh_page + 256);      /* 64 * 16 bytes fits in the page */
    hc->tds_phys = qh_phys + 256u;
    hc->ctl_buf = buf;
    hc->ctl_buf_phys = buf_phys;

    /* Reset. Stop first, then a global reset, then a host-controller reset. */
    hc_w16(hc, UHCI_USBCMD, 0);
    uint64_t until = time_uptime_ms() + 10u;
    while ((hc_r16(hc, UHCI_USBSTS) & STS_HALTED) == 0 && time_uptime_ms() < until)
        __asm__ volatile("pause" ::: "memory");
    hc_w16(hc, UHCI_USBCMD, CMD_GRESET);
    delay_ms(50);
    hc_w16(hc, UHCI_USBCMD, 0);
    delay_ms(10);
    hc_w16(hc, UHCI_USBCMD, CMD_HCRESET);
    until = time_uptime_ms() + 50u;
    while ((hc_r16(hc, UHCI_USBCMD) & CMD_HCRESET) != 0 && time_uptime_ms() < until)
        __asm__ volatile("pause" ::: "memory");

    hc_w16(hc, UHCI_USBSTS, STS_W1C);
    hc_w16(hc, UHCI_USBINTR, 0);                         /* we poll: no interrupts */
    hc_w16(hc, UHCI_FRNUM, 0);
    hc_w32(hc, UHCI_FRBASE, frame_phys);
    hc_w8(hc, UHCI_SOFMOD, 0x40);                        /* standard 1 ms frame */

    /* Schedule: every frame -> qh_int -> qh_ctl -> end. Nothing is queued yet. */
    hc->qh_ctl->horizontal = LINK_T;
    hc->qh_ctl->element = LINK_T;
    hc->qh_int->horizontal = hc->qh_ctl_phys | LINK_QH;
    hc->qh_int->element = LINK_T;
    for (unsigned i = 0; i < 1024u; i++) hc->frame_list[i] = hc->qh_int_phys | LINK_QH;
    __sync_synchronize();

    hc_w16(hc, UHCI_USBCMD, (uint16_t)(CMD_RS | CMD_MAXP));
    until = time_uptime_ms() + 20u;
    while ((hc_r16(hc, UHCI_USBSTS) & STS_HALTED) != 0 && time_uptime_ms() < until)
        __asm__ volatile("pause" ::: "memory");
    if ((hc_r16(hc, UHCI_USBSTS) & STS_HALTED) != 0) return false;

    hc->next_address = 1;
    return true;
}

/* ------------------------------------------------------------- transfers */

/* Run one control transfer (SETUP, optional data stage, STATUS) on endpoint 0 of
 * the device described by t. Returns false on any error, timeout, or stall.
 *
 * UHCI has no per-transfer packet-size field, so the data stage is split into
 * one TD per packet of at most max_packet0 bytes. The data toggle starts at
 * DATA1 for the data stage and alternates per packet; STATUS is always DATA1. */
static bool control(const usb_device_t *t, const uint8_t setup[8], uint8_t *data,
                    uint16_t length) {
    uhci_hc_t *hc = t->hc;
    bool in = (setup[0] & 0x80u) != 0;
    if (length > CTL_MAX_LEN || t->max_packet0 == 0) return false;

    memcpy(hc->ctl_buf, setup, 8);
    if (!in && length != 0) memcpy(hc->ctl_buf + 64, data, length);

    uint32_t chunks = (uint32_t)(length + t->max_packet0 - 1u) / t->max_packet0;
    uint32_t count = 1u + chunks + 1u;                   /* setup + data + status */
    if (count > TD_POOL) return false;

    uhci_td_t *tds = hc->tds;
    uint32_t cerr = TD_CERR3 | (t->low_speed ? TD_LS : 0u);
    uint32_t addr = (uint32_t)t->address << 8;

    tds[0].token = (7u << 21) | addr | PID_SETUP;        /* 8 bytes, toggle 0 */
    tds[0].buffer = hc->ctl_buf_phys;
    tds[0].status = TD_ACTIVE | cerr;

    uint32_t done = 0;
    for (uint32_t i = 0; i < chunks; i++) {
        uint32_t n = length - done;
        if (n > t->max_packet0) n = t->max_packet0;
        uint32_t toggle = (i % 2u == 0u) ? 1u : 0u;      /* DATA1, DATA0, DATA1, ... */
        tds[1u + i].token = ((n - 1u) << 21) | (toggle << 19) | addr | (in ? PID_IN : PID_OUT);
        tds[1u + i].buffer = hc->ctl_buf_phys + 64u + done;
        tds[1u + i].status = TD_ACTIVE | cerr;
        done += n;
    }

    /* STATUS: opposite direction to the data stage; zero-length, always DATA1. */
    uint32_t s = 1u + chunks;
    bool status_out = in && length != 0;
    tds[s].token = (0x7FFu << 21) | (1u << 19) | addr | (status_out ? PID_OUT : PID_IN);
    tds[s].buffer = 0;
    tds[s].status = TD_ACTIVE | cerr | TD_IOC;

    for (uint32_t i = 0; i < count; i++)
        tds[i].link = (i + 1u < count) ? hc->tds_phys + (i + 1u) * 16u : LINK_T;

    hc->qh_ctl->element = hc->tds_phys;                  /* start the chain */
    uint64_t deadline = time_uptime_ms() + 500u;
    for (;;) {
        bool stop = (hc->qh_ctl->element & LINK_T) != 0;
        for (uint32_t i = 0; i < count && !stop; i++)
            if ((tds[i].status & TD_ACTIVE) == 0 && (tds[i].status & TD_ERRORS) != 0) stop = true;
        if (stop || time_uptime_ms() >= deadline) break;
        __asm__ volatile("pause" ::: "memory");
    }

    bool ok = (hc->qh_ctl->element & LINK_T) != 0;
    for (uint32_t i = 0; i < count; i++)
        if (tds[i].status & (TD_ACTIVE | TD_ERRORS)) ok = false;
    hc->qh_ctl->element = LINK_T;                        /* leave the chain empty */

    if (ok && in && length != 0) memcpy(data, hc->ctl_buf + 64, length);
    return ok;
}

static bool get_descriptor(const usb_device_t *t, uint8_t type, uint8_t index, uint8_t *data,
                           uint16_t length) {
    uint8_t s[8];
    make_setup(s, 0x80, REQ_GET_DESCRIPTOR, (uint16_t)((type << 8) | index), 0, length);
    return control(t, s, data, length);
}

static bool set_address(const usb_device_t *t, uint8_t address) {
    uint8_t s[8];
    make_setup(s, 0x00, REQ_SET_ADDRESS, address, 0, 0);
    return control(t, s, NULL, 0);
}

static bool set_configuration(const usb_device_t *t, uint8_t value) {
    uint8_t s[8];
    make_setup(s, 0x00, REQ_SET_CONFIGURATION, value, 0, 0);
    return control(t, s, NULL, 0);
}

static bool hub_feature(const usb_device_t *h, uint8_t request, uint16_t feature, uint8_t port) {
    uint8_t s[8];
    make_setup(s, 0x23, request, feature, port, 0);   /* host -> hub, class, other */
    return control(h, s, NULL, 0);
}

static bool hub_port_status(const usb_device_t *h, uint8_t port, uint16_t *status,
                            uint16_t *change) {
    uint8_t s[8];
    uint8_t data[4];
    make_setup(s, 0xA3, REQ_GET_STATUS, 0, port, 4);  /* hub -> host, class, other */
    if (!control(h, s, data, 4)) return false;
    *status = (uint16_t)(data[0] | (data[1] << 8));
    *change = (uint16_t)(data[2] | (data[3] << 8));
    return true;
}

/* ------------------------------------------------------------- HID interrupt */

/* Point the endpoint's TD at the next report. Called after every completion. */
static void interrupt_arm(const usb_device_t *d, usb_ep_t *e) {
    e->td->link = LINK_T;
    e->td->buffer = e->buf_phys;
    e->td->token = ((uint32_t)(e->packet - 1u) << 21) | ((e->toggle ? 1u : 0u) << 19) |
                   ((uint32_t)e->endpoint << 15) | ((uint32_t)d->address << 8) | PID_IN;
    e->td->status = TD_ACTIVE | TD_CERR3 | (d->low_speed ? TD_LS : 0u);
    __sync_synchronize();
    e->qh->element = e->td_phys;
}

/* Give the endpoint its own queue head, TD and report buffer (one page), and
 * splice its queue head into the interrupt chain ahead of the control queue. */
static void interrupt_attach(usb_device_t *d, usb_ep_t *e) {
    uhci_hc_t *hc = d->hc;
    uint32_t page_phys = 0;
    uint8_t *page = dma_page(&page_phys);
    if (page == NULL) return;
    e->qh = (uhci_qh_t *)(void *)page;
    e->qh_phys = page_phys;
    e->td = (uhci_td_t *)(void *)(page + 16);
    e->td_phys = page_phys + 16u;
    e->buf = page + 64;
    e->buf_phys = page_phys + 64u;
    e->toggle = false;

    interrupt_arm(d, e);
    e->qh->horizontal = hc->qh_int->horizontal;          /* splice in before qh_ctl */
    __sync_synchronize();
    hc->qh_int->horizontal = e->qh_phys | LINK_QH;
    e->armed = true;
}

void usb_host_poll(void) {
    if (g_polling) return;
    g_polling = true;
    for (unsigned i = 0; i < g_dev_count; i++) {
        usb_device_t *d = &g_dev[i];
        for (unsigned k = 0; k < d->ep_count; k++) {
            usb_ep_t *e = &d->eps[k];
            if (!e->armed) continue;
            uint32_t st = e->td->status;
            if (st & TD_ACTIVE) continue;                /* no report yet (NAK) */
            e->last_status = st;
            if (st & TD_STALLED) {
                /* Clear ENDPOINT_HALT; the device restarts its toggle at DATA0. */
                uint8_t s[8];
                make_setup(s, 0x02, REQ_CLEAR_FEATURE, 0, (uint16_t)(e->endpoint | 0x80u), 0);
                (void)control(d, s, NULL, 0);
                e->toggle = false;
                e->errors++;
            } else if (st & (TD_BUFERR | TD_BABBLE | TD_CRCTO | TD_BITSTUFF)) {
                e->errors++;                             /* keep the toggle: the device resends */
            } else {
                unsigned got = st & 0x7FFu;
                got = (got == 0x7FFu) ? 0u : got + 1u;  /* field holds length - 1 */
                if (got != 0) {
                    for (unsigned b = 0; b < 8u && b < got; b++) e->last[b] = e->buf[b];
                    if (e->kind == KIND_KEYBOARD) usb_hid_keyboard_report(e->buf, got);
                    else if (e->kind == KIND_MOUSE) usb_hid_mouse_report(e->buf, got);
                    e->reports++;
                }
                e->toggle = !e->toggle;
                e->errors = 0;
            }
            interrupt_arm(d, e);
        }
    }
    g_polling = false;
}

/* ------------------------------------------------------------- enumeration */

/* Diagnostic printing: hex digits and "root.hub.hub" port paths. */
static void print_hex8(uint8_t v) {
    static const char digits[] = "0123456789abcdef";
    console_putc(digits[v >> 4]);
    console_putc(digits[v & 0xF]);
}

static void print_hex16(uint16_t v) {
    print_hex8((uint8_t)(v >> 8));
    print_hex8((uint8_t)v);
}

static void print_path(const usb_path_t *p) {
    for (unsigned i = 0; i < p->len; i++) {
        if (i != 0) console_putc('.');
        console_write_dec(p->ports[i]);
    }
}

static usb_device_t *new_device(uhci_hc_t *hc) {
    if (g_dev_count >= USB_MAX_DEVICES) return NULL;
    usb_device_t *d = &g_dev[g_dev_count++];
    memset(d, 0, sizeof(*d));
    d->hc = hc;
    return d;
}

static void hub_start(usb_device_t *h, unsigned depth);
static void enumerate(uhci_hc_t *hc, bool low_speed, unsigned depth, const usb_path_t *path);

/* Read the configuration tree and set up whatever it describes. The first
 * interface of interest decides what we do with the device. */
static void configure(usb_device_t *d, const uint8_t *cfg, unsigned total, bool device_is_hub) {
    bool any_hub = device_is_hub;
    uint8_t interface = 0;
    uint8_t iface_kind = KIND_NONE;

    unsigned pos = 0;
    while (pos + 2u <= total) {
        uint8_t len = cfg[pos];
        uint8_t type = cfg[pos + 1];
        if (len < 2u || pos + len > total) break;
        if (type == DESC_INTERFACE && len >= 9u) {
            uint8_t cls = cfg[pos + 5], sub = cfg[pos + 6], proto = cfg[pos + 7];
            interface = cfg[pos + 2];
            if (cls == CLASS_HUB) any_hub = true;
            iface_kind = KIND_NONE;
            if (cls == CLASS_HID && sub == HID_SUBCLASS_BOOT) {
                if (proto == HID_PROTOCOL_KEYBOARD) iface_kind = KIND_KEYBOARD;
                else if (proto == HID_PROTOCOL_MOUSE) iface_kind = KIND_MOUSE;
            }
        } else if (type == DESC_ENDPOINT && len >= 7u && iface_kind != KIND_NONE) {
            uint8_t addr = cfg[pos + 2], attr = cfg[pos + 3];
            if ((addr & 0x80u) != 0 && (attr & 3u) == 3u && d->ep_count < USB_MAX_EPS) {
                usb_ep_t *e = &d->eps[d->ep_count++];
                memset(e, 0, sizeof(*e));
                e->kind = iface_kind;
                e->interface = interface;
                e->endpoint = addr & 0x0Fu;
                unsigned packet = cfg[pos + 4] | (cfg[pos + 5] << 8);
                e->packet = (uint8_t)(packet == 0 ? 8u : packet > 64u ? 64u : packet);
            }
        }
        pos += len;
    }
    d->is_hub = any_hub;
}

static void enumerate(uhci_hc_t *hc, bool low_speed, unsigned depth, const usb_path_t *path) {
    if (depth > USB_MAX_DEPTH || hc->next_address > 127u || g_dev_count >= USB_MAX_DEVICES) {
        console_write("[usb] too many devices or hubs deep; skipping one\n");
        return;
    }

    /* A new device answers at address 0 and tells us its max packet size first. */
    usb_device_t probe;
    memset(&probe, 0, sizeof(probe));
    probe.hc = hc;
    probe.low_speed = low_speed;
    probe.max_packet0 = 8;
    uint8_t first[8];
    bool answered = false;
    for (unsigned attempt = 0; attempt < 4u && !answered; attempt++) {
        if (attempt != 0) delay_ms(50);                  /* let a slow device settle */
        answered = get_descriptor(&probe, DESC_DEVICE, 0, first, 8);
    }
    if (!answered) {
        console_write("[usb] device at path ");
        print_path(path);
        console_write(" did not answer at address 0 after 4 tries\n");
        return;
    }
    uint8_t max_packet0 = first[7];
    if (max_packet0 != 8 && max_packet0 != 16 && max_packet0 != 32 && max_packet0 != 64)
        max_packet0 = 8;
    probe.max_packet0 = max_packet0;

    uint8_t address = hc->next_address++;
    if (!set_address(&probe, address)) {
        console_write("[usb] SET_ADDRESS failed\n");
        return;
    }
    delay_ms(10);                                        /* recovery time after SET_ADDRESS */
    probe.address = address;

    uint8_t dev[18];
    if (!get_descriptor(&probe, DESC_DEVICE, 0, dev, 18)) return;
    usb_device_t *d = new_device(hc);
    if (d == NULL) return;
    d->address = address;
    d->low_speed = low_speed;
    d->max_packet0 = max_packet0;
    d->vid = (uint16_t)(dev[8] | (dev[9] << 8));
    d->pid = (uint16_t)(dev[10] | (dev[11] << 8));
    d->dev_class = dev[4];
    d->path = *path;
    console_write("[usb] address ");
    console_write_dec(address);
    console_write(": device ");
    print_hex16(d->vid);
    console_putc(':');
    print_hex16(d->pid);
    console_write(" class 0x");
    print_hex8(d->dev_class);
    console_write(" at path ");
    print_path(&d->path);
    console_write("\n");

    uint8_t head[9];
    if (!get_descriptor(d, DESC_CONFIG, 0, head, 9)) return;
    unsigned total = (unsigned)(head[2] | (head[3] << 8));
    if (total < 9u) total = 9u;
    if (total > CTL_MAX_LEN) total = CTL_MAX_LEN;
    uint8_t cfg[CTL_MAX_LEN];
    if (!get_descriptor(d, DESC_CONFIG, 0, cfg, (uint16_t)total)) return;

    configure(d, cfg, total, dev[4] == CLASS_HUB);
    if (!set_configuration(d, cfg[5])) {
        console_write("[usb] SET_CONFIGURATION failed\n");
        return;
    }

    /* Boot protocol gives fixed 8-byte keyboard / 3-byte mouse reports, and idle 0
     * means "report only when something changes". Stalls are not fatal. */
    for (unsigned k = 0; k < d->ep_count; k++) {
        usb_ep_t *e = &d->eps[k];
        uint8_t s[8];
        make_setup(s, 0x21, REQ_HID_SET_PROTOCOL, 0, e->interface, 0);
        bool proto_ok = control(d, s, NULL, 0);
        make_setup(s, 0x21, REQ_HID_SET_IDLE, 0, e->interface, 0);
        (void)control(d, s, NULL, 0);
        interrupt_attach(d, e);
        console_write("[usb] address ");
        console_write_dec(d->address);
        console_write(" interface ");
        console_write_dec(e->interface);
        console_write(": HID ");
        console_write(e->kind == KIND_KEYBOARD ? "keyboard" : "mouse");
        console_write(", endpoint ");
        console_write_dec(e->endpoint);
        console_write(", packet ");
        console_write_dec(e->packet);
        console_write(", device ");
        print_hex16(d->vid);
        console_putc(':');
        print_hex16(d->pid);
        console_write(", path ");
        print_path(&d->path);
        console_write(proto_ok ? "" : " (SET_PROTOCOL stalled)");
        console_write(e->armed ? "\n" : " (could not arm)\n");
    }
    if (d->is_hub) hub_start(d, depth);
}

static void hub_start(usb_device_t *h, unsigned depth) {
    uint8_t s[8];
    uint8_t hd[9];
    make_setup(s, 0xA0, REQ_GET_DESCRIPTOR, (uint16_t)(DESC_HUB << 8), 0, 9);
    if (!control(h, s, hd, 9)) return;
    unsigned ports = hd[2];
    if (ports == 0u) return;
    if (ports > 15u) ports = 15u;
    uint32_t power_wait = (uint32_t)hd[5] * 2u;          /* bPwrOn2PwrGood, in 2 ms units */
    if (power_wait < 100u) power_wait = 100u;

    for (unsigned p = 1; p <= ports; p++) {
        if (!hub_feature(h, REQ_SET_FEATURE, HUB_PORT_POWER, (uint8_t)p)) {
            console_write("[usb] port power request failed on ");
            print_path(&h->path);
            console_putc('.');
            console_write_dec(p);
            console_write("\n");
        }
    }
    delay_ms(power_wait);

    console_write("[usb] hub at address ");
    console_write_dec(h->address);
    console_write(", path ");
    print_path(&h->path);
    console_write(" with ");
    console_write_dec(ports);
    console_write(" ports\n");

    for (unsigned p = 1; p <= ports; p++) {
        uint16_t status = 0, change = 0;
        if (!hub_port_status(h, (uint8_t)p, &status, &change)) {
            console_write("[usb] port status request failed on ");
            print_path(&h->path);
            console_putc('.');
            console_write_dec(p);
            console_write("\n");
            continue;
        }
        console_write("[usb] port ");
        print_path(&h->path);
        console_putc('.');
        console_write_dec(p);
        console_write(": status 0x");
        print_hex16(status);
        console_write(" change 0x");
        print_hex16(change);
        console_write("\n");
        if ((status & 1u) == 0) continue;                /* nothing plugged in */
        (void)hub_feature(h, REQ_CLEAR_FEATURE, HUB_C_PORT_CONNECTION, (uint8_t)p);
        (void)hub_feature(h, REQ_SET_FEATURE, HUB_PORT_RESET, (uint8_t)p);
        uint64_t deadline = time_uptime_ms() + 200u;
        do {
            delay_ms(2);
            if (!hub_port_status(h, (uint8_t)p, &status, &change)) break;
        } while ((change & (1u << 4)) == 0 && time_uptime_ms() < deadline);   /* C_PORT_RESET */
        (void)hub_feature(h, REQ_CLEAR_FEATURE, HUB_C_PORT_RESET, (uint8_t)p);
        if (!hub_port_status(h, (uint8_t)p, &status, &change)) continue;
        if ((status & 2u) == 0) continue;                /* reset did not enable the port */
        bool low_speed = (status & (1u << 9)) != 0;
        usb_path_t child = h->path;
        if (child.len < sizeof(child.ports)) child.ports[child.len++] = (uint8_t)p;
        enumerate(h->hc, low_speed, depth + 1u, &child);
    }
}

/* One root port: reset it, enable it, and enumerate whatever is behind it. */
static void root_port(uhci_hc_t *hc, unsigned n) {
    uint16_t off = UHCI_PORTSC(n);
    uint16_t sc = hc_r16(hc, off);
    if ((sc & PORT_CONNECT) == 0) return;

    hc_w16(hc, off, (uint16_t)(PORT_ALWAYS1 | PORT_RESET));
    delay_ms(50);
    hc_w16(hc, off, PORT_ALWAYS1);                       /* release reset */
    delay_ms(20);                                        /* settle: some devices are slow */
    /* Enable the port and acknowledge the change bits raised by the connect. */
    hc_w16(hc, off, (uint16_t)(PORT_ALWAYS1 | PORT_ENABLE | PORT_CSC | PORT_PEC));
    delay_ms(10);
    sc = hc_r16(hc, off);
    if ((sc & PORT_ENABLE) == 0) {
        console_write("[usb] root port ");
        console_write_dec(n + 1u);
        console_write(" did not enable\n");
        return;
    }
    usb_path_t path;
    path.len = 1;
    path.ports[0] = (uint8_t)(n + 1u);
    enumerate(hc, (sc & PORT_LSDA) != 0, 0, &path);
}

/* ------------------------------------------------------------- status */

void usb_host_summary(usb_summary_t *out) {
    out->controllers = g_hc_count;
    out->devices = g_dev_count;
    out->keyboards = out->mice = 0;
    out->keyboard_reports = out->keyboard_errors = 0;
    out->mouse_reports = out->mouse_errors = 0;
    for (unsigned i = 0; i < g_dev_count; i++) {
        const usb_device_t *d = &g_dev[i];
        for (unsigned k = 0; k < d->ep_count; k++) {
            const usb_ep_t *e = &d->eps[k];
            if (e->kind == KIND_KEYBOARD) {
                out->keyboards++;
                out->keyboard_reports += e->reports;
                out->keyboard_errors += e->errors;
            } else if (e->kind == KIND_MOUSE) {
                out->mice++;
                out->mouse_reports += e->reports;
                out->mouse_errors += e->errors;
            }
        }
    }
}

void usb_host_print_status(void) {
    console_write("usb: ");
    console_write_dec(g_hc_count);
    console_write(" uhci controller(s), ");
    console_write_dec(g_dev_count);
    console_write(" device(s)\n");
    for (unsigned i = 0; i < g_dev_count; i++) {
        const usb_device_t *d = &g_dev[i];
        console_write("  address ");
        console_write_dec(d->address);
        console_write(": ");
        console_write(d->is_hub ? "hub" : "device");
        console_write(d->low_speed ? ", low-speed" : ", full-speed");
        console_write(", device ");
        print_hex16(d->vid);
        console_putc(':');
        print_hex16(d->pid);
        console_write(", path ");
        print_path(&d->path);
        console_write("\n");
        for (unsigned k = 0; k < d->ep_count; k++) {
            const usb_ep_t *e = &d->eps[k];
            console_write("    interface ");
            console_write_dec(e->interface);
            console_write(" HID ");
            console_write(e->kind == KIND_KEYBOARD ? "keyboard" : "mouse");
            console_write(": reports ");
            console_write_dec(e->reports);
            console_write(", errors ");
            console_write_dec(e->errors);
            console_write(", status 0x");
            print_hex8((uint8_t)(e->last_status >> 24));
            print_hex8((uint8_t)(e->last_status >> 16));
            print_hex8((uint8_t)(e->last_status >> 8));
            print_hex8((uint8_t)e->last_status);
            console_write(", last");
            for (unsigned b = 0; b < 8u; b++) {
                console_putc(' ');
                print_hex8(e->last[b]);
            }
            console_write("\n");
        }
    }
}

/* ------------------------------------------------------------- entry points */

bool usb_host_init(uint64_t hhdm_offset) {
    if (g_hc_count != 0) return true;
    g_hhdm = hhdm_offset;

    for (size_t i = 0; i < 8; i++) {
        pci_function_t f;
        if (!pci_find_class(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, PCI_PROGIF_EHCI, i, &f)) break;
        ehci_release(&f, (unsigned)i);
    }

    for (size_t i = 0; i < 16 && g_hc_count < USB_MAX_HC; i++) {
        pci_function_t f;
        if (!pci_find_class(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, PCI_PROGIF_UHCI, i, &f)) break;
        uhci_hc_t *hc = &g_hc[g_hc_count];
        memset(hc, 0, sizeof(*hc));
        if (!uhci_start(hc, &f)) {
            console_write("[usb] uhci controller failed to start\n");
            continue;
        }
        g_hc_count++;
        console_write("[usb] uhci at ");
        console_write_dec(f.bus);
        console_write(":");
        console_write_dec(f.device);
        console_write(" I/O port ");
        console_write_dec(hc->io);
        console_write("\n");
        for (unsigned n = 0; n < 2; n++) root_port(hc, n);
    }
    return g_hc_count != 0;
}
