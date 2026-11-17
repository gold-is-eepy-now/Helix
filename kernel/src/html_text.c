/* html_text.c — HTML to plain text (see html_text.h).
 *
 * One pass over the bytes. A '<' starts a tag, a comment, or (if nothing tag-like follows)
 * plain text. A '&' starts an entity if a ';' follows closely. Whitespace is collapsed by
 * the writer: a space is written only after visible text and never twice in a row, and at
 * most one blank line separates blocks.
 *
 * When the caller asks for links and images, <a href> becomes "[N] " in front of the
 * link's words and the Nth href is recorded, and <img> becomes its own line holding
 * "\x01<index>\x01" plus the alt text, with the src recorded. */
#include "html_text.h"

#include <stdbool.h>
#include "kstring.h"
#include <stddef.h>

typedef struct {
    char *out;
    uint32_t cap;
    uint32_t len;
    bool space;          /* the last byte written was a space */
    uint32_t newlines;   /* newlines written in a row */
    html_links_t *links;     /* NULL when the caller does not want links */
    html_images_t *images;   /* likewise for images */
} writer_t;

static void put(writer_t *w, char c) {
    if (w->len + 1 >= w->cap) return;          /* keep room for the NUL */
    w->out[w->len++] = c;
}

static void put_char(writer_t *w, char c) {
    put(w, c);
    w->space = false;
    w->newlines = 0;
}

static void put_space(writer_t *w) {
    if (w->len == 0 || w->space || w->newlines > 0) return;
    put(w, ' ');
    w->space = true;
}

static void put_newline(writer_t *w) {
    if (w->len == 0 || w->newlines >= 2) return;
    if (w->space && w->len > 0 && w->out[w->len - 1] == ' ') w->len--;   /* no space before a break */
    put(w, '\n');
    w->newlines++;
    w->space = false;
}

static bool is_alpha(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_digit(uint8_t c) {
    return c >= '0' && c <= '9';
}

static char lower(uint8_t c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
}

static bool streq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Tags that start a new line when they open or close. */
static bool is_block(const char *name) {
    static const char *const blocks[] = {
        "p", "div", "br", "li", "ul", "ol", "h1", "h2", "h3", "h4", "h5", "h6", "tr", "table",
        "hr", "blockquote", "pre", "title", "section", "article", "header", "footer", "nav",
        "dt", "dd",
    };
    for (uint32_t k = 0; k < sizeof blocks / sizeof blocks[0]; k++) {
        if (streq(name, blocks[k])) return true;
    }
    return false;
}

/* Index of the "</name" (case-insensitive) at or after i, or n if there is none. */
static uint32_t find_close(const uint8_t *in, uint32_t n, uint32_t i, const char *name) {
    for (; i + 2 < n; i++) {
        if (in[i] != '<' || in[i + 1] != '/') continue;
        uint32_t k = 0;
        while (name[k] != '\0' && i + 2 + k < n && lower(in[i + 2 + k]) == name[k]) k++;
        if (name[k] == '\0') return i;
    }
    return n;
}

/* Decode the entity whose body (between '&' and ';') is ent. Returns 0 if unknown. */
static char entity_char(const char *ent) {
    if (streq(ent, "amp")) return '&';
    if (streq(ent, "lt")) return '<';
    if (streq(ent, "gt")) return '>';
    if (streq(ent, "quot")) return '"';
    if (streq(ent, "apos")) return '\'';
    if (streq(ent, "nbsp")) return ' ';
    if (ent[0] == '#' && ent[1] >= '0' && ent[1] <= '9') {       /* decimal, printable ASCII only */
        uint32_t v = 0;
        for (uint32_t k = 1; ent[k] != '\0'; k++) {
            if (!is_digit((uint8_t)ent[k]) || k > 3) return 0;
            v = v * 10u + (uint32_t)(ent[k] - '0');
        }
        if (v >= 32u && v <= 126u) return (char)v;
    }
    return 0;
}

/* The value of attribute `name` in the tag body [a, b), matched case-insensitively.
 * Returns true when found; out then holds the value, NUL-terminated. */
static bool attr_value(const uint8_t *in, uint32_t a, uint32_t b, const char *name, char *out,
                       uint32_t cap) {
    size_t want = strlen(name);
    while (a < b) {
        while (a < b && (in[a] == ' ' || in[a] == '\t' || in[a] == '\n' || in[a] == '\r' ||
                         in[a] == '/')) a++;
        uint32_t s = a;
        while (a < b && in[a] != '=' && in[a] != '>' && in[a] != ' ' && in[a] != '\t' &&
               in[a] != '\n' && in[a] != '\r') a++;
        uint32_t e = a;
        while (a < b && (in[a] == ' ' || in[a] == '\t')) a++;
        if (a >= b || in[a] != '=') continue;               /* a boolean attribute */
        a++;                                                /* past the '=' */
        while (a < b && (in[a] == ' ' || in[a] == '\t')) a++;
        char quote = 0;
        if (a < b && (in[a] == '\'' || in[a] == '"')) { quote = (char)in[a]; a++; }
        uint32_t v0 = a;
        while (a < b && in[a] != '>' && in[a] != '\n' && in[a] != '\r' &&
               (quote != 0 ? in[a] != (uint8_t)quote
                           : (in[a] != ' ' && in[a] != '\t'))) a++;
        uint32_t v1 = a;
        while (v1 > v0 && (in[v1 - 1] == ' ' || in[v1 - 1] == '\t')) v1--;
        if (e - s != want) continue;
        bool same = true;
        for (size_t i = 0; i < want; i++) if (lower(in[s + i]) != name[i]) same = false;
        if (!same) continue;
        uint32_t n = v1 - v0;
        if (n == 0) return false;
        if (n > cap - 1) n = cap - 1;
        for (uint32_t i = 0; i < n; i++) out[i] = (char)in[v0 + i];
        out[n] = '\0';
        return true;
    }
    return false;
}

/* Record a link's href and return its 1-based number, or 0 when the table is full. */
static uint32_t note_link(writer_t *w, const char *url) {
    if (w->links == NULL || w->links->count >= HTML_MAX_LINKS) return 0;
    uint32_t n = (uint32_t)strlen(url);
    if (n > HTML_URL_CAP - 1) n = HTML_URL_CAP - 1;
    for (uint32_t i = 0; i < n; i++) w->links->url[w->links->count][i] = url[i];
    w->links->url[w->links->count][n] = '\0';
    w->links->count++;
    return w->links->count;
}

/* Record an image's src and alt; return its 0-based index, or -1 when the table is full. */
static int note_image(writer_t *w, const char *src, const char *alt) {
    if (w->images == NULL || w->images->count >= HTML_MAX_IMAGES) return -1;
    uint32_t n = (uint32_t)strlen(src);
    if (n > HTML_URL_CAP - 1) n = HTML_URL_CAP - 1;
    for (uint32_t i = 0; i < n; i++) w->images->src[w->images->count][i] = src[i];
    w->images->src[w->images->count][n] = '\0';
    n = (uint32_t)strlen(alt);
    if (n > HTML_ALT_CAP - 1) n = HTML_ALT_CAP - 1;
    for (uint32_t i = 0; i < n; i++) w->images->alt[w->images->count][i] = alt[i];
    w->images->alt[w->images->count][n] = '\0';
    return (int)w->images->count++;
}

/* Write "[N] " in front of a link's words. */
static void put_link_mark(writer_t *w, uint32_t n) {
    if (n == 0) return;
    put_char(w, '[');
    char digits[8];
    uint32_t k = 0;
    do {
        digits[k++] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n != 0);
    while (k > 0) put_char(w, digits[--k]);
    put_char(w, ']');
    put_char(w, ' ');
}

/* The image line: "\x01<index>\x01 alt". The index is a byte the browser parses back. */
static void put_image_line(writer_t *w, int idx, const char *alt) {
    put_newline(w);
    put_char(w, (char)0x01);
    put_char(w, (char)('0' + idx));
    put_char(w, (char)0x01);
    if (alt != NULL && alt[0] != '\0') {
        put_char(w, ' ');
        for (const char *p = alt; *p != '\0'; p++) {
            uint8_t c = (uint8_t)*p;
            if (c >= 0x20 && c < 0x7f) put_char(w, (char)c);
        }
    }
    put_newline(w);
}

uint32_t html_to_text_ex(const uint8_t *in, uint32_t n, char *out, uint32_t cap,
                         html_links_t *links, html_images_t *images) {
    writer_t w = { out, cap, 0, false, 0, links, images };
    if (links != NULL) links->count = 0;
    if (images != NULL) images->count = 0;
    uint32_t i = 0;
    while (i < n) {
        uint8_t c = in[i];

        /* Comments: <!-- ... --> */
        if (c == '<' && i + 3 < n && in[i + 1] == '!' && in[i + 2] == '-' && in[i + 3] == '-') {
            i += 4;
            while (i + 2 < n && !(in[i] == '-' && in[i + 1] == '-' && in[i + 2] == '>')) i++;
            i = (i + 2 < n) ? i + 3 : n;
            continue;
        }

        /* Declarations such as <!DOCTYPE html> carry no text. */
        if (c == '<' && i + 1 < n && in[i + 1] == '!') {
            while (i < n && in[i] != '>') i++;
            i = (i < n) ? i + 1 : n;
            continue;
        }

        /* Tags: <name ...> or </name>. Anything else after '<' is ordinary text. */
        if (c == '<' && i + 1 < n && (in[i + 1] == '/' || is_alpha(in[i + 1]))) {
            bool closing = in[i + 1] == '/';
            uint32_t j = i + (closing ? 2u : 1u);
            char name[16];
            uint32_t k = 0;
            while (j < n && (is_alpha(in[j]) || is_digit(in[j]))) {
                if (k + 1 < sizeof name) name[k++] = lower(in[j]);
                j++;
            }
            name[k] = '\0';
            uint32_t attrs_from = j;
            while (j < n && in[j] != '>') j++;
            uint32_t attrs_to = j;
            i = (j < n) ? j + 1 : n;

            if (!closing && (streq(name, "script") || streq(name, "style"))) {
                /* Skip everything up to the matching close tag. */
                i = find_close(in, n, i, name);
                while (i < n && in[i] != '>') i++;
                i = (i < n) ? i + 1 : n;
                continue;
            }
            if (!closing && streq(name, "img")) {
                char src[HTML_URL_CAP];
                char alt[HTML_ALT_CAP];
                src[0] = '\0';
                alt[0] = '\0';
                attr_value(in, attrs_from, attrs_to, "src", src, sizeof src);
                attr_value(in, attrs_from, attrs_to, "alt", alt, sizeof alt);
                int idx = note_image(&w, src, alt);
                if (idx >= 0) put_image_line(&w, idx, alt);
                continue;
            }
            if (!closing && streq(name, "a")) {
                char href[HTML_URL_CAP];
                href[0] = '\0';
                attr_value(in, attrs_from, attrs_to, "href", href, sizeof href);
                uint32_t num = href[0] != '\0' ? note_link(&w, href) : 0;
                if (num != 0) {
                    put_newline(&w);
                    put_link_mark(&w, num);
                }
                continue;
            }
            if (is_block(name)) put_newline(&w);
            continue;
        }

        /* Entities: &name; or &#NN; */
        if (c == '&') {
            uint32_t e = i + 1;
            while (e < n && e - i < 10 && in[e] != ';' && in[e] != ' ') e++;
            if (e < n && in[e] == ';' && e > i + 1) {
                char ent[10];
                uint32_t k = 0;
                for (uint32_t p = i + 1; p < e; p++) ent[k++] = (char)in[p];
                ent[k] = '\0';
                char d = entity_char(ent);
                if (d != '\0') {
                    if (d == ' ') put_space(&w);
                    else put_char(&w, d);
                    i = e + 1;
                    continue;
                }
            }
            put_char(&w, '&');
            i++;
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            put_space(&w);
        } else if (c < 0x20) {
            /* other control bytes are dropped */
        } else if (c >= 0x7f) {
            put_char(&w, '?');
        } else {
            put_char(&w, (char)c);
        }
        i++;
    }
    out[w.len] = '\0';
    return w.len;
}

uint32_t html_to_text(const uint8_t *in, uint32_t n, char *out, uint32_t cap) {
    return html_to_text_ex(in, n, out, cap, NULL, NULL);
}
