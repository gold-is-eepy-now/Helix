/* image.h — a small still-image decoder for the Browser and the file preview.
 *
 * Supports the two formats a from-scratch system can decode without a codec library:
 * Windows BMP (24/32-bit uncompressed, bottom-up or top-down) and Netpbm P6 PPM. The
 * output is straight-alpha B,G,R,A bytes, ready for fb_blit_bgra. */
#ifndef HELIX_IMAGE_H
#define HELIX_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t w, h;        /* pixels */
    uint8_t *bgra;        /* w*h*4 bytes, straight alpha; NULL until decode succeeds */
} image_t;

/* Decode in[0..n) into *img (img->bgra is kmalloc'd). False when the format is not
 * recognised or the data is malformed; *img is left zeroed. */
bool image_decode(const uint8_t *in, uint32_t n, image_t *img);

/* Release the buffer from image_decode. Safe on a zeroed image_t. */
void image_free(image_t *img);

#endif /* HELIX_IMAGE_H */
