/* editor.c — small shell editor backed by a volatile in-memory scratch file. */
#include "editor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "input.h"
#include "kstring.h"

#define EDITOR_CAPACITY 8192u
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

static void render(const char *buffer, size_t length, size_t cursor,
                   bool dirty, const char *message) {
    size_t cursor_line = line_number(buffer, cursor);
    size_t first_line = cursor_line >= EDITOR_VISIBLE_LINES
                      ? cursor_line - EDITOR_VISIBLE_LINES + 1 : 0;
    size_t last_line = first_line + EDITOR_VISIBLE_LINES;
    size_t source_line = 0;
    size_t index = 0;
    size_t total_lines = 1;
    for (size_t i = 0; i < length; i++) {
        if (buffer[i] == '\n') total_lines++;
    }

    while (source_line < first_line && index < length) {
        if (buffer[index++] == '\n') source_line++;
    }

    begin_row(0);
    console_set_fg(CONSOLE_BRIGHT_CYAN);
    console_write(" Helix Edit | scratch buffer");
    console_set_fg(dirty ? CONSOLE_BRIGHT_YELLOW : CONSOLE_BRIGHT_GREEN);
    console_write(dirty ? " [modified]\n" : " [saved in RAM]\n");
    console_reset_color();

    begin_row(1);
    console_write("----------------------------------------------------------------------------");

    for (size_t visible = 0; visible < EDITOR_VISIBLE_LINES; visible++) {
        begin_row(visible + 2);
        if (source_line >= first_line + visible && source_line < last_line &&
            source_line < total_lines) {
            size_t end = line_end(buffer, length, index);
            size_t column = 0;
            while (index < end && column < EDITOR_VISIBLE_COLUMNS) {
                if (index == cursor && column < EDITOR_VISIBLE_COLUMNS) {
                    console_set_fg(CONSOLE_BRIGHT_GREEN);
                    console_putc('|');
                    console_reset_color();
                    column++;
                }
                char c = buffer[index++];
                if (c == '\t') {
                    for (size_t spaces = 0; spaces < 4 && column < EDITOR_VISIBLE_COLUMNS; spaces++, column++)
                        console_putc(' ');
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
            index = end;
            if (index < length && buffer[index] == '\n') index++;
            source_line++;
        }
    }

    begin_row(EDITOR_VISIBLE_LINES + 2);
    console_write("----------------------------------------------------------------------------");
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
    console_write("Ctrl-S save to RAM   Ctrl-Q quit   Esc quit without saving");
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

void editor_run(void) {
    char buffer[EDITOR_CAPACITY];
    memcpy(buffer, g_scratch, g_scratch_len + 1);
    size_t length = g_scratch_len;
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

        if (key == 0x13) { /* Ctrl-S: persist for this boot session only. */
            memcpy(g_scratch, buffer, length + 1);
            g_scratch_len = length;
            dirty = false;
            message = "saved to volatile scratch";
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
            }
        } else if (key == KBD_KEY_DELETE) {
            if (cursor < length) {
                memmove(buffer + cursor, buffer + cursor + 1,
                        length - cursor);
                length--;
                dirty = true;
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
            } else {
                message = "buffer full";
            }
        }
        if (running) {
            render(buffer, length, cursor, dirty, message);
            message = "";
        }
    }

    console_clear();
    console_write("Returned to helixsh. Scratch contents are ");
    console_write(g_scratch_len == 0 ? "empty.\n" : "in RAM for this boot only.\n");
}
