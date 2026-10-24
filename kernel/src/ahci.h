/* ahci.h — polled AHCI SATA block-device backend. */
#ifndef HELIX_AHCI_H
#define HELIX_AHCI_H

#include <stdbool.h>
#include <stdint.h>

bool ahci_init(uint64_t hhdm_offset);
bool ahci_disk_ready(void);
uint32_t ahci_sector_size(void);
uint64_t ahci_sector_count(void);
void ahci_print_status(void);
bool ahci_read_sector(uint64_t lba, void *buffer, uint32_t length);
bool ahci_write_sector(uint64_t lba, const void *buffer, uint32_t length);

#endif /* HELIX_AHCI_H */
