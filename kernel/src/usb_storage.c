/* usb_storage.c — USB mass storage over Bulk-Only Transport, read-only.
 *
 * A command is three phases (BOT spec section 5): a 31-byte Command Block Wrapper
 * (CBW) on the bulk OUT endpoint carrying a SCSI command block, an optional data
 * phase, and a 13-byte Command Status Wrapper (CSW) on bulk IN with a pass/fail
 * status and the number of bytes not transferred.
 *
 * Error handling follows the spec's recovery rules: a stalled data phase has its
 * endpoint's halt cleared and the CSW is still read; a stalled or invalid CSW, or a
 * phase error, is answered with Bulk-Only Mass Storage Reset and clearing both halts.
 * A command that fails with CHECK CONDITION gets REQUEST SENSE, because a freshly
 * attached disk usually reports "unit attention" or "becoming ready" first.
 */
#include "usb_storage.h"

#include "block_device.h"
#include "console.h"
#include "panic.h"
#include "kstring.h"
#include "time.h"
#include "usb_host.h"

#define MAX_DISKS 4

#define CBW_SIG 0x43425355u   /* "USBC" */
#define CSW_SIG 0x53425355u   /* "USBS" */
#define CBW_LEN 31u
#define CSW_LEN 13u

#define CSW_PASSED 0
#define CSW_FAILED 1
#define CSW_PHASE  2

static usb_disk_t g_disks[MAX_DISKS];
static unsigned g_host_index[MAX_DISKS];   /* index in the USB host's device table */
static char g_names[MAX_DISKS][8];
static int g_disk_count;
static uint32_t g_tag = 0x48454C58u;       /* arbitrary start; each CBW gets a new tag */

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Run one BOT command. Returns CSW_PASSED / CSW_FAILED (command failed: ask for sense),
 * or -1 for a transport failure that has already been recovered from with a reset. */
static int bot_command(usb_disk_t *disk, unsigned host, const uint8_t *cb, uint8_t cb_len,
                       bool data_in, void *data, uint32_t data_len, uint32_t *transferred) {
    uint8_t cbw[CBW_LEN];
    memset(cbw, 0, sizeof cbw);
    uint32_t tag = ++g_tag;
    put_le32(cbw + 0, CBW_SIG);
    put_le32(cbw + 4, tag);
    put_le32(cbw + 8, data_len);
    cbw[12] = data_in ? 0x80u : 0x00u;
    cbw[13] = 0;                                  /* LUN 0 */
    cbw[14] = cb_len;
    memcpy(cbw + 15, cb, cb_len);
    disk->commands++;
    if (transferred != NULL) *transferred = 0;

    uint32_t done = 0;
    if (usb_host_bulk(host, false, cbw, CBW_LEN, &done) != 0 || done != CBW_LEN) goto reset;

    if (data_len != 0) {
        int r = usb_host_bulk(host, data_in, data, data_len, &done);
        if (r == -2) {
            (void)usb_host_clear_halt(host, data_in);   /* spec: clear the halt, then read the CSW */
        } else if (r != 0) {
            goto reset;
        } else if (transferred != NULL) {
            *transferred = done;
        }
    }

    uint8_t csw[CSW_LEN];
    int r = usb_host_bulk(host, true, csw, CSW_LEN, &done);
    if (r == -2) {                                /* one retry after clearing a stalled CSW */
        (void)usb_host_clear_halt(host, true);
        r = usb_host_bulk(host, true, csw, CSW_LEN, &done);
    }
    if (r != 0 || done != CSW_LEN) goto reset;
    if (get_le32(csw) != CSW_SIG || get_le32(csw + 4) != tag) goto reset;   /* not our CSW */
    if (csw[12] == CSW_PHASE) goto reset;
    if (csw[12] == CSW_PASSED) return CSW_PASSED;
    return CSW_FAILED;

reset:
    disk->errors++;
    disk->resets++;
    (void)usb_host_msc_reset(host);
    return -1;
}

/* REQUEST SENSE: returns the sense key (0 none, 2 not ready, 6 unit attention, ...) or -1. */
static int request_sense(usb_disk_t *disk, unsigned host) {
    uint8_t cb[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint8_t sense[18];
    memset(sense, 0, sizeof sense);
    if (bot_command(disk, host, cb, 6, true, sense, sizeof sense, NULL) != CSW_PASSED) return -1;
    return sense[2] & 0x0F;
}

static bool test_unit_ready(usb_disk_t *disk, unsigned host) {
    for (unsigned attempt = 0; attempt < 8u; attempt++) {
        uint8_t cb[6] = { 0x00, 0, 0, 0, 0, 0 };
        int r = bot_command(disk, host, cb, 6, true, NULL, 0, NULL);
        if (r == CSW_PASSED) return true;
        if (r == CSW_FAILED) (void)request_sense(disk, host);   /* clears unit attention */
        uint64_t until = time_uptime_ms() + 100u;
        while (time_uptime_ms() < until) __asm__ volatile("pause" ::: "memory");
    }
    return false;
}

static void copy_trimmed(char *dst, const uint8_t *src, unsigned n) {
    unsigned len = n;
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == 0)) len--;
    for (unsigned i = 0; i < len; i++) dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
    dst[len] = 0;
}

/* Block device read: READ(10), in chunks that fit one bulk transfer. */
static usb_disk_t *disk_for(block_device_t *dev) {
    for (int i = 0; i < g_disk_count; i++)
        if (g_disks[i].dev == dev) return &g_disks[i];
    return NULL;
}

static uint64_t usb_read(block_device_t *dev, uint64_t lba, void *buffer, uint64_t blocks) {
    usb_disk_t *disk = disk_for(dev);
    if (disk == NULL || buffer == NULL || blocks == 0) return 0;
    unsigned host = g_host_index[disk - g_disks];
    if (lba >= disk->block_count || blocks > disk->block_count - lba) return 0;
    uint32_t per = usb_host_bulk_max(host) / disk->block_size;
    if (per == 0) return 0;
    if (per > 0xFFFFu) per = 0xFFFFu;
    uint8_t *out = buffer;
    uint64_t total = 0;
    while (total < blocks) {
        uint32_t n = (blocks - total) < per ? (uint32_t)(blocks - total) : per;
        uint64_t at = lba + total;
        uint8_t cb[10] = { 0x28, 0, 0, 0, 0, 0, 0, (uint8_t)(n >> 8), (uint8_t)n, 0 };
        put_be32(cb + 2, (uint32_t)at);
        uint32_t got = 0;
        int r = -1;
        for (unsigned attempt = 0; attempt < 2u; attempt++) {
            r = bot_command(disk, host, cb, 10, true, out, n * disk->block_size, &got);
            if (r == CSW_PASSED && got == n * disk->block_size) break;
            if (r == CSW_FAILED) (void)request_sense(disk, host);
            r = -1;
        }
        if (r != CSW_PASSED) return total;
        out += (uint64_t)n * disk->block_size;
        total += n;
    }
    return total;
}

static void probe(unsigned host, const usb_msc_info_t *info) {
    if (g_disk_count >= MAX_DISKS) return;
    usb_disk_t *disk = &g_disks[g_disk_count];
    memset(disk, 0, sizeof *disk);

    /* Start from a known state: a reset also clears any halt left by the boot firmware. */
    (void)usb_host_msc_reset(host);

    uint8_t cb6[6] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t inq[36];
    memset(inq, 0, sizeof inq);
    if (bot_command(disk, host, cb6, 6, true, inq, sizeof inq, NULL) != CSW_PASSED) {
        kprintf("[usb-storage] %04x:%04x: INQUIRY failed\n", info->vid, info->pid);
        return;
    }
    uint8_t type = inq[0] & 0x1F;
    copy_trimmed(disk->vendor, inq + 8, 8);
    copy_trimmed(disk->product, inq + 16, 16);
    if (type != 0x00 && type != 0x07 && type != 0x0E) {          /* disk, optical memory, RBC */
        kprintf("[usb-storage] %04x:%04x: SCSI device type 0x%02x is not a disk, skipped\n",
                info->vid, info->pid, type);
        return;
    }
    if (!test_unit_ready(disk, host)) {
        kprintf("[usb-storage] %04x:%04x: unit never became ready (no medium?)\n", info->vid, info->pid);
        return;
    }
    uint8_t cb10[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t cap[8];
    if (bot_command(disk, host, cb10, 10, true, cap, sizeof cap, NULL) != CSW_PASSED) {
        kprintf("[usb-storage] %04x:%04x: READ CAPACITY failed\n", info->vid, info->pid);
        return;
    }
    uint32_t last = get_be32(cap), bsize = get_be32(cap + 4);
    if (last == 0xFFFFFFFFu) {
        kprintf("[usb-storage] %04x:%04x: larger than 2 TiB needs READ CAPACITY(16), not implemented\n",
                info->vid, info->pid);
        return;
    }
    if (bsize < 512 || bsize > 4096 || (bsize & (bsize - 1)) != 0 || usb_host_bulk_max(host) < bsize) {
        kprintf("[usb-storage] %04x:%04x: unsupported block size %u\n", info->vid, info->pid, bsize);
        return;
    }
    disk->block_size = bsize;
    disk->block_count = (uint64_t)last + 1u;

    int n = g_disk_count;
    g_names[n][0] = 'u'; g_names[n][1] = 's'; g_names[n][2] = 'b';
    g_names[n][3] = (char)('0' + n); g_names[n][4] = 0;
    disk->dev = block_device_register(g_names[n], bsize, disk->block_count);
    if (disk->dev == NULL) return;
    disk->dev->read = usb_read;
    g_host_index[n] = host;
    g_disk_count++;
    kprintf("[usb-storage] %s: %s %s, %llu blocks of %u bytes (%llu MiB)\n", g_names[n], disk->vendor,
            disk->product, (unsigned long long)disk->block_count, bsize,
            (unsigned long long)(disk->block_count * bsize / (1024u * 1024u)));
}

void usb_storage_init(void) {
    unsigned n = usb_host_device_count();
    for (unsigned i = 0; i < n; i++) {
        usb_msc_info_t info;
        if (usb_host_msc_info(i, &info)) probe(i, &info);
    }
}

int usb_storage_disk_count(void) { return g_disk_count; }

const usb_disk_t *usb_storage_disk(int i) {
    return (i >= 0 && i < g_disk_count) ? &g_disks[i] : NULL;
}
