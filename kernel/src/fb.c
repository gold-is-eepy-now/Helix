/* fb.c — linear framebuffer text renderer with colour and a block cursor.
 *
 * Everything here is "dumb but correct": we compute the byte address of each
 * pixel from the framebuffer's pitch (bytes per scanline) and bit depth, then
 * write it directly. That is how every pre-3D OS console worked.
 *
 * The cursor is drawn as an underscore under the next character cell. Every
 * character write erases the cursor first, draws the glyph, then redraws the
 * cursor, so the cursor never smears.
 */
#include "fb.h"

#include <limine.h>

#include "font8x8.h" /* public-domain 8x8 font, const unsigned char [128][8] */
#include "virtio_gpu.h"

#define GLYPH_W 8u
#define GLYPH_H 8u
#define LINE_H  12u /* glyph height + 4px leading, so lines do not touch */
#define MARGIN_X 16u
#define MARGIN_Y 16u

static struct limine_framebuffer *g_fb;     /* where drawing goes: the firmware framebuffer or g_virt */
static struct limine_framebuffer g_virt;    /* the virtio-gpu backing memory, described like a framebuffer */
static bool g_on_virtio;
static bool g_dirty;                        /* something changed since the last fb_present */
static size_t g_dx0, g_dy0, g_dx1, g_dy1;   /* the changed rectangle: [x0,x1) x [y0,y1) */
static size_t g_cursor_x;
static size_t g_cursor_y;
static uint32_t g_fg;
static uint32_t g_bg;
static bool g_cursor_drawn;
static bool g_graphics_mode;

void fb_init(struct limine_framebuffer *fb) {
    g_fb = fb;
    g_cursor_x = MARGIN_X;
    g_cursor_y = MARGIN_Y;
    g_cursor_drawn = false;
    g_fg = 0x00E0E0u;
    g_bg = 0x000000u;
    if (fb_ready()) {
        g_fg = fb_rgb(0xE0, 0xE0, 0xE0);
        g_bg = fb_rgb(0x00, 0x00, 0x00);
        fb_clear(g_bg);
    }
}

bool fb_ready(void) {
    return g_fb != NULL && g_fb->address != NULL;
}

void fb_set_colors(uint32_t fg, uint32_t bg) {
    g_fg = fg;
    g_bg = bg;
}

/* Scale one 8-bit channel into a field of `size` bits starting at `shift`.
 * The firmware describes channels with a bit width and a bit offset; we
 * respect both so we work on 32-bit XRGB, BGRX and 24-bit RGB alike. */
static uint32_t scale_channel(uint8_t value, uint8_t size, uint8_t shift) {
    if (size == 0) {
        return 0;
    }
    uint32_t max = (1u << size) - 1u;
    return (uint32_t)(((uint32_t)value * max) / 255u) << shift;
}

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_fb) {
        return 0;
    }
    return scale_channel(r, g_fb->red_mask_size, g_fb->red_mask_shift)
         | scale_channel(g, g_fb->green_mask_size, g_fb->green_mask_shift)
         | scale_channel(b, g_fb->blue_mask_size, g_fb->blue_mask_shift);
}

static void mark_dirty(size_t x, size_t y) {
    if (!g_dirty) {
        g_dx0 = x;
        g_dy0 = y;
        g_dx1 = x + 1;
        g_dy1 = y + 1;
        g_dirty = true;
        return;
    }
    if (x < g_dx0) g_dx0 = x;
    if (y < g_dy0) g_dy0 = y;
    if (x + 1 > g_dx1) g_dx1 = x + 1;
    if (y + 1 > g_dy1) g_dy1 = y + 1;
}

static void put_pixel(size_t x, size_t y, uint32_t color) {
    if (x >= g_fb->width || y >= g_fb->height) {
        return;
    }
    if (g_on_virtio) mark_dirty(x, y);
    size_t bytes_per_pixel = g_fb->bpp / 8u;
    uint8_t *pixel = (uint8_t *)g_fb->address
                   + y * g_fb->pitch
                   + x * bytes_per_pixel;
    for (size_t i = 0; i < bytes_per_pixel; i++) {
        pixel[i] = (uint8_t)(color >> (8u * i));
    }
}

/* Read a pixel back in the same packing put_pixel writes. Off-screen reads
 * return 0. The desktop uses this to save what is under the pointer. */
uint32_t fb_get_pixel(uint32_t x, uint32_t y) {
    if (!fb_ready() || x >= g_fb->width || y >= g_fb->height) {
        return 0;
    }
    size_t bytes_per_pixel = g_fb->bpp / 8u;
    const uint8_t *pixel = (const uint8_t *)g_fb->address
                         + (size_t)y * g_fb->pitch
                         + (size_t)x * bytes_per_pixel;
    uint32_t color = 0;
    for (size_t i = 0; i < bytes_per_pixel; i++) {
        color |= (uint32_t)pixel[i] << (8u * i);
    }
    return color;
}

void fb_clear(uint32_t color) {
    if (!fb_ready()) {
        return;
    }
    if (g_on_virtio) { mark_dirty(0, 0); g_dx1 = g_fb->width; g_dy1 = g_fb->height; }
    for (size_t y = 0; y < g_fb->height; y++) {
        for (size_t x = 0; x < g_fb->width; x++) {
            put_pixel(x, y, color);
        }
    }
    g_cursor_x = MARGIN_X;
    g_cursor_y = MARGIN_Y;
    g_cursor_drawn = false;
}

bool fb_present_rgb(const uint32_t *pixels, uint32_t width, uint32_t height) {
    if (!fb_ready() || pixels == NULL || width == 0 || height == 0 ||
        g_fb->bpp < 24) return false;

    /* Fit the image without distortion, then center it to produce black bars. */
    uint64_t out_w = g_fb->width;
    uint64_t out_h = (out_w * height) / width;
    if (out_h > g_fb->height) {
        out_h = g_fb->height;
        out_w = (out_h * width) / height;
    }
    if (out_w == 0 || out_h == 0) return false;
    uint64_t x0 = (g_fb->width - out_w) / 2;
    uint64_t y0 = (g_fb->height - out_h) / 2;

    /* Clear bars first, honoring firmware channel layout for the pixels. */
    uint32_t black = fb_rgb(0, 0, 0);
    for (uint64_t y = 0; y < g_fb->height; y++) {
        for (uint64_t x = 0; x < g_fb->width; x++) put_pixel(x, y, black);
    }
    for (uint64_t y = 0; y < out_h; y++) {
        uint32_t sy = (uint32_t)((y * height) / out_h);
        for (uint64_t x = 0; x < out_w; x++) {
            uint32_t sx = (uint32_t)((x * width) / out_w);
            uint32_t rgb = pixels[(size_t)sy * width + sx];
            uint32_t packed = fb_rgb((uint8_t)(rgb >> 16),
                                     (uint8_t)(rgb >> 8), (uint8_t)rgb);
            put_pixel(x0 + x, y0 + y, packed);
        }
    }
    g_graphics_mode = true;
    g_cursor_drawn = false;
    return true;
}

void fb_graphics_end(void) {
    g_graphics_mode = false;
    g_cursor_drawn = false;
    if (fb_ready()) fb_clear(g_bg);
}

/* Move the whole image up by one text row and blank the new bottom row. */
static void scroll_up(void) {
    size_t row_bytes = g_fb->pitch;
    size_t shift = LINE_H * row_bytes;
    size_t total = row_bytes * g_fb->height;
    uint8_t *base = (uint8_t *)g_fb->address;

    for (size_t i = 0; i + shift < total; i++) {
        base[i] = base[i + shift];
    }
    for (size_t i = total - shift; i < total; i++) {
        base[i] = 0;
    }
    if (g_on_virtio) {                    /* the whole screen moved */
        mark_dirty(0, 0);
        g_dx1 = g_fb->width;
        g_dy1 = g_fb->height;
    }
}

bool fb_use_virtio(void *pixels, uint32_t width, uint32_t height) {
    if (!fb_ready() || pixels == NULL || width == 0 || height == 0) return false;
    /* The backing uses 32-bit B8G8R8X8. Colours are already packed for the firmware
     * framebuffer, so the layouts must agree or every colour would change. */
    if (g_fb->bpp != 32 || g_fb->red_mask_size != 8 || g_fb->red_mask_shift != 16 ||
        g_fb->green_mask_size != 8 || g_fb->green_mask_shift != 8 ||
        g_fb->blue_mask_size != 8 || g_fb->blue_mask_shift != 0) {
        return false;
    }
    size_t cw = width < g_fb->width ? width : g_fb->width;
    size_t ch = height < g_fb->height ? height : g_fb->height;
    g_virt = *g_fb;
    g_virt.address = pixels;
    g_virt.width = width;
    g_virt.height = height;
    g_virt.pitch = (uint64_t)width * 4u;
    /* Keep what is on screen: copy the firmware framebuffer's rows across. */
    for (size_t y = 0; y < ch; y++) {
        const uint8_t *src = (const uint8_t *)g_fb->address + y * g_fb->pitch;
        uint8_t *dst = (uint8_t *)pixels + y * g_virt.pitch;
        for (size_t i = 0; i < cw * 4u; i++) dst[i] = src[i];
    }
    g_fb = &g_virt;
    g_on_virtio = true;
    g_dirty = true;
    g_dx0 = 0;
    g_dy0 = 0;
    g_dx1 = width;
    g_dy1 = height;
    return true;
}

void fb_present(void) {
    /* Never re-enter: logging from here would draw text, which would present again. */
    static bool in_present;
    if (in_present || !g_on_virtio || !g_dirty) return;
    in_present = true;
    size_t x1 = g_dx1 > g_fb->width ? g_fb->width : g_dx1;
    size_t y1 = g_dy1 > g_fb->height ? g_fb->height : g_dy1;
    g_dirty = false;
    if (x1 > g_dx0 && y1 > g_dy0) {
        /* A failed flush leaves the rest of the picture as it was; the next change retries. */
        (void)virtio_gpu_present((uint32_t)g_dx0, (uint32_t)g_dy0,
                                 (uint32_t)(x1 - g_dx0), (uint32_t)(y1 - g_dy0));
    }
    in_present = false;
}

static void draw_cursor(void) {
    if (!fb_ready() || g_cursor_drawn) {
        return;
    }
    size_t y = g_cursor_y + GLYPH_H - 1;
    for (size_t x = 0; x < GLYPH_W; x++) {
        put_pixel(g_cursor_x + x, y, g_fg);
    }
    g_cursor_drawn = true;
}

static void erase_cursor(void) {
    if (!fb_ready() || !g_cursor_drawn) {
        return;
    }
    size_t y = g_cursor_y + GLYPH_H - 1;
    for (size_t x = 0; x < GLYPH_W; x++) {
        put_pixel(g_cursor_x + x, y, g_bg);
    }
    g_cursor_drawn = false;
}

static void newline(void) {
    erase_cursor();
    g_cursor_x = MARGIN_X;
    g_cursor_y += LINE_H;
    if (g_cursor_y + GLYPH_H > g_fb->height) {
        scroll_up();
        g_cursor_y -= LINE_H;
    }
}

static void putchar_raw(char c) {
    if (!fb_ready() || g_graphics_mode) {
        return;
    }

    erase_cursor();

    if (c == '\n') {
        newline();
        draw_cursor();
        return;
    }
    if (c == '\r') {
        g_cursor_x = MARGIN_X;
        draw_cursor();
        return;
    }

    if (g_cursor_x + GLYPH_W > g_fb->width) {
        newline();
    }

    unsigned char glyph = (unsigned char)c;
    if (glyph < 128) {
        for (size_t row = 0; row < GLYPH_H; row++) {
            unsigned char bits = font8x8_basic[glyph][row];
            for (size_t col = 0; col < GLYPH_W; col++) {
                uint32_t color = (bits & (1u << col)) ? g_fg : g_bg;
                put_pixel(g_cursor_x + col, g_cursor_y + row, color);
            }
        }
    }
    g_cursor_x += GLYPH_W;
    draw_cursor();
}

void fb_putchar(char c) {
    putchar_raw(c);
    fb_present();                   /* the shell echoes each key: show it now */
}

void fb_write(const char *s) {
    for (; *s != '\0'; s++) {
        fb_putchar(*s);
    }
}

void fb_backspace(void) {
    if (!fb_ready()) {
        return;
    }
    erase_cursor();
    if (g_cursor_x > MARGIN_X) {
        g_cursor_x -= GLYPH_W;
        /* Blank the cell we just left. */
        for (size_t row = 0; row < GLYPH_H; row++) {
            for (size_t col = 0; col < GLYPH_W; col++) {
                put_pixel(g_cursor_x + col, g_cursor_y + row, g_bg);
            }
        }
    }
    draw_cursor();
}

bool fb_get_info(uint64_t *width, uint64_t *height, uint64_t *bpp) {
    if (!fb_ready()) {
        return false;
    }
    if (width) {
        *width = g_fb->width;
    }
    if (height) {
        *height = g_fb->height;
    }
    if (bpp) {
        *bpp = g_fb->bpp;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Graphics primitives for the desktop (desktop.c).                    */
/* ------------------------------------------------------------------ */

/* Stop the text console from drawing while a program owns the screen. */
void fb_graphics_begin(void) {
    g_graphics_mode = true;
    g_cursor_drawn = false;
}

uint32_t fb_width(void) {
    return fb_ready() ? (uint32_t)g_fb->width : 0;
}

uint32_t fb_height(void) {
    return fb_ready() ? (uint32_t)g_fb->height : 0;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb_ready()) {
        return;
    }
    for (uint32_t j = 0; j < h; j++) {
        for (uint32_t i = 0; i < w; i++) {
            put_pixel((size_t)x + i, (size_t)y + j, color);
        }
    }
}

/* Draw one 8x8 glyph per character, left to right. With `opaque` false only the
 * set pixels are drawn, so text can sit on a coloured background. */
void fb_draw_text(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg, bool opaque) {
    if (!fb_ready()) {
        return;
    }
    for (; *s != '\0'; s++, x += GLYPH_W) {
        unsigned char glyph = (unsigned char)*s;
        if (glyph >= 128) {
            glyph = '?';
        }
        for (uint32_t row = 0; row < GLYPH_H; row++) {
            unsigned char bits = font8x8_basic[glyph][row];
            for (uint32_t col = 0; col < GLYPH_W; col++) {
                if (bits & (1u << col)) {
                    put_pixel((size_t)x + col, (size_t)y + row, fg);
                } else if (opaque) {
                    put_pixel((size_t)x + col, (size_t)y + row, bg);
                }
            }
        }
    }
}
