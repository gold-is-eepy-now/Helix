/* fb.h — linear framebuffer text console with colour and a cursor. */
#ifndef HELIX_FB_H
#define HELIX_FB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct limine_framebuffer;

/* Attach to a Limine framebuffer and reset the text cursor. */
void fb_init(struct limine_framebuffer *fb);

/* True once a usable framebuffer has been attached. */
bool fb_ready(void);

/* Pack 8-bit R/G/B into whatever pixel format the firmware gave us. */
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

/* Set the colours used by subsequent writes (packed with fb_rgb). */
void fb_set_colors(uint32_t fg, uint32_t bg);

/* Fill the whole screen with one packed colour and home the cursor. */
void fb_clear(uint32_t color);
void fb_cursor_to(size_t row, size_t column);
void fb_clear_current_line(void);

/* Present packed 0x00RRGGBB pixels, scaling them to fit while preserving the
 * source aspect ratio. This switches the framebuffer into graphics mode and
 * disables text cursor updates until fb_graphics_end(). */
bool fb_present_rgb(const uint32_t *pixels, uint32_t width, uint32_t height);
void fb_graphics_end(void);

/* Draw a NUL-terminated string / one character at the cursor. */
void fb_write(const char *s);
void fb_putchar(char c);

/* Rub out the previous character (does nothing at the start of a line). */
void fb_backspace(void);

/* Report the framebuffer geometry. Returns false if there is no framebuffer. */
bool fb_get_info(uint64_t *width, uint64_t *height, uint64_t *bpp);

#endif /* HELIX_FB_H */
