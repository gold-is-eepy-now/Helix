/* acpi.h — ACPI table discovery (stage 1 of the Apple SPI work). See docs/16-acpi.md.
 *
 * Limine gives us the RSDP. From it we walk the root table (XSDT, or RSDT on ACPI 1.0)
 * and record every table's signature, length and checksum. We do not interpret AML yet. */
#ifndef HELIX_ACPI_H
#define HELIX_ACPI_H

#include <stdbool.h>
#include <stdint.h>

#define ACPI_MAX_TABLES 32u

typedef struct {
    char sig[5];          /* four characters and a NUL */
    uint64_t phys;        /* physical address of the table header */
    uint32_t length;      /* length from the table header */
    bool checksum_ok;     /* the bytes of the table sum to zero */
    bool from_fadt;       /* the DSDT, found through the FADT rather than the root table */
} acpi_table_t;

/* rsdp is the address Limine gave us: a higher-half virtual address, or a physical one.
 * Returns false when the RSDP is missing or its checksum fails. */
bool acpi_init(uint64_t hhdm_offset, uint64_t rsdp);

unsigned acpi_table_count(void);
bool acpi_table_at(unsigned i, acpi_table_t *out);

/* ACPI revision from the RSDP: 0 is ACPI 1.0, 2 and later have an XSDT. 0 before init. */
uint8_t acpi_revision(void);

/* Mapped pointer to the table at index i, or NULL. */
const void *acpi_table_data(unsigned index);

/* Higher-half direct-map offset (physical + offset is a usable address). */
uint64_t acpi_hhdm(void);

/* Pointer to the first table with this four-character signature, or NULL. */
const void *acpi_find(const char sig[4]);

/* The fixed ACPI hardware registers from the FADT, as I/O ports. Only I/O-port blocks are
 * recorded; a block in memory space is left out. See docs/16-acpi.md. */
typedef struct {
    uint16_t sci_irq;     /* FADT SCI_INT: the interrupt line the firmware raises for events */
    uint16_t pm1a_evt;    /* PM1a event block: status at this port, enable at +pm1_evt_len/2 */
    uint8_t pm1_evt_len;
    uint16_t pm1a_cnt;    /* PM1a control block; bit 0 (SCI_EN) is set when ACPI mode is on */
    uint16_t gpe0;        /* GPE0 block: status bytes here, enable bytes at +gpe0_len/2 */
    uint8_t gpe0_len;     /* total block length in bytes (status plus enable) */
} acpi_pm_t;

/* False until the FADT has been read, or when it names no usable PM blocks. */
bool acpi_get_pm(acpi_pm_t *out);

#endif /* HELIX_ACPI_H */
