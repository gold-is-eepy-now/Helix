/* Access to an optional Doom IWAD supplied as a Limine module. */
#ifndef HELIX_DOOM_WAD_H
#define HELIX_DOOM_WAD_H

#include <stddef.h>

void doom_wad_set(const void *data, size_t size);
const void *doom_wad_data(void);
size_t doom_wad_size(void);

#endif
