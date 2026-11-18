/* browser-unit.c — host checks for the Browser's HTML and picture decoding.
 *
 * Compiles kernel/src/html_text.c and kernel/src/image.c for the host, so the parts of
 * TODO item 8 that do not need a boot (link numbering, image markers, BMP/PPM) are tested
 * without a VM. kmalloc/kfree are supplied here; the kernel's own heap is not linked.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html_text.h"
#include "image.h"

/* The heap the image decoder allocates from. */
void *kmalloc(size_t size) { return malloc(size); }
void *kzalloc(size_t size) { return calloc(1, size); }
void kfree(void *ptr) { free(ptr); }

static int failures = 0;

static void check(bool ok, const char *what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

/* The image marker line the writer emits: 0x01, the index digit, 0x01. */
static const char *image_line(const char *text, char *buf, uint32_t cap) {
    const char *line = strstr(text, "\x01");
    if (line == NULL) return NULL;
    const char *end = strchr(line, '\n');
    uint32_t n = (uint32_t)((end == NULL ? line + strlen(line) : end) - line);
    if (n >= cap) n = cap - 1;
    memcpy(buf, line, n);
    buf[n] = '\0';
    return buf;
}

static void test_links(void) {
    const char *html =
        "<html><body><h1>Helix</h1><p>See <a href=\"http://example.com/a\">the docs</a> "
        "and <a href='/b'>the other one</a>.</p><a href=\"http://example.com/c\">third</a>"
        "</body></html>";
    char out[1024];
    html_links_t links;
    html_images_t images;
    uint32_t n = html_to_text_ex((const uint8_t *)html, (uint32_t)strlen(html), out, sizeof out,
                                 &links, &images);
    check(n > 0, "html_to_text_ex returns text");
    check(strstr(out, "[1]") != NULL, "first anchor is numbered 1");
    check(strstr(out, "[2]") != NULL, "second anchor is numbered 2");
    check(strstr(out, "[3]") != NULL, "third anchor is numbered 3");
    check(links.count == 3, "three hrefs recorded");
    check(strcmp(links.url[0], "http://example.com/a") == 0, "href 1 kept verbatim");
    check(strcmp(links.url[1], "/b") == 0, "href 2 kept verbatim");
    check(strcmp(links.url[2], "http://example.com/c") == 0, "href 3 kept verbatim");
    check(strstr(out, "the docs") != NULL, "the link's words survive");
    check(strstr(out, "<a") == NULL && strstr(out, "href") == NULL, "tags are gone");
    check(images.count == 0, "no images means none recorded");

    /* The plain html_to_text wrapper still works and records nothing. */
    html_links_t *no_links = NULL;
    uint32_t n2 = html_to_text((const uint8_t *)html, (uint32_t)strlen(html), out, sizeof out);
    (void)no_links;
    check(n2 > 0 && strstr(out, "[1]") == NULL, "html_to_text leaves out the numbering");
}

static void test_images(void) {
    const char *html =
        "<p>Before</p><img src=\"pic.png\" alt=\"A picture\"><p>After</p>"
        "<img alt=\"no source\"><img src=\"/x.bmp\" alt='second'>";
    char out[1024];
    html_links_t links;
    html_images_t images;
    html_to_text_ex((const uint8_t *)html, (uint32_t)strlen(html), out, sizeof out, &links,
                    &images);
    check(images.count == 3, "three images recorded");
    check(strcmp(images.src[0], "pic.png") == 0, "src 1 kept");
    check(strcmp(images.alt[0], "A picture") == 0, "alt 1 kept");
    check(strcmp(images.src[1], "") == 0, "an image with no src keeps an empty src");
    check(strcmp(images.src[2], "/x.bmp") == 0, "src 3 kept (single quotes)");
    check(strcmp(images.alt[2], "second") == 0, "alt 3 kept");

    char buf[128];
    check(image_line(out, buf, sizeof buf) != NULL, "an image marker line exists");
    check(buf[0] == 0x01 && buf[1] == '0' && buf[2] == 0x01, "first marker is index 0");
    check(strstr(buf, "A picture") != NULL, "the alt text follows the marker");
    check(strstr(out, "Before") != NULL && strstr(out, "After") != NULL,
          "surrounding text survives");
    check(links.count == 0, "an <a> is still needed for links");
}

/* A 2x2 bottom-up 24-bit BMP: red, green / blue, white. Each row is 6 bytes of pixels
 * plus 2 bytes of padding, because BMP rows are a multiple of 4. */
static uint32_t make_bmp(uint8_t *buf, uint32_t cap) {
    static const uint8_t row0[] = {255, 0, 0, 0, 255, 0, 0, 0};      /* bottom: red, green */
    static const uint8_t row1[] = {0, 0, 255, 255, 255, 255, 0, 0};  /* top: blue, white */
    uint32_t stride = 8;
    uint32_t size = 14 + 40 + stride * 2;
    if (size > cap) return 0;
    memset(buf, 0, size);
    buf[0] = 'B';
    buf[1] = 'M';
    buf[2] = (uint8_t)size;
    buf[5] = (uint8_t)(size >> 16);
    buf[10] = 54;                        /* pixel offset */
    buf[14] = 40;                        /* DIB header size */
    buf[18] = 2;                         /* width */
    buf[22] = 2;                         /* height */
    buf[26] = 1;                         /* planes */
    buf[28] = 24;                        /* bits per pixel */
    memcpy(buf + 54, row0, sizeof row0);
    memcpy(buf + 54 + stride, row1, sizeof row1);
    return size;
}

static void test_bmp(void) {
    uint8_t raw[256];
    uint32_t n = make_bmp(raw, sizeof raw);
    image_t im;
    bool ok = image_decode(raw, n, &im);
    check(ok, "a 24-bit BMP decodes");
    if (!ok) return;
    check(im.w == 2 && im.h == 2, "2x2 decoded");
    /* Bottom-up: the file's first row is the image's bottom row, and BMP stores BGR. */
    check(im.bgra[0] == 0 && im.bgra[1] == 0 && im.bgra[2] == 255 && im.bgra[3] == 255,
          "top-left pixel is red");
    check(im.bgra[4] == 255 && im.bgra[5] == 255 && im.bgra[6] == 255,
          "top-right pixel is white");
    check(im.bgra[8] == 255 && im.bgra[9] == 0 && im.bgra[10] == 0 && im.bgra[11] == 255,
          "bottom-left pixel is blue");
    check(im.bgra[12] == 0 && im.bgra[13] == 255 && im.bgra[14] == 0,
          "bottom-right pixel is green");
    image_free(&im);
    check(im.bgra == NULL, "image_free releases the pixels");

    /* A truncated file must be refused rather than read past its end. */
    image_t bad;
    check(!image_decode(raw, 20, &bad), "a truncated BMP is refused");
    check(!image_decode((const uint8_t *)"not an image at all", 19, &bad),
          "garbage is refused");
    check(bad.bgra == NULL, "a refused decode leaves no pixels");
}

static void test_ppm(void) {
    const char *header = "P6\n# a comment\n2 1\n255\n";
    static const uint8_t body[] = {255, 0, 0, 0, 255, 0};   /* red, green */
    char buf[128];
    memcpy(buf, header, strlen(header));
    memcpy(buf + strlen(header), body, sizeof body);
    uint32_t n = (uint32_t)strlen(header) + (uint32_t)sizeof body;
    image_t im;
    bool ok = image_decode((const uint8_t *)buf, n, &im);
    check(ok, "a P6 PPM decodes");
    if (!ok) return;
    check(im.w == 2 && im.h == 1, "2x1 decoded");
    check(im.bgra[0] == 0 && im.bgra[1] == 0 && im.bgra[2] == 255, "first pixel is red");
    check(im.bgra[4] == 0 && im.bgra[5] == 255 && im.bgra[6] == 0, "second pixel is green");
    image_free(&im);
}

int main(void) {
    test_links();
    test_images();
    test_bmp();
    test_ppm();
    printf(failures == 0 ? "\nAll browser checks passed\n" : "\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
