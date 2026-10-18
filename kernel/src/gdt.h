/* gdt.h — Helix-owned long-mode segment descriptors and privilege stack. */
#ifndef HELIX_GDT_H
#define HELIX_GDT_H

#include <stdbool.h>
#include <stdint.h>

#define GDT_KERNEL_CODE_SELECTOR 0x08u
#define GDT_KERNEL_DATA_SELECTOR 0x10u
#define GDT_USER_DATA_SELECTOR   0x18u
#define GDT_USER_CODE_SELECTOR   0x20u
#define GDT_TSS_SELECTOR         0x28u
#define GDT_USER_RPL             3u

bool gdt_init(uint64_t initial_rsp0);
bool gdt_set_rsp0(uint64_t rsp0);

#endif /* HELIX_GDT_H */
