/* icon_pack.c — see icon_pack.h. The pack is read in place from the boot module, so
 * nothing is copied and nothing is compiled into the kernel. */
#include "icon_pack.h"

#include <stddef.h>
#include <stdint.h>

static const uint8_t *g_pack;
static size_t g_pack_size;

#define PACK_MAX_ICONS 64u
#define PACK_ENTRY_LEN 32u     /* name[24], u32 offset, u32 size */

void icon_pack_set(const void *data, size_t size) {
    g_pack = (const uint8_t *)data;
    g_pack_size = size;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* True when the 24-byte NUL-padded entry name equals name exactly. */
static int name_matches(const uint8_t entry[24], const char *name) {
    for (size_t i = 0; i < 24; i++) {
        char want = name[i];
        if ((char)entry[i] != want) return 0;
        if (want == '\0') return 1;
    }
    return name[24] == '\0';
}

const uint8_t *icon_pack_find(const char *name) {
    if (g_pack == NULL || g_pack_size < 8) return NULL;
    if (g_pack[0] != 'O' || g_pack[1] != 'X' || g_pack[2] != 'I' || g_pack[3] != '1') return NULL;
    uint32_t count = rd32(g_pack + 4);
    if (count > PACK_MAX_ICONS) return NULL;
    if (8u + (size_t)count * PACK_ENTRY_LEN > g_pack_size) return NULL;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = g_pack + 8u + (size_t)i * PACK_ENTRY_LEN;
        if (!name_matches(e, name)) continue;
        uint32_t off = rd32(e + 24);
        uint32_t size = rd32(e + 28);
        /* Refuse anything that is not exactly one icon inside the module. */
        if (size != ICON32_BYTES || off > g_pack_size || size > g_pack_size - off) return NULL;
        return g_pack + off;
    }
    return NULL;
}
