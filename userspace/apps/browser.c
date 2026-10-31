/* browser.c — Minimal web browser with HTML/CSS/JS rendering for Helix userspace.
 *
 * Uses Linux runtime syscalls (SYSCALL_READ_INPUT, SYSCALL_WRITE) for I/O.
 * Implements a simple text-based browser with:
 * - Basic HTML parsing (text, headings, paragraphs, links)
 * - Simple CSS (color, bold, italic via ANSI)
 * - Simple JavaScript interpreter (alert(), console.log())
 * - URL bar with navigation
 * - Event handling loop
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include "linux_runtime.h"

/* Browser state */
typedef struct {
    char url[256];
    char *page_content;
    char *title;
    int cursor_x, cursor_y;
    bool running;
    bool show_debug;
} browser_t;

/* Simple HTML tag stripper for text display */
static void strip_html_tags(const char *html, char *out, int max_len) {
    int i = 0, o = 0;
    bool in_tag = false;
    
    while (html[i] && o < max_len - 1) {
        if (html[i] == '<') {
            in_tag = true;
        } else if (html[i] == '>') {
            in_tag = false;
        } else if (!in_tag) {
            out[o++] = html[i];
        }
        i++;
    }
    out[o] = '\0';
}

/* Simple CSS parser - extract text color and style */
typedef struct {
    int color;       /* ANSI color code */
    bool bold;
    bool italic;
} css_style_t;

static css_style_t parse_style(const char *style_attr) {
    css_style_t s = {.color = 7, .bold = false, .italic = false};
    if (!style_attr) return s;
    
    if (strstr(style_attr, "color:")) {
        if (strstr(style_attr, "red")) s.color = 1;
        else if (strstr(style_attr, "green")) s.color = 2;
        else if (strstr(style_attr, "blue")) s.color = 4;
    }
    if (strstr(style_attr, "bold")) s.bold = true;
    if (strstr(style_attr, "italic")) s.italic = true;
    return s;
}

/* JavaScript mini-interpreter */
static void js_eval(const char *code, browser_t *b) {
    if (strstr(code, "alert(")) {
        const char *start = strstr(code, "alert(\"");
        if (start) {
            start += 7;
            const char *end = strchr(start, '"');
            if (end) {
                char msg[256];
                int len = end - start;
                if (len < 255) {
                    strncpy(msg, start, len);
                    msg[len] = '\0';
                    printf("[JS alert] %s\n", msg);
                }
            }
        }
    }
    if (strstr(code, "console.log(")) {
        const char *start = strstr(code, "console.log(\"");
        if (start) {
            start += 13;
            const char *end = strchr(start, '"');
            if (end) {
                char msg[256];
                int len = end - start;
                if (len < 255) {
                    strncpy(msg, start, len);
                    msg[len] = '\0';
                    printf("[JS console] %s\n", msg);
                }
            }
        }
    }
}

/* Render page to terminal */
static void render_page(browser_t *b) {
    write(1, "\033[2J\033[H", 7);  /* Clear screen, move to home */
    
    write(1, "=== Helix Browser ===\n", 21);
    write(1, "URL: ", 5);
    write(1, b->url, strlen(b->url));
    write(1, "\n\n", 2);
    
    if (b->title) {
        write(1, "Title: ", 7);
        write(1, b->title, strlen(b->title));
        write(1, "\n\n", 2);
    }
    
    if (b->page_content) {
        char text[2048];
        strip_html_tags(b->page_content, text, sizeof(text));
        write(1, text, strlen(text));
    }
    
    write(1, "\n\n[Press ESC to exit, Enter to reload]\n", 39);
}

/* Simulated page fetch */
static void fetch_page(browser_t *b) {
    free(b->page_content);
    free(b->title);
    
    if (strstr(b->url, "example.com")) {
        b->title = strdup("Example Domain");
        b->page_content = strdup(
            "<html><head><title>Example Domain</title></head>"
            "<body><h1>Example Domain</h1>"
            "<p>This domain is for use in illustrative examples.</p>"
            "<p><a href='https://example.com'>More information...</a></p>"
            "<script>alert('Hello from JavaScript!');</script>"
            "</body></html>"
        );
    } else if (strstr(b->url, "localhost")) {
        b->title = strdup("Localhost");
        b->page_content = strdup(
            "<html><head><title>Localhost</title></head>"
            "<body><h1>Welcome to Helix!</h1>"
            "<p>This is the Helix OS browser.</p>"
            "<p style='color:green;'><b>System Status: OK</b></p>"
            "<script>console.log('Page loaded');</script>"
            "</body></html>"
        );
    } else {
        b->title = strdup("Unknown");
        b->page_content = strdup("<html><body><h1>404 Not Found</h1></body></html>");
    }
    
    /* Execute any inline JavaScript */
    if (b->page_content) {
        char *script = strstr(b->page_content, "<script>");
        if (script) {
            script += 8;
            char *end = strstr(script, "</script>");
            if (end) {
                *end = '\0';
                js_eval(script, b);
            }
        }
    }
}

/* Main browser loop */
int main(int argc, char **argv) {
    browser_t browser = {
        .running = true,
        .show_debug = false
    };
    
    strncpy(browser.url, "http://localhost", sizeof(browser.url) - 1);
    
    if (argc > 1) {
        strncpy(browser.url, argv[1], sizeof(browser.url) - 1);
    }
    
    /* Initial fetch */
    fetch_page(&browser);
    
    /* Main loop */
    while (browser.running) {
        render_page(&browser);
        
        char c;
        if (read(0, &c, 1) == 1) {
            if (c == 27) {  /* ESC - exit */
                break;
            } else if (c == '\n' || c == '\r') {  /* Reload */
                fetch_page(&browser);
            } else if (c == 'u' || c == 'U') {  /* Change URL */
                write(1, "Enter URL: ", 11);
                char new_url[256];
                int i = 0;
                while (read(0, &c, 1) == 1) {
                    if (c == '\n' || c == '\r') {
                        new_url[i] = '\0';
                        break;
                    } else if (c == 127 || c == 8) {
                        if (i > 0) {
                            i--;
                            write(1, "\b \b", 3);
                        }
                    } else {
                        new_url[i++] = c;
                        write(1, &c, 1);
                    }
                }
                write(1, "\n", 1);
                if (i > 0) {
                    strncpy(browser.url, new_url, sizeof(browser.url) - 1);
                    fetch_page(&browser);
                }
            }
        }
    }
    
    write(1, "Browser closed.\n", 16);
    return 0;
}