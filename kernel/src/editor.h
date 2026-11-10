/* editor.h — the text buffer behind the Text Editor window.
 *
 * The text is one flat byte array with a cursor (a byte offset into it). An
 * insert or delete moves the bytes after the cursor, which is cheap for the few
 * KiB a note holds. Lines are not stored: a line is whatever sits between two
 * '\n' bytes, found by scanning. The file lives in the root directory as
 * note.txt and is read and written through the ext4 API. */
#ifndef HELIX_EDITOR_H
#define HELIX_EDITOR_H

#include <stdbool.h>
#include <stdint.h>

#define EDITOR_BYTES 8192u
#define EDITOR_NAME  "note.txt"

typedef enum { ED_LEFT, ED_RIGHT, ED_UP, ED_DOWN, ED_HOME, ED_END } editor_dir_t;

typedef struct {
    char text[EDITOR_BYTES];
    uint32_t len;       /* bytes in use */
    uint32_t pos;       /* cursor: 0..len, the byte before which typing goes */
    uint32_t top;       /* first line shown in the window */
    bool dirty;         /* changed since the last save */
    bool loaded;        /* the file has been read (once per session) */
    char msg[48];       /* status line text: "Saved", "Modified", an error */
} editor_t;

/* Read note.txt into the buffer the first time the window opens. A missing
 * file gives an empty buffer; the window keeps unsaved text across close and
 * reopen because the buffer is only read once. */
void editor_load_file(editor_t *e);

/* Write the buffer to note.txt (creating it, or replacing its contents).
 * Returns false when the disk is missing or the write fails. */
bool editor_save(editor_t *e);

/* Editing. Each returns false when nothing changed (the buffer is full, or
 * there was nothing to delete). */
bool editor_insert(editor_t *e, char c);
bool editor_backspace(editor_t *e);
bool editor_delete(editor_t *e);
bool editor_move(editor_t *e, editor_dir_t dir);

/* Lines. Line numbers start at 0. */
uint32_t editor_line_count(const editor_t *e);
uint32_t editor_offset_of_line(const editor_t *e, uint32_t line);
uint32_t editor_line_of(const editor_t *e, uint32_t pos);
uint32_t editor_line_end(const editor_t *e, uint32_t line_start);

/* Scroll so the cursor's line is inside a window showing `rows` lines. */
void editor_scroll_to_cursor(editor_t *e, uint32_t rows);

/* Put the cursor at (line, col), clamping col to the end of that line. */
void editor_goto(editor_t *e, uint32_t line, uint32_t col);

#endif
