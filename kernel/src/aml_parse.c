/* aml_parse.c — reading AML primitives from a byte cursor: little-endian integers, PkgLength
 * and NameString. The encodings are from ACPI 6.x section 20.2. */
#include "aml_internal.h"
#include "kprintf_compat.h"

uint8_t aml_peek(aml_stream_t *s) {
    if (s->pos >= s->end) {
        s->bad = true;
        return 0;
    }
    return s->base[s->pos];
}

uint8_t aml_u8(aml_stream_t *s) {
    uint8_t v = aml_peek(s);
    if (!s->bad) s->pos++;
    return v;
}

static uint64_t read_le(aml_stream_t *s, unsigned n) {
    if (s->pos + n > s->end) {
        s->bad = true;
        s->pos = s->end;
        return 0;
    }
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) v |= (uint64_t)s->base[s->pos + i] << (8 * i);
    s->pos += n;
    return v;
}

uint16_t aml_u16(aml_stream_t *s) { return (uint16_t)read_le(s, 2); }
uint32_t aml_u32(aml_stream_t *s) { return (uint32_t)read_le(s, 4); }
uint64_t aml_u64(aml_stream_t *s) { return read_le(s, 8); }

/* PkgLength: the top two bits of the lead byte give how many more bytes follow (0-3). With no
 * extra bytes the length is the low six bits; otherwise it is the low four bits of the lead
 * byte followed by the extra bytes, the first of which is shifted left by four. The length
 * counts the PkgLength bytes themselves, so the object ends at start + length. */
uint32_t aml_pkglen(aml_stream_t *s) {
    uint8_t lead = aml_u8(s);
    unsigned extra = lead >> 6;
    uint32_t len = lead & (extra ? 0x0Fu : 0x3Fu);
    for (unsigned i = 0; i < extra; i++) {
        len |= (uint32_t)aml_u8(s) << (4 + 8 * i);
    }
    if (s->bad) return 0;
    /* No end check here: a field element's PkgLength is a bit count, not an offset. Callers
     * that treat the value as an offset check it (see aml_pkg_end). */
    return len;
}

/* A NameSeg is four characters: a letter or '_' first, then letters, digits or '_'. */
bool aml_nameseg(aml_stream_t *s, char out[4]) {
    for (unsigned i = 0; i < 4; i++) out[i] = (char)aml_u8(s);
    if (s->bad) return false;
    char c = out[0];
    bool first_ok = (c >= 'A' && c <= 'Z') || c == '_';
    if (!first_ok) {
        s->bad = true;
        return false;
    }
    return true;
}

/* NameString := [RootChar] [PrefixPath] NamePath. A NamePath is a NameSeg, a DualNamePrefix
 * with two segments, a MultiNamePrefix with a count and segments, or NullName (no segments). */
bool aml_namestring(aml_stream_t *s, aml_path_t *out) {
    for (unsigned i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;
    if (aml_peek(s) == '\\') {
        out->root = true;
        s->pos++;
    }
    while (!s->bad && aml_peek(s) == '^') {
        out->parents++;
        s->pos++;
    }
    uint8_t lead = aml_peek(s);
    unsigned count = 1;
    if (s->bad) return false;
    if (lead == 0x00) {                 /* NullName: no segments */
        s->pos++;
        return true;
    }
    if (lead == 0x2E) {                 /* DualNamePrefix */
        s->pos++;
        count = 2;
    } else if (lead == 0x2F) {          /* MultiNamePrefix */
        s->pos++;
        count = aml_u8(s);
    }
    if (count > 16) {
        s->bad = true;
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        if (!aml_nameseg(s, out->seg[out->nseg])) return false;
        out->nseg++;
    }
    return !s->bad;
}

/* End offset of the object whose PkgLength starts at the cursor. Reads the position before
 * the length (C leaves the order of "s->pos + f(s)" unspecified, so it is split here). */
uint32_t aml_pkg_end(aml_stream_t *s) {
    uint32_t start = s->pos;
    uint32_t len = aml_pkglen(s);
    if (s->bad || start + len > s->end) {
        s->bad = true;
        return s->end;
    }
    return start + len;
}
