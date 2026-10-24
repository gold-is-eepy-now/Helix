/* editor.c — small shell editor backed by a volatile in-memory scratch file. */
#include "editor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "fat32.h"
#include "hfs.h"
#include "input.h"
#include "kstring.h"

#define EDITOR_CAPACITY 8193u
#define EDITOR_VISIBLE_LINES 18u
#define EDITOR_VISIBLE_COLUMNS 78u

static char g_scratch[EDITOR_CAPACITY];
static size_t g_scratch_len;

static size_t line_start(const char *buffer, size_t cursor) {
    while (cursor > 0 && buffer[cursor - 1] != '\n') cursor--;
    return cursor;
}

static size_t line_end(const char *buffer, size_t length, size_t cursor) {
    while (cursor < length && buffer[cursor] != '\n') cursor++;
    return cursor;
}

static size_t line_number(const char *buffer, size_t cursor) {
    size_t line = 0;
    for (size_t i = 0; i < cursor; i++) {
        if (buffer[i] == '\n') line++;
    }
    return line;
}

static size_t minimum(size_t a, size_t b) { return a < b ? a : b; }

static void begin_row(size_t row) {
    console_cursor_to(row, 0);
    console_clear_current_line();
}

static size_t first_visible_line(const char *buffer, size_t cursor) {
    size_t current = line_number(buffer, cursor);
    return current >= EDITOR_VISIBLE_LINES
         ? current - EDITOR_VISIBLE_LINES + 1 : 0;
}

static size_t text_line_count(const char *buffer, size_t length) {
    size_t lines = 1;
    for (size_t i = 0; i < length; i++) if (buffer[i] == '\n') lines++;
    return lines;
}

static void render_header(bool dirty) {
    begin_row(0);
    console_set_fg(CONSOLE_BRIGHT_CYAN);
    console_write(" Helix Edit | file buffer");
    console_set_fg(dirty ? CONSOLE_BRIGHT_YELLOW : CONSOLE_BRIGHT_GREEN);
    console_write(dirty ? " [modified]\n" : " [saved in RAM]\n");
    console_reset_color();

}

static void render_text_line(const char *buffer, size_t length, size_t cursor,
                             size_t first_line, size_t visible_line) {
    size_t wanted_line = first_line + visible_line;
    size_t source_line = 0;
    size_t start = 0;
    while (start < length && source_line < wanted_line) {
        if (buffer[start++] == '\n') source_line++;
    }
    begin_row(visible_line + 2);
    if (source_line != wanted_line) return;

    size_t end = line_end(buffer, length, start);
    size_t column = 0;
    for (size_t i = start; i < end && column < EDITOR_VISIBLE_COLUMNS; i++) {
        if (i == cursor) {
            console_set_fg(CONSOLE_BRIGHT_GREEN);
            console_putc('|');
            console_reset_color();
            column++;
            if (column == EDITOR_VISIBLE_COLUMNS) break;
        }
        char c = buffer[i];
        if (c == '\t') {
            for (size_t spaces = 0; spaces < 4 && column < EDITOR_VISIBLE_COLUMNS;
                 spaces++, column++) console_putc(' ');
        } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) {
            console_putc(c);
            column++;
        }
    }
    if (cursor == end && column < EDITOR_VISIBLE_COLUMNS) {
        console_set_fg(CONSOLE_BRIGHT_GREEN);
        console_putc('|');
        console_reset_color();
    }
}

static void render_status(const char *buffer, size_t cursor, bool dirty,
                          const char *message) {
    size_t cursor_line = line_number(buffer, cursor);
    begin_row(EDITOR_VISIBLE_LINES + 3);
    console_write("Line ");
    console_write_dec(cursor_line + 1);
    console_write(", column ");
    console_write_dec(cursor - line_start(buffer, cursor) + 1);
    console_write(dirty ? " | modified" : " | saved in RAM");
    if (message != NULL) {
        console_write(" | ");
        console_write(message);
    }
    begin_row(EDITOR_VISIBLE_LINES + 4);
    console_write("Ctrl-S save to HFS path or new root file   Ctrl-Q quit");
}

static void render(const char *buffer, size_t length, size_t cursor,
                   bool dirty, const char *message) {
    size_t first_line = first_visible_line(buffer, cursor);
    render_header(dirty);
    begin_row(1);
    console_write("----------------------------------------------------------------------------");
    for (size_t visible = 0; visible < EDITOR_VISIBLE_LINES; visible++)
        render_text_line(buffer, length, cursor, first_line, visible);
    begin_row(EDITOR_VISIBLE_LINES + 2);
    console_write("----------------------------------------------------------------------------");
    render_status(buffer, cursor, dirty, message);
}

static void render_incremental(const char *buffer, size_t length, size_t cursor,
                               bool dirty, const char *message,
                               size_t old_line, size_t old_first,
                               size_t old_line_count,
                               bool text_changed, bool old_dirty) {
    size_t first_line = first_visible_line(buffer, cursor);
    size_t current_line = line_number(buffer, cursor);
    size_t current_line_count = text_line_count(buffer, length);
    if (first_line != old_first) {
        render(buffer, length, cursor, dirty, message);
        return;
    }
    if (dirty != old_dirty) render_header(dirty);

    if (text_changed && current_line_count != old_line_count) {
        size_t from = old_line < current_line ? old_line : current_line;
        if (from < first_line) from = first_line;
        for (size_t line = from; line < first_line + EDITOR_VISIBLE_LINES; line++)
            render_text_line(buffer, length, cursor, first_line, line - first_line);
    } else if (text_changed || old_line != current_line) {
        if (old_line >= first_line && old_line < first_line + EDITOR_VISIBLE_LINES)
            render_text_line(buffer, length, cursor, first_line, old_line - first_line);
        if (current_line >= first_line && current_line < first_line + EDITOR_VISIBLE_LINES &&
            current_line != old_line)
            render_text_line(buffer, length, cursor, first_line, current_line - first_line);
    }
    render_status(buffer, cursor, dirty, message);
}

static bool prompt_save_name(char name[13]) {
    size_t length = 0;
    begin_row(23);
    console_set_fg(CONSOLE_BRIGHT_CYAN);
    console_write("Save new file to usb0:/");
    console_reset_color();
    for (;;) {
        int key = input_getkey();
        if (key < 0) { __asm__ volatile("pause"); continue; }
        if (key == '\n' || key == '\r') {
            name[length] = '\0';
            begin_row(23);
            return length != 0;
        }
        if (key == 0x1b) { name[0] = '\0'; begin_row(23); return false; }
        if (key == 0x08 || key == 0x7f) {
            if (length != 0) {
                length--;
                name[length] = '\0';
                console_write("\b \b");
            }
            continue;
        }
        if (length < 12 && ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
            (key >= '0' && key <= '9') || key == '_' || key == '.')) {
            char c = (char)key;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            name[length++] = c;
            name[length] = '\0';
            console_putc(c);
        }
    }
}

static void move_vertical(const char *buffer, size_t length, size_t *cursor,
                          bool down) {
    size_t start = line_start(buffer, *cursor);
    size_t column = *cursor - start;
    size_t end = line_end(buffer, length, *cursor);
    if (down) {
        if (end == length) return;
        size_t next = end + 1;
        *cursor = minimum(next + column, line_end(buffer, length, next));
    } else {
        if (start == 0) return;
        size_t previous_end = start - 1;
        size_t previous_start = line_start(buffer, previous_end);
        *cursor = minimum(previous_start + column, previous_end);
    }
}

static bool editor_run_internal(const char *path) {
    char buffer[EDITOR_CAPACITY];
    size_t length = g_scratch_len;
    if (path != NULL) {
        unsigned long loaded = 0;
        if (!hfs_read_file(path, buffer, EDITOR_CAPACITY - 1u, &loaded)) {
            console_write("Helix Edit: could not load HFS path: ");
            console_write(path);
            console_putc('\n');
            return false;
        }
        length = (size_t)loaded;
        buffer[length] = '\0';
        memcpy(g_scratch, buffer, length + 1);
        g_scratch_len = length;
    } else {
        memcpy(buffer, g_scratch, g_scratch_len + 1);
    }
    size_t cursor = length;
    bool dirty = false;
    bool running = true;
    const char *message = "";

    console_clear();
    render(buffer, length, cursor, dirty, message);
    while (running) {
        int key = input_getkey();
        if (key < 0) {
            __asm__ volatile("pause");
            continue;
        }

        size_t old_line = line_number(buffer, cursor);
        size_t old_first = first_visible_line(buffer, cursor);
        size_t old_line_count = text_line_count(buffer, length);
        bool old_dirty = dirty;
        bool text_changed = false;

        if (key == 0x13) { /* Ctrl-S: save to HFS or FAT32. */
            char name[13] = {0};
            bool save_prompted = path != NULL || prompt_save_name(name);
            bool saved = false;
            if (save_prompted) {
                begin_row(23);
                console_write("Saving ");
                console_write(path != NULL ? path : name);
                console_write(" ...");
                if (path != NULL)
                    saved = hfs_write_file(path, buffer, length);
                else if (hfs_usb0_is_formatted())
                    saved = hfs_write_file(name, buffer, length);
                else
                    saved = fat32_save_usb0_file(name, buffer, length);
                begin_row(23);
            }
            if (saved) {
                memcpy(g_scratch, buffer, length + 1);
                g_scratch_len = length;
                dirty = false;
                message = path != NULL || hfs_usb0_is_formatted()
                    ? "saved file on HFS" : "saved file on FAT32";
            } else {
                    message = "save failed; check HFS path or FAT32 usb0";
            }
        } else if (key == 0x11) { /* Ctrl-Q */
            if (dirty) {
                message = "unsaved edits: Ctrl-S keeps them in RAM; Esc discards";
            } else {
                running = false;
            }
        } else if (key == 0x1B) { /* Escape: discard unsaved edits. */
            if (dirty) {
                memcpy(buffer, g_scratch, g_scratch_len + 1);
                length = g_scratch_len;
            }
            running = false;
        } else if (key == KBD_KEY_LEFT) {
            if (cursor > 0) cursor--;
        } else if (key == KBD_KEY_RIGHT) {
            if (cursor < length) cursor++;
        } else if (key == KBD_KEY_UP) {
            move_vertical(buffer, length, &cursor, false);
        } else if (key == KBD_KEY_DOWN) {
            move_vertical(buffer, length, &cursor, true);
        } else if (key == KBD_KEY_HOME) {
            cursor = line_start(buffer, cursor);
        } else if (key == KBD_KEY_END) {
            cursor = line_end(buffer, length, cursor);
        } else if (key == 0x7F || key == 0x08) {
            if (cursor > 0) {
                memmove(buffer + cursor - 1, buffer + cursor,
                        length - cursor + 1);
                cursor--;
                length--;
                dirty = true;
                text_changed = true;
            }
        } else if (key == KBD_KEY_DELETE) {
            if (cursor < length) {
                memmove(buffer + cursor, buffer + cursor + 1,
                        length - cursor);
                length--;
                dirty = true;
                text_changed = true;
            }
        } else if (key == '\n' || key == '\r' || key == '\t' ||
                   (key >= 0x20 && key < 0x7F)) {
            size_t insert_count = key == '\t' ? 4 : 1;
            if (length + insert_count < sizeof(buffer)) {
                memmove(buffer + cursor + insert_count, buffer + cursor,
                        length - cursor + 1);
                if (key == '\t') {
                    for (size_t i = 0; i < insert_count; i++)
                        buffer[cursor + i] = ' ';
                } else {
                    buffer[cursor] = (char)(key == '\r' ? '\n' : key);
                }
                cursor += insert_count;
                length += insert_count;
                dirty = true;
                text_changed = true;
            } else {
                message = "buffer full";
            }
        }
        if (running) {
            render_incremental(buffer, length, cursor, dirty, message,
                               old_line, old_first, old_line_count,
                               text_changed, old_dirty);
            message = "";
        }
    }

    console_clear();
    console_write("Returned to helixsh. Scratch contents are ");
    console_write(g_scratch_len == 0 ? "empty.\n" : "in RAM for this boot only.\n");
    return true;
}

void editor_run(void) { (void)editor_run_internal(NULL); }

bool editor_run_file(const char *path) {
    return editor_run_internal(path);
}
