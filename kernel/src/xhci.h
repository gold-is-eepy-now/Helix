/* xhci.h — minimal polled xHCI bring-up interface. */
#ifndef HELIX_XHCI_H
#define HELIX_XHCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool xhci_init(uint64_t hhdm_offset);
bool xhci_ready(void);
bool xhci_storage_ready(void);
void xhci_print_status(void);
uint32_t xhci_sector_size(void);
uint64_t xhci_sector_count(void);
bool xhci_read_sector(uint64_t lba, void *buffer, size_t length);
bool xhci_flush(void);
bool xhci_write_sector(uint64_t lba, const void *buffer, size_t length);

#endif /* HELIX_XHCI_H */
