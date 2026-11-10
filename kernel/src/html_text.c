/* html_text.c — HTML to plain text (see html_text.h).
 *
 * One pass over the bytes. A '<' starts a tag, a comment, or (if nothing tag-like follows)
 * plain text. A '&' starts an entity if a ';' follows closely. Whitespace is collapsed by
 * the writer: a space is written only after visible text and never twice in a row, and at
 * most one blank line separates blocks. */
#include "html_text.h"

#include <stdbool.h>

typedef struct {
    char *out;
    uint32_t cap;
    uint32_t len;
    bool space;          /* the last byte written was a space */
    uint32_t newlines;   /* newlines written in a row */
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

uint32_t html_to_text(const uint8_t *in, uint32_t n, char *out, uint32_t cap) {
    writer_t w = { out, cap, 0, false, 0 };
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
            while (j < n && in[j] != '>') j++;           /* attributes are dropped */
            i = (j < n) ? j + 1 : n;

            if (!closing && (streq(name, "script") || streq(name, "style"))) {
                /* Skip everything up to the matching close tag. */
                i = find_close(in, n, i, name);
                while (i < n && in[i] != '>') i++;
                i = (i < n) ? i + 1 : n;
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
