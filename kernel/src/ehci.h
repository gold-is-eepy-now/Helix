/* ehci.h — polled EHCI USB 2.0 storage controller. */
#ifndef HELIX_EHCI_H
#define HELIX_EHCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool ehci_init(uint64_t hhdm_offset);
bool ehci_ready(void);
bool ehci_storage_ready(void);
bool ehci_rescan_storage(void);
void ehci_print_status(void);
uint32_t ehci_sector_size(void);
uint64_t ehci_sector_count(void);
bool ehci_read_sector(uint64_t lba, void *buffer, size_t length);
bool ehci_write_sector(uint64_t lba, const void *buffer, size_t length);
bool ehci_flush(void);

#endif
