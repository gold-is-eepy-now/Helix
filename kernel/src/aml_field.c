/* aml_field.c — reading and writing named fields inside operation regions.
 *
 * A field is a run of bits in a region. Simple fields go straight to the region. Bank fields
 * first write their bank selector. Index fields select a byte with an index register and then
 * read or write a data register, one byte at a time. Access widths (ByteAcc, WordAcc and so
 * on) are not honoured yet: memory-mapped registers that need a wide access may misbehave. */
#include "aml_internal.h"
#include "heap.h"
#include "kstring.h"

static aml_status_t read_bits_simple(aml_node_t *f, uint8_t *dst, uint32_t nbytes) {
    aml_node_t *r = f->u.field.region;
    if (r == NULL || r->kind != NK_REGION || !r->u.region.ok) return AML_ERR_HARDWARE;
    uint64_t bit_off = f->u.field.bit_off;
    uint32_t bits = f->u.field.bit_len;
    for (uint32_t i = 0; i < nbytes; i++) dst[i] = 0;
    for (uint32_t k = 0; k < bits; k++) {
        uint64_t src = bit_off + k;
        uint8_t byte;
        aml_status_t st = aml_region_read8(r->u.region.space, r->u.region.base + src / 8, &byte);
        if (st != AML_OK) return st;
        if ((byte >> (src % 8)) & 1) dst[k / 8] |= (uint8_t)(1u << (k % 8));
    }
    return AML_OK;
}

/* Write the field's bits, touching only the bytes the field covers. A byte that the field
 * shares with other fields is read first, so their bits are kept. */
static aml_status_t write_bits_simple(aml_node_t *f, const uint8_t *src) {
    aml_node_t *r = f->u.field.region;
    if (r == NULL || r->kind != NK_REGION || !r->u.region.ok) return AML_ERR_HARDWARE;
    uint64_t bit_off = f->u.field.bit_off;
    uint32_t bits = f->u.field.bit_len;
    for (uint64_t k = 0; k < bits;) {
        uint64_t pos = bit_off + k;
        uint64_t addr = r->u.region.base + pos / 8;
        uint8_t cur;
        aml_status_t st = aml_region_read8(r->u.region.space, addr, &cur);
        if (st != AML_OK) return st;
        for (unsigned bit = (unsigned)(pos % 8); bit < 8 && k < bits; bit++, k++) {
            uint8_t v = (uint8_t)((src[k / 8] >> (k % 8)) & 1u);
            cur = (uint8_t)((cur & ~(1u << bit)) | (v << bit));
        }
        st = aml_region_write8(r->u.region.space, addr, cur);
        if (st != AML_OK) return st;
    }
    return AML_OK;
}

static aml_status_t select_bank(aml_node_t *f) {
    if (f->u.field.kind != FK_BANK) return AML_OK;
    aml_value_t v = { .type = AML_T_INT, .integer = f->u.field.bank_value };
    return aml_store_node(f->u.field.bank_reg, &v);
}

static aml_status_t index_byte_read(aml_node_t *f, uint32_t k, uint8_t *out) {
    aml_value_t idx = { .type = AML_T_INT, .integer = f->u.field.bit_off / 8 + k };
    aml_status_t st = aml_store_node(f->u.field.index_reg, &idx);
    if (st != AML_OK) return st;
    aml_value_t data;
    st = aml_value_of_node(f->u.field.data_reg, &data);
    if (st != AML_OK) return st;
    uint64_t v;
    st = aml_to_int(&data, &v);
    *out = (uint8_t)v;
    return st;
}

static aml_status_t index_byte_write(aml_node_t *f, uint32_t k, uint8_t byte) {
    aml_value_t idx = { .type = AML_T_INT, .integer = f->u.field.bit_off / 8 + k };
    aml_status_t st = aml_store_node(f->u.field.index_reg, &idx);
    if (st != AML_OK) return st;
    aml_value_t data = { .type = AML_T_INT, .integer = byte };
    return aml_store_node(f->u.field.data_reg, &data);
}

aml_status_t aml_field_read(aml_node_t *f, aml_value_t *out) {
    uint32_t bits = f->u.field.bit_len;
    if (bits == 0) {
        memset(out, 0, sizeof(*out));
        out->type = AML_T_INT;
        return AML_OK;
    }
    uint32_t nbytes = (bits + 7) / 8;
    uint8_t *buf = aml_arena_alloc(nbytes);
    if (buf == NULL) return AML_ERR_HARDWARE;
    aml_status_t st;
    if (f->u.field.kind == FK_INDEX) {
        if (f->u.field.bit_off % 8 || bits % 8) return AML_ERR_UNSUPPORTED;
        for (uint32_t k = 0; k < nbytes; k++) {
            st = index_byte_read(f, k, &buf[k]);
            if (st != AML_OK) return st;
        }
    } else {
        st = select_bank(f);
        if (st != AML_OK) return st;
        st = read_bits_simple(f, buf, nbytes);
        if (st != AML_OK) return st;
    }
    memset(out, 0, sizeof(*out));
    if (bits <= 64) {
        uint64_t v = 0;
        for (uint32_t i = 0; i < nbytes; i++) v |= (uint64_t)buf[i] << (8 * i);
        out->type = AML_T_INT;
        out->integer = aml_mask(v & (bits == 64 ? ~0ull : ((1ull << bits) - 1)));
    } else {
        out->type = AML_T_BUF;
        out->data = buf;
        out->len = nbytes;
    }
    return AML_OK;
}

aml_status_t aml_field_write(aml_node_t *f, const aml_value_t *value) {
    uint32_t bits = f->u.field.bit_len;
    uint32_t nbytes = (bits + 7) / 8;
    uint8_t *src = aml_arena_alloc(nbytes ? nbytes : 1);
    if (src == NULL) return AML_ERR_HARDWARE;
    memset(src, 0, nbytes ? nbytes : 1);
    if (value->type == AML_T_BUF) {
        for (uint32_t i = 0; i < nbytes && i < value->len; i++) src[i] = value->data[i];
    } else {
        uint64_t v;
        aml_status_t st = aml_to_int(value, &v);
        if (st != AML_OK) return st;
        for (uint32_t i = 0; i < nbytes && i < 8; i++) src[i] = (uint8_t)(v >> (8 * i));
    }
    aml_status_t st;
    if (f->u.field.kind == FK_INDEX) {
        if (f->u.field.bit_off % 8 || bits % 8) return AML_ERR_UNSUPPORTED;
        for (uint32_t k = 0; k < nbytes; k++) {
            st = index_byte_write(f, k, src[k]);
            if (st != AML_OK) return st;
        }
        return AML_OK;
    }
    st = select_bank(f);
    if (st != AML_OK) return st;
    return write_bits_simple(f, src);
}
