/* ahci.c — AHCI (SATA) host controller, stage 1: discover, identify, read.
 *
 * AHCI is the standard SATA interface: one PCI function (class 01/06/01) whose BAR5
 * (ABAR) holds the host registers, then one register block per port. Commands are
 * described in memory: a command header points at a command table, which holds the
 * FIS (the ATA command) and a physical-region table (PRDT) for the data. The device
 * reads those structures with DMA, so every one lives in a frame with a physical
 * address. There are no interrupts here: completion is polled.
 *
 * Reference: Serial ATA AHCI 1.3.1 (sections 3 and 5). */

#include "ahci.h"

#include <stddef.h>

#include "kstring.h"
#include "panic.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"

/* PCI */
#define PCI_COMMAND        0x04u
#define PCI_CMD_MEMORY     (1u << 1)
#define PCI_CMD_BUSMASTER  (1u << 2)
#define PCI_BAR5           0x24u

/* Where the host registers are mapped. Chosen clear of e1000 (...1000000) and xHCI
 * (...3000000). */
#define AHCI_MMIO_VIRT     0xFFFFA00001800000ULL
#define AHCI_MMIO_PAGES    6u      /* 0x1100 for ports 0..31 fits in 6 pages */
#define PTE_PCD            (1ULL << 4)   /* page-level cache disable: device registers */

/* HBA (global) registers */
#define HBA_CAP   0x00u
#define HBA_GHC   0x04u
#define HBA_PI    0x0Cu
#define GHC_AE    (1u << 31)       /* AHCI enable: use the AHCI interface, not legacy */

/* Port registers, at 0x100 + 0x80 * port */
#define PORT_BASE   0x100u
#define PORT_STRIDE 0x80u
#define PX_CLB   0x00u
#define PX_CLBU  0x04u
#define PX_FB    0x08u
#define PX_FBU   0x0Cu
#define PX_IS    0x10u
#define PX_CMD   0x18u
#define PX_TFD   0x20u
#define PX_SIG   0x24u
#define PX_SSTS  0x28u
#define PX_SERR  0x30u
#define PX_CI    0x38u
#define PX_CMD_ST   (1u << 0)     /* start the command list */
#define PX_CMD_FRE  (1u << 4)     /* receive FIS enable */
#define PX_CMD_FR   (1u << 14)    /* FIS receive running */
#define PX_CMD_CR   (1u << 15)    /* command list running */
#define PX_IS_TFES  (1u << 30)    /* task file error */
#define TFD_BSY_DRQ 0x88u         /* BSY (0x80) and DRQ (0x08) in the status byte */
#define TFD_ERR     0x01u

#define SIG_SATA_DISK 0x00000101u

/* ATA commands */
#define ATA_IDENTIFY_DEVICE 0xECu
#define ATA_READ_DMA_EXT    0x25u

#define SECTOR_SIZE   512u
#define MAX_SECTORS_PER_CMD 8u    /* one 4 KiB data frame per port */
#define SPIN_LIMIT    1000000u

/* Command header: 32 bytes, one per slot. Only slot 0 is used. */
typedef struct {
    volatile uint32_t flags;      /* FIS length in dwords (bits 4:0), PRDT entries (31:16) */
    volatile uint32_t prd_bytes;  /* bytes transferred, written by the device */
    volatile uint32_t ctba;       /* command table, physical, 128-byte aligned */
    volatile uint32_t ctbau;
    volatile uint32_t reserved[4];
} ahci_cmd_header_t;

/* A physical-region descriptor: one contiguous piece of the data buffer. */
typedef struct {
    volatile uint32_t dba;        /* data base address, physical */
    volatile uint32_t dbau;
    volatile uint32_t reserved;
    volatile uint32_t dbc;        /* byte count - 1, bits 21:0 */
} ahci_prdt_t;

/* One port: its frames, its registers, and the disk on it. */
typedef struct {
    volatile uint8_t *regs;       /* this port's register block */
    uint8_t *page;                /* DMA frame: command list, FIS, command table, identify */
    uint64_t page_phys;
    uint8_t *data;                /* DMA frame for read data */
    uint64_t data_phys;
    bool has_disk;
    ahci_disk_t disk;
} ahci_port_t;

/* Layout of the DMA page (offsets into page). */
#define OFF_CLB      0x000u       /* 1 KiB command list */
#define OFF_FIS      0x400u       /* 256-byte received FIS area */
#define OFF_CTABLE   0x500u       /* command table: 0x80 FIS area, then the PRDT at 0x80 */
#define OFF_IDENT    0x600u       /* 512-byte IDENTIFY DEVICE result */
#define OFF_PRDT     0x580u       /* OFF_CTABLE + 0x80 */

static volatile uint8_t *g_hba;
static ahci_port_t g_ports[32];
static int g_port_count;
static int g_disk_count;
static ahci_disk_t g_disks[AHCI_MAX_DISKS];

static uint32_t hba_read(uint32_t off) { return *(volatile uint32_t *)(g_hba + off); }
static void hba_write(uint32_t off, uint32_t value) { *(volatile uint32_t *)(g_hba + off) = value; }
static uint32_t px_read(ahci_port_t *p, uint32_t off) { return *(volatile uint32_t *)(p->regs + off); }
static void px_write(ahci_port_t *p, uint32_t off, uint32_t value) {
    *(volatile uint32_t *)(p->regs + off) = value;
}

/* Allocate one zeroed frame; returns its virtual address and physical address. */
static uint8_t *dma_frame(uint64_t *phys_out) {
    uintptr_t phys = pmm_alloc_frame();
    if (phys == 0) return NULL;
    uint8_t *virt = (uint8_t *)(uintptr_t)(phys + pmm_hhdm_offset());
    memset(virt, 0, 4096);
    *phys_out = (uint64_t)phys;
    return virt;
}

/* Stop the port's command engine and wait until the hardware reports it idle. */
static bool port_stop(ahci_port_t *p) {
    uint32_t cmd = px_read(p, PX_CMD);
    cmd &= ~(PX_CMD_ST | PX_CMD_FRE);
    px_write(p, PX_CMD, cmd);
    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
        if ((px_read(p, PX_CMD) & (PX_CMD_CR | PX_CMD_FR)) == 0) return true;
    }
    return false;
}

static void port_start(ahci_port_t *p) {
    for (uint32_t i = 0; i < SPIN_LIMIT && (px_read(p, PX_CMD) & PX_CMD_CR); i++) {}
    px_write(p, PX_CMD, px_read(p, PX_CMD) | PX_CMD_FRE | PX_CMD_ST);
}

/* Fill in a host-to-device register FIS (the ATA command) at fis. */
static void fis_h2d(volatile uint8_t *fis, uint8_t command, uint64_t lba, uint16_t count) {
    for (uint32_t i = 0; i < 20; i++) fis[i] = 0;
    fis[0] = 0x27;                       /* FIS type: host to device */
    fis[1] = 0x80;                       /* bit 7: this FIS carries a command */
    fis[2] = command;
    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 0x40;                       /* LBA addressing */
    fis[8] = (uint8_t)(lba >> 24);
    fis[9] = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[12] = (uint8_t)count;
    fis[13] = (uint8_t)(count >> 8);
}

/* Run the command in slot 0 and wait for it. prdt_entries is how many PRDT entries are set.
 * Returns true when the device finished without a task-file error. */
static bool port_exec(ahci_port_t *p, uint32_t prdt_entries) {
    volatile ahci_cmd_header_t *hdr = (volatile ahci_cmd_header_t *)(p->page + OFF_CLB);
    hdr->flags = 5u | (prdt_entries << 16);      /* FIS is 5 dwords; a read, so no write bit */
    hdr->prd_bytes = 0;
    hdr->ctba = (uint32_t)(p->page_phys + OFF_CTABLE);
    hdr->ctbau = (uint32_t)((p->page_phys + OFF_CTABLE) >> 32);

    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {  /* wait for the port to be free */
        if ((px_read(p, PX_CI) & 1u) == 0 && (px_read(p, PX_TFD) & TFD_BSY_DRQ) == 0) break;
    }
    px_write(p, PX_IS, 0xFFFFFFFFu);
    px_write(p, PX_CI, 1u);                      /* issue slot 0 */
    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
        if ((px_read(p, PX_CI) & 1u) == 0) break;
        if (px_read(p, PX_IS) & PX_IS_TFES) break;
    }
    if (px_read(p, PX_IS) & PX_IS_TFES) return false;
    if (px_read(p, PX_CI) & 1u) return false;    /* timed out */
    return (px_read(p, PX_TFD) & TFD_ERR) == 0;
}

/* Put one PRDT entry for a physically contiguous buffer. */
static void prdt_set(ahci_port_t *p, uint64_t phys, uint32_t bytes) {
    volatile ahci_prdt_t *e = (volatile ahci_prdt_t *)(p->page + OFF_PRDT);
    e->dba = (uint32_t)phys;
    e->dbau = (uint32_t)(phys >> 32);
    e->reserved = 0;
    e->dbc = (bytes - 1u) & 0x3FFFFFu;
}

/* ATA IDENTIFY DEVICE: the model and the capacity. Fills p->disk. */
static bool port_identify(ahci_port_t *p) {
    fis_h2d(p->page + OFF_CTABLE, ATA_IDENTIFY_DEVICE, 0, 0);
    prdt_set(p, p->page_phys + OFF_IDENT, 512);
    if (!port_exec(p, 1)) return false;

    volatile uint16_t *w = (volatile uint16_t *)(p->page + OFF_IDENT);
    /* Model: words 27..46, two ASCII characters per word, high byte first. */
    uint32_t n = 0;
    for (uint32_t i = 27; i <= 46; i++) {
        uint16_t word = w[i];
        p->disk.model[n++] = (char)(word >> 8);
        p->disk.model[n++] = (char)(word & 0xFF);
    }
    p->disk.model[n] = '\0';
    while (n > 0 && p->disk.model[n - 1] == ' ') p->disk.model[--n] = '\0';

    /* Capacity: 48-bit sector count (words 100..103) when the drive has it, else 28-bit (60..61). */
    uint64_t sectors = 0;
    if (w[83] & (1u << 10)) {
        sectors = (uint64_t)w[100] | ((uint64_t)w[101] << 16) |
                  ((uint64_t)w[102] << 32) | ((uint64_t)w[103] << 48);
    } else {
        sectors = (uint64_t)w[60] | ((uint64_t)w[61] << 16);
    }
    p->disk.sectors = sectors;
    return sectors > 0;
}

/* Read up to MAX_SECTORS_PER_CMD sectors at lba into p->data. */
static bool port_read(ahci_port_t *p, uint64_t lba, uint32_t count) {
    fis_h2d(p->page + OFF_CTABLE, ATA_READ_DMA_EXT, lba, (uint16_t)count);
    prdt_set(p, p->data_phys, count * SECTOR_SIZE);
    return port_exec(p, 1);
}

/* Bring one port up: if a SATA disk is attached, give the port its DMA structures,
 * start it and identify the disk. Returns true when a disk was found. */
static bool port_init(ahci_port_t *p, uint32_t index) {
    p->regs = g_hba + PORT_BASE + index * PORT_STRIDE;
    uint32_t ssts = px_read(p, PX_SSTS);
    /* DET 3 = device present and communicating; SIG identifies a SATA disk (not ATAPI). */
    if ((ssts & 0x0F) != 3) return false;
    if (px_read(p, PX_SIG) != SIG_SATA_DISK) return false;

    if (!port_stop(p)) return false;
    p->page = dma_frame(&p->page_phys);
    p->data = dma_frame(&p->data_phys);
    if (p->page == NULL || p->data == NULL) return false;

    px_write(p, PX_CLB, (uint32_t)p->page_phys);
    px_write(p, PX_CLBU, (uint32_t)(p->page_phys >> 32));
    px_write(p, PX_FB, (uint32_t)(p->page_phys + OFF_FIS));
    px_write(p, PX_FBU, (uint32_t)((p->page_phys + OFF_FIS) >> 32));
    px_write(p, PX_SERR, 0xFFFFFFFFu);
    px_write(p, PX_IS, 0xFFFFFFFFu);
    port_start(p);

    if (!port_identify(p)) return false;
    p->has_disk = true;
    return true;
}

/* The block device interface: read from the port that owns dev. */
static ahci_port_t *port_of(block_device_t *dev) {
    for (int i = 0; i < g_port_count; i++) {
        if (g_ports[i].has_disk && g_ports[i].disk.dev == dev) return &g_ports[i];
    }
    return NULL;
}

static uint64_t ahci_read(block_device_t *dev, uint64_t lba, void *buffer, uint64_t blocks) {
    ahci_port_t *p = port_of(dev);
    if (p == NULL || buffer == NULL) return 0;
    uint8_t *out = (uint8_t *)buffer;
    uint64_t done = 0;
    while (done < blocks) {
        uint64_t n = blocks - done;
        if (n > MAX_SECTORS_PER_CMD) n = MAX_SECTORS_PER_CMD;
        if (lba + done + n > p->disk.sectors) break;
        if (!port_read(p, lba + done, (uint32_t)n)) break;
        for (uint64_t i = 0; i < n * SECTOR_SIZE; i++) out[done * SECTOR_SIZE + i] = p->data[i];
        done += n;
    }
    return done;
}

/* Writes are not implemented yet. */
static uint64_t ahci_write(block_device_t *dev, uint64_t lba, const void *buffer, uint64_t blocks) {
    (void)dev; (void)lba; (void)buffer; (void)blocks;
    return 0;
}

bool ahci_init(void) {
    pci_function_t f;
    if (!pci_find_class(0x01, 0x06, 0x01, 0, &f)) return false;

    uint32_t cmd = pci_config_read32(f.bus, f.device, f.function, PCI_COMMAND);
    pci_config_write32(f.bus, f.device, f.function, PCI_COMMAND, cmd | PCI_CMD_MEMORY | PCI_CMD_BUSMASTER);

    uint32_t bar = pci_config_read32(f.bus, f.device, f.function, PCI_BAR5);
    if ((bar & 1u) != 0 || ((bar >> 1) & 3u) != 0) {
        kprintf("[ahci] ABAR is not a 32-bit memory BAR\n");
        return false;
    }
    uint64_t phys = (uint64_t)(bar & ~0xFu);
    for (uint64_t page = 0; page < AHCI_MMIO_PAGES; page++) {
        if (!vmm_map(AHCI_MMIO_VIRT + page * 4096u, phys + page * 4096u,
                     VMM_PRESENT | VMM_WRITE | VMM_NX | PTE_PCD)) {
            kprintf("[ahci] could not map the host registers\n");
            return false;
        }
    }
    g_hba = (volatile uint8_t *)(uintptr_t)AHCI_MMIO_VIRT;
    hba_write(HBA_GHC, hba_read(HBA_GHC) | GHC_AE);

    uint32_t pi = hba_read(HBA_PI);                /* which ports the controller implements */
    uint32_t nports = (hba_read(HBA_CAP) & 0x1Fu) + 1u;
    for (uint32_t i = 0; i < 32 && i < nports; i++) {
        if (((pi >> i) & 1u) == 0) continue;
        ahci_port_t *p = &g_ports[g_port_count];
        g_port_count++;
        if (!port_init(p, i)) continue;
        if (g_disk_count >= AHCI_MAX_DISKS) break;

        static char names[AHCI_MAX_DISKS][8];
        names[g_disk_count][0] = 's';
        names[g_disk_count][1] = 'a';
        names[g_disk_count][2] = 't';
        names[g_disk_count][3] = 'a';
        names[g_disk_count][4] = (char)('0' + g_disk_count);
        names[g_disk_count][5] = '\0';
        p->disk.dev = block_device_register(names[g_disk_count], SECTOR_SIZE, p->disk.sectors);
        if (p->disk.dev == NULL) {
            p->has_disk = false;
            continue;
        }
        p->disk.dev->read = ahci_read;
        p->disk.dev->write = ahci_write;
        g_disks[g_disk_count] = p->disk;
        g_disk_count++;
    }
    return true;
}

int ahci_disk_count(void) { return g_disk_count; }

const ahci_disk_t *ahci_disk(int index) {
    if (index < 0 || index >= g_disk_count) return NULL;
    return &g_disks[index];
}
