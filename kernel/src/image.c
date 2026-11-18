/* image.c — BMP and P6 PPM decoding (see image.h).
 *
 * Only uncompressed 24/32-bit BMP and binary PPM are handled: those are the formats a
 * text browser needs and the only ones whose layout is small enough to trust without a
 * codec. Anything else is refused rather than guessed at. */
#include "image.h"

#include "heap.h"
#include "kstring.h"

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t rd32s(const uint8_t *p) {
    return (int32_t)rd32(p);
}

/* A 0xAARRGGBB pixel into straight-alpha B,G,R,A bytes. */
static void put_px(uint8_t *d, uint32_t argb) {
    uint32_t a = argb >> 24;
    if (a == 0) a = 255;                     /* BMP has no alpha in BI_RGB */
    d[0] = (uint8_t)(argb & 0xffu);
    d[1] = (uint8_t)((argb >> 8) & 0xffu);
    d[2] = (uint8_t)((argb >> 16) & 0xffu);
    d[3] = (uint8_t)a;
}

/* BI_RGB BMP: 24 or 32 bits per pixel, rows padded to 4 bytes. */
static bool decode_bmp(const uint8_t *in, uint32_t n, image_t *img) {
    if (n < 54 || in[0] != 'B' || in[1] != 'M') return false;
    uint32_t off = rd32(in + 10);
    uint32_t hdr = rd32(in + 14);
    if (hdr < 40 || n < 14 + hdr) return false;
    int32_t w = rd32s(in + 18);
    int32_t h = rd32s(in + 22);
    uint32_t bpp = rd16(in + 28);
    uint32_t comp = rd32(in + 30);
    if (comp != 0 || (bpp != 24 && bpp != 32)) return false;   /* compressed or paletted: no */
    bool top_down = h < 0;
    if (top_down) h = -h;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    uint32_t stride = (bpp == 24) ? ((uint32_t)w * 3u + 3u) & ~3u : (uint32_t)w * 4u;
    if (off > n || (uint64_t)off + (uint64_t)stride * (uint64_t)h > n) return false;

    uint8_t *out = (uint8_t *)kmalloc((size_t)w * (size_t)h * 4u);
    if (out == NULL) return false;
    img->w = (uint32_t)w;
    img->h = (uint32_t)h;
    img->bgra = out;

    for (int32_t row = 0; row < h; row++) {
        int32_t src_row = top_down ? row : h - 1 - row;
        const uint8_t *s = in + off + (uint64_t)stride * (uint64_t)src_row;
        uint8_t *d = out + (size_t)row * (size_t)w * 4u;
        for (int32_t col = 0; col < w; col++) {
            if (bpp == 24) {
                put_px(d + 4u * (size_t)col, 0xff000000u |
                       ((uint32_t)s[col * 3 + 2] << 16) |
                       ((uint32_t)s[col * 3 + 1] << 8) | (uint32_t)s[col * 3 + 0]);
            } else {
                put_px(d + 4u * (size_t)col, 0xff000000u |
                       ((uint32_t)s[col * 4 + 2] << 16) |
                       ((uint32_t)s[col * 4 + 1] << 8) | (uint32_t)s[col * 4 + 0]);
            }
        }
    }
    return true;
}

/* P6 PPM: "P6" whitespace width whitespace height whitespace maxval, then raw RGB. */
static bool decode_ppm(const uint8_t *in, uint32_t n, image_t *img) {
    uint32_t i = 0;
    if (n < 2 || in[0] != 'P' || in[1] != '6') return false;
    i = 2;
    uint32_t vals[3];
    for (uint32_t k = 0; k < 3; k++) {
        while (i < n && (in[i] == ' ' || in[i] == '\t' || in[i] == '\n' || in[i] == '\r' ||
                         in[i] == '#')) {
            if (in[i] == '#') while (i < n && in[i] != '\n') i++;
            else i++;
        }
        if (i >= n || in[i] < '0' || in[i] > '9') return false;
        uint32_t v = 0;
        while (i < n && in[i] >= '0' && in[i] <= '9' && v < 100000u) {
            v = v * 10u + (uint32_t)(in[i] - '0');
            i++;
        }
        vals[k] = v;
    }
    while (i < n && (in[i] == ' ' || in[i] == '\t' || in[i] == '\n' || in[i] == '\r')) i++;
    uint32_t w = vals[0], h = vals[1], maxv = vals[2];
    if (w == 0 || h == 0 || w > 4096 || h > 4096 || maxv == 0 || maxv > 255) return false;
    if ((uint64_t)i + (uint64_t)w * (uint64_t)h * 3u > n) return false;

    uint8_t *out = (uint8_t *)kmalloc((size_t)w * (size_t)h * 4u);
    if (out == NULL) return false;
    img->w = w;
    img->h = h;
    img->bgra = out;
    const uint8_t *s = in + i;
    for (uint32_t p = 0; p < w * h; p++) {
        put_px(out + 4u * p, 0xff000000u | ((uint32_t)s[p * 3] << 16) |
                             ((uint32_t)s[p * 3 + 1] << 8) | (uint32_t)s[p * 3 + 2]);
    }
    return true;
}

bool image_decode(const uint8_t *in, uint32_t n, image_t *img) {
    if (img == NULL) return false;
    img->w = 0;
    img->h = 0;
    img->bgra = NULL;
    if (in == NULL || n < 8) return false;
    if (decode_bmp(in, n, img)) return true;
    if (decode_ppm(in, n, img)) return true;
    return false;
}

void image_free(image_t *img) {
    if (img == NULL) return;
    if (img->bgra != NULL) kfree(img->bgra);
    img->bgra = NULL;
    img->w = 0;
    img->h = 0;
}
