#include "doom_wad.h"

static const void *g_doom_wad;
static size_t g_doom_wad_size;

void doom_wad_set(const void *data, size_t size) {
    g_doom_wad = data;
    g_doom_wad_size = size;
}

const void *doom_wad_data(void) { return g_doom_wad; }
size_t doom_wad_size(void) { return g_doom_wad_size; }
