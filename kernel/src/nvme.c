/* nvme.c — NVM Express driver (polled, one I/O queue pair).
 *
 * How NVMe works, briefly: the controller's registers (BAR0) hold a few control
 * words and "doorbells". Software builds 64-byte commands in a submission queue in RAM,
 * writes the new tail index to that queue's doorbell, and the controller DMA-reads the
 * command, does the work, and DMA-writes a 16-byte completion into a completion queue.
 * Each completion carries a "phase" bit that flips every time the queue wraps, so
 * software can tell a new entry from an old one without a separate counter.
 * Data buffers are described by PRP entries (physical page pointers): PRP1 and PRP2
 * for up to two pages, otherwise PRP2 points at a page holding a list of the rest.
 *
 * Reference: NVM Express Base Specification, controller registers, queue model,
 * Admin (Identify, Create I/O Submission/Completion Queue) and NVM (Read, Write, Flush)
 * command sets. */
#include "nvme.h"

#include "kstring.h"
#include "panic.h"
#include "pci.h"
#include "pmm.h"
#include "time.h"
#include "vmm.h"

#define NVME_MMIO_VIRT  0xFFFFA00001C00000ULL   /* clear of ahci (...1800000) and lpss (...2000000) */
#define NVME_MMIO_PAGES 4u
#define PTE_PCD         (1ULL << 4)

#define PCI_COMMAND       0x04u
#define PCI_CMD_MEMORY    (1u << 1)
#define PCI_CMD_BUSMASTER (1u << 2)

/* Controller registers */
#define REG_CAP   0x00u     /* 64-bit */
#define REG_CC    0x14u
#define REG_CSTS  0x1Cu
#define REG_AQA   0x24u
#define REG_ASQ   0x28u     /* 64-bit */
#define REG_ACQ   0x30u     /* 64-bit */
#define DOORBELL  0x1000u
#define CC_EN     (1u << 0)
#define CSTS_RDY  (1u << 0)
#define CSTS_CFS  (1u << 1)

/* Admin and NVM opcodes */
#define ADMIN_CREATE_IO_SQ 0x01u
#define ADMIN_CREATE_IO_CQ 0x05u
#define ADMIN_IDENTIFY     0x06u
#define IO_FLUSH           0x00u
#define IO_WRITE           0x01u
#define IO_READ            0x02u

#define QUEUE_ENTRIES 64u
#define DATA_PAGES    16u          /* 64 KiB bounce buffer per controller */
#define PAGE          4096u

typedef struct {
    uint32_t cdw0;                 /* opcode, flags, command id */
    uint32_t nsid;
    uint32_t reserved[2];
    uint64_t mptr;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} nvme_cmd_t;                      /* 64 bytes */

typedef struct {
    uint32_t dw0, dw1, dw2, dw3;   /* dw3: command id, phase (bit 16), status (31:17) */
} nvme_cqe_t;                      /* 16 bytes */

typedef struct {
    nvme_cmd_t *sq;
    volatile nvme_cqe_t *cq;
    uint64_t sq_phys, cq_phys;
    uint16_t tail, head;
    uint8_t phase;
    uint16_t qid;
} nvme_queue_t;

typedef struct {
    volatile uint8_t *regs;
    uint32_t doorbell_stride;      /* bytes between doorbells: 4 << CAP.DSTRD */
    uint32_t timeout_ms;           /* CAP.TO in 500 ms units, converted */
    nvme_queue_t admin, io;
    uint8_t *ident;                /* 4 KiB Identify buffer */
    uint64_t ident_phys;
    uint8_t *data[DATA_PAGES];     /* bounce buffer pages */
    uint64_t data_phys[DATA_PAGES];
    uint64_t *prp_list;
    uint64_t prp_list_phys;
    uint32_t nsid;
    uint16_t next_cid;
    nvme_disk_t disk;
    bool ready;
} nvme_ctrl_t;

static nvme_ctrl_t g_ctrl;
static char g_name[8];

static uint32_t rd32(const nvme_ctrl_t *c, uint32_t off) { return *(volatile uint32_t *)(c->regs + off); }
static void wr32(nvme_ctrl_t *c, uint32_t off, uint32_t v) { *(volatile uint32_t *)(c->regs + off) = v; }
static uint64_t rd64(const nvme_ctrl_t *c, uint32_t off) {
    return (uint64_t)rd32(c, off) | ((uint64_t)rd32(c, off + 4) << 32);
}
static void wr64(nvme_ctrl_t *c, uint32_t off, uint64_t v) {
    wr32(c, off, (uint32_t)v);
    wr32(c, off + 4, (uint32_t)(v >> 32));
}

static uint8_t *dma_frame(uint64_t *phys) {
    uintptr_t p = pmm_alloc_frame();
    if (p == 0) return NULL;
    uint8_t *v = (uint8_t *)(p + pmm_hhdm_offset());
    memset(v, 0, PAGE);
    *phys = p;
    return v;
}

/* Wait for CSTS.RDY to equal `want`, up to the controller's advertised timeout. */
static bool wait_ready(nvme_ctrl_t *c, bool want) {
    uint64_t start = time_uptime_ms();
    uint64_t limit = c->timeout_ms;
    for (uint64_t spin = 0;; spin++) {
        uint32_t sts = rd32(c, REG_CSTS);
        if (((sts & CSTS_RDY) != 0) == want) return true;
        if (sts & CSTS_CFS) return false;               /* controller fatal status */
        if (time_available() ? (time_uptime_ms() - start > limit) : (spin > 50000000u)) return false;
        __asm__ volatile("pause" ::: "memory");
    }
}

static bool queue_alloc(nvme_queue_t *q, uint16_t qid) {
    q->sq = (nvme_cmd_t *)dma_frame(&q->sq_phys);
    q->cq = (volatile nvme_cqe_t *)dma_frame(&q->cq_phys);
    q->tail = q->head = 0;
    q->phase = 1;
    q->qid = qid;
    return q->sq != NULL && q->cq != NULL;
}

static void ring_sq(nvme_ctrl_t *c, nvme_queue_t *q) {
    wr32(c, DOORBELL + (2u * q->qid) * c->doorbell_stride, q->tail);
}
static void ring_cq(nvme_ctrl_t *c, nvme_queue_t *q) {
    wr32(c, DOORBELL + (2u * q->qid + 1u) * c->doorbell_stride, q->head);
}

/* Submit one command and poll for its completion. Returns 0 on success, otherwise the
 * 15-bit status field (never zero on failure), or 0xFFFF if nothing completed in time. */
static uint32_t submit(nvme_ctrl_t *c, nvme_queue_t *q, const nvme_cmd_t *cmd) {
    nvme_cmd_t *slot = &q->sq[q->tail];
    *slot = *cmd;
    uint16_t cid = c->next_cid++;
    slot->cdw0 = (cmd->cdw0 & 0xFFFFu) | ((uint32_t)cid << 16);
    q->tail = (uint16_t)((q->tail + 1u) % QUEUE_ENTRIES);
    __sync_synchronize();
    ring_sq(c, q);
    c->disk.commands++;

    uint64_t start = time_uptime_ms();
    for (uint64_t spin = 0;; spin++) {
        __sync_synchronize();
        volatile nvme_cqe_t *e = &q->cq[q->head];
        uint32_t dw3 = e->dw3;
        if (((dw3 >> 16) & 1u) == q->phase) {
            q->head = (uint16_t)((q->head + 1u) % QUEUE_ENTRIES);
            if (q->head == 0) q->phase ^= 1u;           /* wrapped: the phase bit flips */
            ring_cq(c, q);
            uint32_t status = (dw3 >> 17) & 0x7FFFu;
            if (status != 0) c->disk.errors++;
            return status;
        }
        if (time_available() ? (time_uptime_ms() - start > 5000u) : (spin > 100000000u)) {
            c->disk.errors++;
            return 0xFFFFu;
        }
        __asm__ volatile("pause" ::: "memory");
    }
}

static uint32_t admin_identify(nvme_ctrl_t *c, uint32_t cns, uint32_t nsid) {
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = ADMIN_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = c->ident_phys;
    cmd.cdw10 = cns;
    memset(c->ident, 0, PAGE);
    return submit(c, &c->admin, &cmd);
}

static void copy_field(char *dst, const uint8_t *src, unsigned n) {
    unsigned len = n;
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == 0)) len--;
    for (unsigned i = 0; i < len; i++) dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
    dst[len] = 0;
}

/* Describe `pages` bounce pages to the controller with PRP1/PRP2 (and a list if needed). */
static void set_prps(nvme_ctrl_t *c, nvme_cmd_t *cmd, uint32_t bytes) {
    uint32_t pages = (bytes + PAGE - 1u) / PAGE;
    cmd->prp1 = c->data_phys[0];
    if (pages == 2) {
        cmd->prp2 = c->data_phys[1];
    } else if (pages > 2) {
        for (uint32_t i = 1; i < pages; i++) c->prp_list[i - 1] = c->data_phys[i];
        cmd->prp2 = c->prp_list_phys;
    }
}

static uint32_t io_transfer(nvme_ctrl_t *c, bool write, uint64_t lba, uint32_t blocks) {
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = write ? IO_WRITE : IO_READ;
    cmd.nsid = c->nsid;
    set_prps(c, &cmd, blocks * c->disk.block_size);
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = blocks - 1u;                            /* number of blocks, 0-based */
    return submit(c, &c->io, &cmd);
}

static uint64_t nvme_rw(block_device_t *dev, bool write, uint64_t lba, void *buffer, uint64_t blocks) {
    (void)dev;
    nvme_ctrl_t *c = &g_ctrl;
    if (!c->ready || buffer == NULL || blocks == 0) return 0;
    if (lba >= c->disk.block_count || blocks > c->disk.block_count - lba) return 0;
    uint8_t *p = buffer;
    uint64_t done = 0;
    while (done < blocks) {
        uint32_t n = (blocks - done) < c->disk.max_blocks ? (uint32_t)(blocks - done) : c->disk.max_blocks;
        uint32_t bytes = n * c->disk.block_size;
        if (write) {
            for (uint32_t off = 0; off < bytes; off += PAGE) {
                uint32_t part = bytes - off < PAGE ? bytes - off : PAGE;
                memcpy(c->data[off / PAGE], p + off, part);
            }
        }
        if (io_transfer(c, write, lba + done, n) != 0) return done;
        if (!write) {
            for (uint32_t off = 0; off < bytes; off += PAGE) {
                uint32_t part = bytes - off < PAGE ? bytes - off : PAGE;
                memcpy(p + off, c->data[off / PAGE], part);
            }
        }
        p += bytes;
        done += n;
    }
    return done;
}

static uint64_t nvme_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t blocks) {
    return nvme_rw(dev, false, lba, buf, blocks);
}
static uint64_t nvme_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t blocks) {
    return nvme_rw(dev, true, lba, (void *)(uintptr_t)buf, blocks);
}
static bool nvme_flush(block_device_t *dev) {
    (void)dev;
    nvme_ctrl_t *c = &g_ctrl;
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = IO_FLUSH;
    cmd.nsid = c->nsid;
    return c->ready && submit(c, &c->io, &cmd) == 0;
}

bool nvme_init(void) {
    nvme_ctrl_t *c = &g_ctrl;
    pci_function_t f;
    if (!pci_find_class(0x01, 0x08, 0x02, 0, &f)) return false;

    uint32_t cmdreg = pci_config_read32(f.bus, f.device, f.function, PCI_COMMAND);
    pci_config_write32(f.bus, f.device, f.function, PCI_COMMAND, cmdreg | PCI_CMD_MEMORY | PCI_CMD_BUSMASTER);
    uint32_t bar0 = pci_config_read32(f.bus, f.device, f.function, 0x10);
    uint32_t bar1 = pci_config_read32(f.bus, f.device, f.function, 0x14);
    if ((bar0 & 1u) != 0) {
        kprintf("[nvme] BAR0 is an I/O BAR, not memory\n");
        return false;
    }
    uint64_t phys = (uint64_t)(bar0 & ~0xFu);
    if (((bar0 >> 1) & 3u) == 2u) phys |= (uint64_t)bar1 << 32;      /* 64-bit memory BAR */
    for (uint64_t pg = 0; pg < NVME_MMIO_PAGES; pg++) {
        if (!vmm_map(NVME_MMIO_VIRT + pg * PAGE, phys + pg * PAGE, VMM_PRESENT | VMM_WRITE | VMM_NX | PTE_PCD)) {
            kprintf("[nvme] could not map the controller registers\n");
            return false;
        }
    }
    c->regs = (volatile uint8_t *)(uintptr_t)NVME_MMIO_VIRT;

    uint64_t cap = rd64(c, REG_CAP);
    uint32_t mqes = (uint32_t)(cap & 0xFFFFu);
    c->timeout_ms = (uint32_t)(((cap >> 24) & 0xFFu) + 1u) * 500u;
    c->doorbell_stride = 4u << ((cap >> 32) & 0xFu);
    uint32_t css = (uint32_t)((cap >> 37) & 0xFFu);
    uint32_t mpsmin = (uint32_t)((cap >> 48) & 0xFu);
    if (mqes + 1u < QUEUE_ENTRIES || mpsmin != 0 || (css & 1u) == 0 ||
        DOORBELL + 4u * c->doorbell_stride > NVME_MMIO_PAGES * PAGE) {
        kprintf("[nvme] controller not supported (CAP=%llx)\n", (unsigned long long)cap);
        return false;
    }

    /* Reset into a known state: clear EN and wait for RDY to drop. */
    wr32(c, REG_CC, rd32(c, REG_CC) & ~CC_EN);
    if (!wait_ready(c, false)) { kprintf("[nvme] controller did not stop\n"); return false; }

    if (!queue_alloc(&c->admin, 0) || !queue_alloc(&c->io, 1)) return false;
    c->ident = dma_frame(&c->ident_phys);
    c->prp_list = (uint64_t *)dma_frame(&c->prp_list_phys);
    for (uint32_t i = 0; i < DATA_PAGES; i++) c->data[i] = dma_frame(&c->data_phys[i]);
    if (c->ident == NULL || c->prp_list == NULL || c->data[DATA_PAGES - 1] == NULL) return false;

    wr32(c, REG_AQA, ((QUEUE_ENTRIES - 1u) << 16) | (QUEUE_ENTRIES - 1u));
    wr64(c, REG_ASQ, c->admin.sq_phys);
    wr64(c, REG_ACQ, c->admin.cq_phys);
    /* NVM command set, 4 KiB memory pages, round robin, 64-byte SQ entries (2^6), 16-byte CQ entries (2^4). */
    wr32(c, REG_CC, CC_EN | (6u << 16) | (4u << 20));
    if (!wait_ready(c, true)) { kprintf("[nvme] controller did not become ready\n"); return false; }

    /* Identify the controller (CNS 1). */
    uint32_t st = admin_identify(c, 1, 0);
    if (st != 0) { kprintf("[nvme] Identify Controller failed, status 0x%x\n", st); return false; }
    copy_field(c->disk.serial, c->ident + 4, 20);
    copy_field(c->disk.firmware, c->ident + 64, 8);
    copy_field(c->disk.model, c->ident + 24, 40);
    uint8_t mdts = c->ident[77];                         /* max transfer = 2^MDTS pages; 0 = no limit */
    uint32_t max_pages = DATA_PAGES;
    if (mdts != 0 && (1u << mdts) < max_pages) max_pages = 1u << mdts;

    /* First active namespace (CNS 2), falling back to namespace 1 on older controllers. */
    c->nsid = 1;
    if (admin_identify(c, 2, 0) == 0) {
        uint32_t first = *(uint32_t *)(void *)c->ident;
        if (first != 0) c->nsid = first;
    }
    st = admin_identify(c, 0, c->nsid);
    if (st != 0) { kprintf("[nvme] Identify Namespace %u failed, status 0x%x\n", c->nsid, st); return false; }
    uint64_t nsze = *(uint64_t *)(void *)c->ident;
    uint8_t flbas = c->ident[26];
    uint32_t lbaf = *(uint32_t *)(void *)(c->ident + 128 + 4u * (flbas & 0xFu));
    uint32_t lbads = (lbaf >> 16) & 0xFFu;               /* log2 of the data block size */
    uint32_t metadata = lbaf & 0xFFFFu;
    if (nsze == 0 || lbads < 9 || lbads > 12 || metadata != 0) {
        kprintf("[nvme] namespace %u unsupported (blocks %llu, lbads %u, metadata %u)\n", c->nsid,
                (unsigned long long)nsze, lbads, metadata);
        return false;
    }
    c->disk.block_size = 1u << lbads;
    c->disk.block_count = nsze;
    c->disk.max_blocks = (max_pages * PAGE) / c->disk.block_size;

    /* Create the I/O completion queue first, then the submission queue that feeds it. */
    nvme_cmd_t cq;
    memset(&cq, 0, sizeof cq);
    cq.cdw0 = ADMIN_CREATE_IO_CQ;
    cq.prp1 = c->io.cq_phys;
    cq.cdw10 = ((QUEUE_ENTRIES - 1u) << 16) | 1u;        /* queue size (0-based) and queue id 1 */
    cq.cdw11 = 1u;                                       /* physically contiguous, no interrupts */
    st = submit(c, &c->admin, &cq);
    if (st != 0) { kprintf("[nvme] Create I/O CQ failed, status 0x%x\n", st); return false; }
    nvme_cmd_t sq;
    memset(&sq, 0, sizeof sq);
    sq.cdw0 = ADMIN_CREATE_IO_SQ;
    sq.prp1 = c->io.sq_phys;
    sq.cdw10 = ((QUEUE_ENTRIES - 1u) << 16) | 1u;
    sq.cdw11 = (1u << 16) | 1u;                          /* completes to CQ 1, physically contiguous */
    st = submit(c, &c->admin, &sq);
    if (st != 0) { kprintf("[nvme] Create I/O SQ failed, status 0x%x\n", st); return false; }

    g_name[0] = 'n'; g_name[1] = 'v'; g_name[2] = 'm'; g_name[3] = 'e'; g_name[4] = '0'; g_name[5] = 0;
    c->disk.dev = block_device_register(g_name, c->disk.block_size, c->disk.block_count);
    if (c->disk.dev == NULL) return false;
    c->disk.dev->read = nvme_read;
    c->disk.dev->write = nvme_write;
    c->disk.dev->flush = nvme_flush;
    c->ready = true;
    kprintf("[nvme] %s: %s (serial %s, firmware %s), namespace %u, %llu blocks of %u bytes (%llu MiB)\n",
            g_name, c->disk.model, c->disk.serial, c->disk.firmware, c->nsid,
            (unsigned long long)c->disk.block_count, c->disk.block_size,
            (unsigned long long)(c->disk.block_count * c->disk.block_size / (1024u * 1024u)));
    return true;
}

int nvme_disk_count(void) { return g_ctrl.ready ? 1 : 0; }
const nvme_disk_t *nvme_disk(int index) { return (index == 0 && g_ctrl.ready) ? &g_ctrl.disk : NULL; }
