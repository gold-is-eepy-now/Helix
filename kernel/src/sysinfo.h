/* sysinfo.h — the `sysinfo` / `neofetch` system information app. */
#ifndef HELIX_SYSINFO_H
#define HELIX_SYSINFO_H

#include <stdint.h>

/* Record which firmware type the bootloader reported (for the Boot line). */
void sysinfo_set_firmware(uint64_t firmware_type);

/* Print the system information panel. */
void sysinfo_print(void);

#endif /* HELIX_SYSINFO_H */
