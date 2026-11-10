/* html_text.h — turn an HTML page into plain text for the Browser window.
 *
 * This is not a layout engine. Tags are dropped, <script> and <style> contents are
 * skipped, block tags become line breaks, runs of whitespace become one space, and a few
 * entities are decoded. Bytes outside ASCII are shown as '?' because the font is ASCII. */
#ifndef HELIX_HTML_TEXT_H
#define HELIX_HTML_TEXT_H

#include <stdint.h>

/* Write the text of in[0..n) into out (at most cap-1 bytes, then a NUL). Returns the
 * number of bytes written, not counting the NUL. cap must be at least 1. */
uint32_t html_to_text(const uint8_t *in, uint32_t n, char *out, uint32_t cap);

#endif /* HELIX_HTML_TEXT_H */
