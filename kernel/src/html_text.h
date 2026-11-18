/* html_text.h — turn an HTML page into plain text for the Browser window.
 *
 * This is not a layout engine. Tags are dropped, <script> and <style> contents are
 * skipped, block tags become line breaks, runs of whitespace become one space, and a few
 * entities are decoded. Bytes outside ASCII are shown as '?' because the font is ASCII.
 *
 * The "_ex" entry point also records <a href> anchors and <img src> images: the text gets
 * "[N]" before each link's words and a "\x01<idx>" line where an image stood, so the
 * browser can make links clickable and blit the images it decodes. */
#ifndef HELIX_HTML_TEXT_H
#define HELIX_HTML_TEXT_H

#include <stdint.h>

#define HTML_MAX_LINKS  32
#define HTML_MAX_IMAGES 16
#define HTML_URL_CAP    240
#define HTML_ALT_CAP    48

typedef struct {
    uint32_t count;                                   /* links found */
    char url[HTML_MAX_LINKS][HTML_URL_CAP];           /* each link's href */
} html_links_t;

typedef struct {
    uint32_t count;                                   /* images found */
    char src[HTML_MAX_IMAGES][HTML_URL_CAP];          /* each image's src */
    char alt[HTML_MAX_IMAGES][HTML_ALT_CAP];          /* its alt text, or "" */
} html_images_t;

/* Write the text of in[0..n) into out (at most cap-1 bytes, then a NUL). Returns the
 * number of bytes written, not counting the NUL. cap must be at least 1. */
uint32_t html_to_text(const uint8_t *in, uint32_t n, char *out, uint32_t cap);

/* As above, but also fills *links and *images when they are not NULL. */
uint32_t html_to_text_ex(const uint8_t *in, uint32_t n, char *out, uint32_t cap,
                         html_links_t *links, html_images_t *images);

#endif /* HELIX_HTML_TEXT_H */
