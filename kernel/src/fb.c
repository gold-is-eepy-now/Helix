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

#define GLYPH_W 8u
#define GLYPH_H 8u
#define LINE_H  12u /* glyph height + 4px leading, so lines do not touch */
#define MARGIN_X 16u
#define MARGIN_Y 16u

static struct limine_framebuffer *g_fb;
static size_t g_cursor_x;
static size_t g_cursor_y;
static uint32_t g_fg;
static uint32_t g_bg;
static bool g_cursor_drawn;

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

static void put_pixel(size_t x, size_t y, uint32_t color) {
    if (x >= g_fb->width || y >= g_fb->height) {
        return;
    }
    size_t bytes_per_pixel = g_fb->bpp / 8u;
    uint8_t *pixel = (uint8_t *)g_fb->address
                   + y * g_fb->pitch
                   + x * bytes_per_pixel;
    for (size_t i = 0; i < bytes_per_pixel; i++) {
        pixel[i] = (uint8_t)(color >> (8u * i));
    }
}

void fb_clear(uint32_t color) {
    if (!fb_ready()) {
        return;
    }
    for (size_t y = 0; y < g_fb->height; y++) {
        for (size_t x = 0; x < g_fb->width; x++) {
            put_pixel(x, y, color);
        }
    }
    g_cursor_x = MARGIN_X;
    g_cursor_y = MARGIN_Y;
    g_cursor_drawn = false;
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

void fb_putchar(char c) {
    if (!fb_ready()) {
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
