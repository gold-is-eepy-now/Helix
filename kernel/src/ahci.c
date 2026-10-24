/* ahci.c — small, polled AHCI driver for one SATA disk.
 *
 * The first version supports a single 512-byte logical-sector disk and one
 * command slot. It uses DMA bounce memory so callers may pass ordinary kernel
 * buffers, and leaves interrupts unused. This is enough for boot images and
 * the initial HFS volume while the block layer remains synchronous.
 */
#include "ahci.h"

#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "driver.h"
#include "io.h"
#include "kstring.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"

#define AHCI_PORT_BASE       0x100u
#define AHCI_PORT_STRIDE     0x80u
#define AHCI_CAP             0x00u
#define AHCI_GHC             0x04u
#define AHCI_PI              0x0cu
#define AHCI_CAP2            0x24u
#define AHCI_GHC_AE          (1u << 31)
#define AHCI_PORT_CLB        0x00u
#define AHCI_PORT_CLBU       0x04u
#define AHCI_PORT_FB         0x08u
#define AHCI_PORT_FBU        0x0cu
#define AHCI_PORT_IS         0x10u
#define AHCI_PORT_CMD        0x18u
#define AHCI_PORT_TFD        0x20u
#define AHCI_PORT_SIG        0x24u
#define AHCI_PORT_SSTS       0x28u
#define AHCI_PORT_SERR       0x30u
#define AHCI_PORT_CI         0x38u
#define AHCI_CMD_ST          (1u << 0)
#define AHCI_CMD_FRE         (1u << 4)
#define AHCI_CMD_FR          (1u << 14)
#define AHCI_CMD_CR          (1u << 15)
#define AHCI_TFD_BSY         (1u << 7)
#define AHCI_TFD_DRQ         (1u << 3)
#define AHCI_TFD_ERR         (1u << 0)
#define AHCI_SSTS_DET_MASK   0x0fu
#define AHCI_SSTS_IPM_MASK   0x0f00u
#define AHCI_SIG_ATA         0x00000101u
#define AHCI_PORT_POLL_LIMIT 200000u
#define AHCI_COMMAND_POLL_LIMIT 30000000u
#define AHCI_SECTOR_SIZE     512u
#define AHCI_PORT_LIMIT      32u

typedef struct {
    uint16_t flags;
    uint16_t prdt_length;
    uint32_t transferred;
    uint32_t table_low;
    uint32_t table_high;
    uint32_t reserved[4];
} __attribute__((packed)) ahci_command_header_t;

typedef struct {
    uint32_t data_low;
    uint32_t data_high;
    uint32_t reserved;
    uint32_t byte_count_interrupt;
} __attribute__((packed)) ahci_prdt_t;

typedef struct {
    uint8_t command_fis[64];
    uint8_t atapi_command[16];
    uint8_t reserved[48];
    ahci_prdt_t prdt[1];
} __attribute__((packed, aligned(128))) ahci_command_table_t;

typedef struct {
    bool ready;
    uint8_t port_number;
    uint64_t mmio;
    uint32_t sector_size;
    uint64_t sector_count;
    uint64_t command_list_phys;
    uint64_t fis_phys;
    uint64_t command_table_phys;
    uint64_t bounce_phys;
    ahci_command_header_t *command_list;
    ahci_command_table_t *command_table;
    uint8_t *bounce;
} ahci_disk_t;

static ahci_disk_t g_disks[1];
static size_t g_disk_count;
static bool g_initialized;
static uint64_t g_ahci_hhdm;
static bool g_ahci_dma64;

static volatile uint32_t *reg32(uint64_t base, uint32_t offset) {
    return (volatile uint32_t *)(uintptr_t)(base + offset);
}

static volatile uint32_t *port_reg(ahci_disk_t *disk, uint32_t offset) {
    return reg32(disk->mmio, AHCI_PORT_BASE +
                 (uint32_t)disk->port_number * AHCI_PORT_STRIDE + offset);
}

static void dma_memory(uint64_t *phys, void **virt) {
    uintptr_t frame = pmm_alloc_frame();
    if (frame == 0 || g_ahci_hhdm == 0) {
        *phys = 0;
        *virt = NULL;
        return;
    }
    if (!g_ahci_dma64 && frame > UINT32_MAX - 4095u) {
        pmm_free_frame(frame);
        *phys = 0;
        *virt = NULL;
        return;
    }
    *phys = frame;
    *virt = (void *)(uintptr_t)(frame + g_ahci_hhdm);
    memset(*virt, 0, 4096);
}

/* Largest transfer per command: 64 sectors = 32 KiB = 8 frames. The bounce
 * buffer must be physically contiguous because it is described by a single
 * PRDT entry. */
#define AHCI_MAX_SECTORS 64u
#define AHCI_BOUNCE_FRAMES ((AHCI_MAX_SECTORS * AHCI_SECTOR_SIZE) / 4096u)

static void dma_memory_contiguous(uint64_t *phys, void **virt) {
    uintptr_t frame = pmm_alloc_frames(AHCI_BOUNCE_FRAMES);
    uintptr_t last = frame + AHCI_BOUNCE_FRAMES * 4096u - 1u;
    if (frame == 0 || g_ahci_hhdm == 0 ||
        (!g_ahci_dma64 && last > UINT32_MAX)) {
        if (frame != 0) pmm_free_frames(frame, AHCI_BOUNCE_FRAMES);
        *phys = 0;
        *virt = NULL;
        return;
    }
    *phys = frame;
    *virt = (void *)(uintptr_t)(frame + g_ahci_hhdm);
    memset(*virt, 0, AHCI_BOUNCE_FRAMES * 4096u);
}

static bool wait_clear(volatile uint32_t *reg, uint32_t mask) {
    for (uint32_t i = 0; i < AHCI_PORT_POLL_LIMIT; i++) {
        if ((*reg & mask) == 0) return true;
        __asm__ volatile("pause");
    }
    return false;
}

static bool wait_set(volatile uint32_t *reg, uint32_t mask) {
    for (uint32_t i = 0; i < AHCI_PORT_POLL_LIMIT; i++) {
        if ((*reg & mask) == mask) return true;
        __asm__ volatile("pause");
    }
    return false;
}

static bool issue_command(ahci_disk_t *disk, uint8_t command,
                          uint64_t lba, bool write, const void *data,
                          uint32_t sectors) {
    bool has_data = command != 0xEAu; /* FLUSH CACHE EXT moves no data. */
    uint32_t bytes = sectors * AHCI_SECTOR_SIZE;
    volatile uint32_t *tfd = port_reg(disk, AHCI_PORT_TFD);
    if (!wait_clear(tfd, AHCI_TFD_BSY | AHCI_TFD_DRQ)) return false;
    if ((*port_reg(disk, AHCI_PORT_CI) & 1u) != 0) return false;

    if (command == 0xECu) memset(disk->bounce, 0, AHCI_SECTOR_SIZE);
    else if (write && has_data) memcpy(disk->bounce, data, bytes);

    ahci_command_header_t *header = &disk->command_list[0];
    memset(header, 0, sizeof(*header));
    header->flags = 5u | (write ? (1u << 6) : 0u);
    header->prdt_length = has_data ? 1u : 0u;
    header->table_low = (uint32_t)disk->command_table_phys;
    header->table_high = (uint32_t)(disk->command_table_phys >> 32);

    ahci_command_table_t *table = disk->command_table;
    memset(table, 0, sizeof(*table));
    if (has_data) {
        table->prdt[0].data_low = (uint32_t)disk->bounce_phys;
        table->prdt[0].data_high = (uint32_t)(disk->bounce_phys >> 32);
        table->prdt[0].byte_count_interrupt = (bytes - 1u) | (1u << 31);
    }
    uint8_t *fis = table->command_fis;
    fis[0] = 0x27u; /* Register Host to Device FIS. */
    fis[1] = 0x80u; /* C bit: this FIS carries an ATA command. */
    fis[2] = command;
    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = command == 0xECu
           ? 0u /* IDENTIFY DEVICE requires Device/LBA bits cleared. */
           : 0x40u | (uint8_t)((lba >> 24) & 0x0fu);
    fis[8] = (uint8_t)(lba >> 32);
    fis[9] = (uint8_t)(lba >> 40);
    fis[10] = (uint8_t)(lba >> 48);
    /* Sector count (16 bits for the EXT commands); IDENTIFY and FLUSH use 0. */
    fis[12] = has_data && command != 0xECu ? (uint8_t)sectors : 0u;
    fis[13] = has_data && command != 0xECu ? (uint8_t)(sectors >> 8) : 0u;

    __sync_synchronize();
    *port_reg(disk, AHCI_PORT_IS) = 0xffffffffu;
    *port_reg(disk, AHCI_PORT_SERR) = 0xffffffffu;
    *port_reg(disk, AHCI_PORT_CI) = 1u;
    volatile uint32_t *ci = port_reg(disk, AHCI_PORT_CI);
    for (uint32_t i = 0; i < AHCI_COMMAND_POLL_LIMIT; i++) {
        if ((*ci & 1u) == 0) {
            __sync_synchronize();
            if ((*tfd & AHCI_TFD_ERR) != 0 ||
                (*port_reg(disk, AHCI_PORT_IS) & (1u << 30)) != 0)
                return false;
            if (has_data && (command == 0xECu || !write))
                memcpy((void *)data, disk->bounce,
                       command == 0xECu ? AHCI_SECTOR_SIZE : bytes);
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

static bool stop_port(ahci_disk_t *disk) {
    volatile uint32_t *cmd = port_reg(disk, AHCI_PORT_CMD);
    /* AHCI requires stopping command processing before stopping FIS receive. */
    console_write("[helix] ahci: stopping port, CMD=");
    console_write_hex(*cmd);
    console_putc('\n');
    *cmd &= ~AHCI_CMD_ST;
    if (!wait_clear(cmd, AHCI_CMD_CR)) {
        console_write("[helix] ahci: CR stayed set, CMD=");
        console_write_hex(*cmd);
        console_putc('\n');
        return false;
    }
    *cmd &= ~AHCI_CMD_FRE;
    if (!wait_clear(cmd, AHCI_CMD_FR)) {
        console_write("[helix] ahci: FR stayed set, CMD=");
        console_write_hex(*cmd);
        console_putc('\n');
        return false;
    }
    console_write("[helix] ahci: port engine stopped\n");
    return true;
}

static bool prepare_port(ahci_disk_t *disk, uint64_t mmio, uint8_t port) {
    disk->mmio = mmio;
    disk->port_number = port;
    if (!stop_port(disk)) {
        return false;
    }

    console_write("[helix] ahci: allocating DMA buffers\n");
    void *memory = NULL;
    dma_memory(&disk->command_list_phys, &memory);
    disk->command_list = memory;
    if (memory == NULL) return false;
    dma_memory(&disk->fis_phys, &memory);
    if (memory == NULL) return false;
    dma_memory(&disk->command_table_phys, &memory);
    disk->command_table = memory;
    if (memory == NULL) return false;
    dma_memory_contiguous(&disk->bounce_phys, &memory);
    disk->bounce = memory;
    if (memory == NULL) return false;

    *port_reg(disk, AHCI_PORT_CLB) = (uint32_t)disk->command_list_phys;
    *port_reg(disk, AHCI_PORT_CLBU) = (uint32_t)(disk->command_list_phys >> 32);
    *port_reg(disk, AHCI_PORT_FB) = (uint32_t)disk->fis_phys;
    *port_reg(disk, AHCI_PORT_FBU) = (uint32_t)(disk->fis_phys >> 32);
    disk->command_list[0].table_low = (uint32_t)disk->command_table_phys;
    disk->command_list[0].table_high = (uint32_t)(disk->command_table_phys >> 32);
    *port_reg(disk, AHCI_PORT_IS) = 0xffffffffu;
    *port_reg(disk, AHCI_PORT_SERR) = 0xffffffffu;
    volatile uint32_t *cmd = port_reg(disk, AHCI_PORT_CMD);
    /* Start FIS reception first, wait for FR, then start command processing. */
    console_write("[helix] ahci: starting FIS receiver\n");
    *cmd |= AHCI_CMD_FRE;
    if (!wait_set(cmd, AHCI_CMD_FR)) {
        console_write("[helix] ahci: FIS receiver did not start\n");
        return false;
    }
    console_write("[helix] ahci: starting command engine\n");
    *cmd |= AHCI_CMD_ST;
    if (!wait_set(cmd, AHCI_CMD_CR)) {
        console_write("[helix] ahci: command engine did not start\n");
        return false;
    }

    uint16_t identify[256];
    console_write("[helix] ahci: issuing IDENTIFY\n");
    if (!issue_command(disk, 0xECu, 0, false, identify, 1u)) {
        console_write("[helix] ahci: IDENTIFY failed, TFD=");
        console_write_hex(*port_reg(disk, AHCI_PORT_TFD));
        console_write(" IS=");
        console_write_hex(*port_reg(disk, AHCI_PORT_IS));
        console_write(" CI=");
        console_write_hex(*port_reg(disk, AHCI_PORT_CI));
        console_write(" CMD=");
        console_write_hex(*port_reg(disk, AHCI_PORT_CMD));
        console_putc('\n');
        return false;
    }
    uint64_t sectors = (uint64_t)identify[100] |
        ((uint64_t)identify[101] << 16) | ((uint64_t)identify[102] << 32) |
        ((uint64_t)identify[103] << 48);
    if (sectors == 0 || (identify[83] & (1u << 10)) == 0) {
        sectors = (uint64_t)identify[60] | ((uint64_t)identify[61] << 16);
    }
    if (sectors == 0) return false;
    if ((identify[106] & 0xc000u) == 0x4000u &&
        (identify[106] & (1u << 12)) != 0) {
        uint32_t logical_words = (uint32_t)identify[117] |
            ((uint32_t)identify[118] << 16);
        if (logical_words != AHCI_SECTOR_SIZE / 2u) return false;
    }
    disk->sector_size = AHCI_SECTOR_SIZE;
    disk->sector_count = sectors;
    disk->ready = true;
    return true;
}

static bool disk_start(void *context) {
    return context != NULL && ((ahci_disk_t *)context)->ready;
}

/* length must be a whole number of sectors, at most AHCI_MAX_SECTORS. */
static bool transfer_ok(ahci_disk_t *disk, uint64_t lba, const void *buffer,
                        size_t length, uint32_t *sectors) {
    if (disk == NULL || !disk->ready || buffer == NULL || length == 0 ||
        length % AHCI_SECTOR_SIZE != 0 ||
        length > AHCI_MAX_SECTORS * AHCI_SECTOR_SIZE) return false;
    *sectors = (uint32_t)(length / AHCI_SECTOR_SIZE);
    return lba < disk->sector_count && *sectors <= disk->sector_count - lba;
}

static bool disk_read(void *context, uint64_t lba, void *buffer, size_t length) {
    ahci_disk_t *disk = context;
    uint32_t sectors;
    if (!transfer_ok(disk, lba, buffer, length, &sectors)) return false;
    return issue_command(disk, 0x25u, lba, false, buffer, sectors);
}

static bool disk_write(void *context, uint64_t lba, const void *buffer,
                       size_t length) {
    ahci_disk_t *disk = context;
    uint32_t sectors;
    if (!transfer_ok(disk, lba, buffer, length, &sectors)) return false;
    return issue_command(disk, 0x35u, lba, true, buffer, sectors);
}

/* FLUSH CACHE EXT: make the drive commit its volatile write cache. */
static bool disk_flush(void *context) {
    ahci_disk_t *disk = context;
    return disk != NULL && disk->ready &&
           issue_command(disk, 0xEAu, 0, false, NULL, 0);
}

bool ahci_init(uint64_t hhdm_offset) {
    if (g_initialized) return g_disk_count != 0;
    g_ahci_hhdm = hhdm_offset;
    g_disk_count = 0;
    console_write("[helix] ahci: controllers=");
    console_write_dec(pci_ahci_controller_count());
    console_putc('\n');
    for (size_t i = 0; i < pci_ahci_controller_count(); i++) {
        pci_ahci_controller_t controller;
        if (!pci_ahci_controller_get(i, &controller) ||
            controller.abar_virt == 0) continue;
        uint64_t mmio = controller.abar_virt;
        uint32_t cap = *reg32(mmio, AHCI_CAP);
        uint32_t implemented = *reg32(mmio, AHCI_PI);
        console_write("[helix] ahci: controller ");
        console_write_dec(i);
        console_write(" CAP=");
        console_write_hex(cap);
        console_write(" PI=");
        console_write_hex(implemented);
        console_putc('\n');
        /* 32-bit DMA is common on older SATA controllers. The allocator keeps
         * its buffers below 4 GiB in that case; descriptor upper halves remain
         * zero automatically. */
        g_ahci_dma64 = (cap & (1u << 31)) != 0;
        console_write("[helix] ahci: DMA mode ");
        console_write(g_ahci_dma64 ? "64-bit\n" : "32-bit\n");
        *reg32(mmio, AHCI_GHC) |= AHCI_GHC_AE;
        for (uint8_t port = 0; port < AHCI_PORT_LIMIT; port++) {
            if ((implemented & (1u << port)) == 0) continue;
            volatile uint32_t *p = reg32(mmio, AHCI_PORT_BASE +
                                         (uint32_t)port * AHCI_PORT_STRIDE);
            uint32_t ssts = p[AHCI_PORT_SSTS / 4u];
            console_write("[helix] ahci: port ");
            console_write_dec(port);
            console_write(" SSTS=");
            console_write_hex(ssts);
            console_putc('\n');
            if ((ssts & AHCI_SSTS_DET_MASK) != 3u ||
                (ssts & AHCI_SSTS_IPM_MASK) != 0x100u) continue;
            uint32_t signature = p[AHCI_PORT_SIG / 4u];
            console_write("[helix] ahci: port ");
            console_write_dec(port);
            console_write(" connected, signature=");
            console_write_hex(signature);
            console_putc('\n');
            if (signature != AHCI_SIG_ATA) continue;
            ahci_disk_t *disk = &g_disks[0];
            console_write("[helix] ahci: preparing SATA port\n");
            if (!prepare_port(disk, mmio, port)) {
                console_write("[helix] ahci: SATA port initialization failed\n");
                continue;
            }
            static const char disk_name[] = "sata0";
            driver_descriptor_t descriptor = {
                .name = disk_name,
                .device_class = "block-storage",
                .context = disk,
                .start = disk_start,
                .read = disk_read,
                .write = disk_write,
                .flush = disk_flush,
                .sector_size = disk->sector_size,
                .sector_count = disk->sector_count,
            };
            if (!driver_register(&descriptor)) {
                disk->ready = false;
                continue;
            }
            g_disk_count = 1;
            console_write("[helix] ahci: sata0 ready, ");
            console_write_dec(disk->sector_count);
            console_write(" sectors\n");
            break;
        }
        if (g_disk_count != 0) break;
    }
    g_initialized = true;
    return g_disk_count != 0;
}

bool ahci_disk_ready(void) { return g_disk_count != 0; }
uint32_t ahci_sector_size(void) {
    return g_disk_count != 0 ? g_disks[0].sector_size : 0;
}
uint64_t ahci_sector_count(void) {
    return g_disk_count != 0 ? g_disks[0].sector_count : 0;
}
void ahci_print_status(void) { pci_print_ahci_controllers(); }
bool ahci_read_sector(uint64_t lba, void *buffer, uint32_t length) {
    return disk_read(g_disk_count ? &g_disks[0] : NULL, lba, buffer, length);
}
bool ahci_write_sector(uint64_t lba, const void *buffer, uint32_t length) {
    return disk_write(g_disk_count ? &g_disks[0] : NULL, lba, buffer, length);
}
