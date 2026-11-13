/* acpi.c — walk the ACPI root table and record what the firmware provides.
 *
 * Every ACPI table starts with the same 36-byte header: signature (4), length (4),
 * revision (1), checksum (1), then OEM and creator fields. Tables are valid when all
 * of their bytes, summed modulo 256, come to zero. */
#include "acpi.h"

#include <stddef.h>
#include <stdint.h>

#include "kstring.h"

#define RSDP_V1_LEN 20u
#define SDT_HEADER  36u
#define SDT_MAX_LEN (1024u * 1024u)   /* no firmware table we care about is near this */

static uint64_t g_hhdm;
static uint8_t g_revision;
static acpi_table_t g_tables[ACPI_MAX_TABLES];
static unsigned g_count;
static bool g_ready;

static const uint8_t *map(uint64_t phys) {
    return (const uint8_t *)(uintptr_t)(phys + g_hhdm);
}

static bool bytes_sum_to_zero(const uint8_t *p, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void add_table(const uint8_t *hdr, uint64_t phys, bool from_fadt) {
    if (g_count >= ACPI_MAX_TABLES) return;
    acpi_table_t *t = &g_tables[g_count++];
    for (unsigned k = 0; k < 4; k++) t->sig[k] = (char)hdr[k];
    t->sig[4] = '\0';
    t->phys = phys;
    t->length = rd32(hdr + 4);
    t->checksum_ok = t->length >= SDT_HEADER && t->length <= SDT_MAX_LEN
                  && bytes_sum_to_zero(hdr, t->length);
    t->from_fadt = from_fadt;
}

/* The FADT names the DSDT, which holds the device tree (AML). Record it even though it is
 * not listed in the root table. A 64-bit X_DSDT at offset 140 wins over the 32-bit one. */
static void add_dsdt_from_fadt(const uint8_t *fadt, uint32_t fadt_len) {
    uint64_t dsdt = rd32(fadt + 40);
    if (fadt_len >= 148u && rd64(fadt + 140) != 0) dsdt = rd64(fadt + 140);
    if (dsdt == 0) return;
    const uint8_t *hdr = map(dsdt);
    if (rd32(hdr + 4) < SDT_HEADER) return;
    add_table(hdr, dsdt, true);
}

bool acpi_init(uint64_t hhdm_offset, uint64_t rsdp) {
    if (g_ready) return true;
    g_hhdm = hhdm_offset;
    /* Limine may hand over the physical address or the higher-half one. */
    uint64_t rsdp_phys = rsdp >= hhdm_offset ? rsdp - hhdm_offset : rsdp;
    const uint8_t *r = map(rsdp_phys);
    static const char sig[] = "RSD PTR ";
    for (unsigned k = 0; k < 8; k++) {
        if (r[k] != (uint8_t)sig[k]) return false;
    }
    if (!bytes_sum_to_zero(r, RSDP_V1_LEN)) return false;

    g_revision = r[15];
    uint64_t root_phys;
    bool xsdt = g_revision >= 2 && rd64(r + 24) != 0;
    if (xsdt) {
        uint32_t len = rd32(r + 20);
        if (len < 36u || len > SDT_MAX_LEN || !bytes_sum_to_zero(r, len)) return false;
        root_phys = rd64(r + 24);
    } else {
        root_phys = rd32(r + 16);
    }

    const uint8_t *root = map(root_phys);
    uint32_t root_len = rd32(root + 4);
    if (root_len < SDT_HEADER || root_len > SDT_MAX_LEN) return false;
    unsigned entry = xsdt ? 8u : 4u;
    uint32_t entries = (root_len - SDT_HEADER) / entry;
    g_count = 0;
    add_table(root, root_phys, false);
    for (uint32_t i = 0; i < entries; i++) {
        uint64_t phys = xsdt ? rd64(root + SDT_HEADER + i * 8u) : rd32(root + SDT_HEADER + i * 4u);
        if (phys == 0) continue;
        const uint8_t *hdr = map(phys);
        uint32_t len = rd32(hdr + 4);
        if (len < SDT_HEADER || len > SDT_MAX_LEN) continue;
        add_table(hdr, phys, false);
        if (hdr[0] == 'F' && hdr[1] == 'A' && hdr[2] == 'C' && hdr[3] == 'P') {
            add_dsdt_from_fadt(hdr, len);
        }
    }
    g_ready = true;
    return true;
}

unsigned acpi_table_count(void) {
    return g_count;
}

bool acpi_table_at(unsigned i, acpi_table_t *out) {
    if (i >= g_count) return false;
    *out = g_tables[i];
    return true;
}

uint8_t acpi_revision(void) {
    return g_revision;
}

const void *acpi_find(const char sig[4]) {
    for (unsigned i = 0; i < g_count; i++) {
        const acpi_table_t *t = &g_tables[i];
        if (!t->checksum_ok) continue;
        if (t->sig[0] == sig[0] && t->sig[1] == sig[1] && t->sig[2] == sig[2] && t->sig[3] == sig[3]) {
            return map(t->phys);
        }
    }
    return NULL;
}
