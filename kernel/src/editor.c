/* editor.c — the text buffer behind the Text Editor window (see editor.h).
 *
 * Nothing here draws. The window code in desktop.c asks for the line and cursor
 * data it needs and paints it, so this file could also serve a line editor. */
#include "editor.h"

#include "ext4.h"

static void set_msg(editor_t *e, const char *s) {
    uint32_t i = 0;
    while (s[i] != '\0' && i + 1 < sizeof(e->msg)) {
        e->msg[i] = s[i];
        i++;
    }
    e->msg[i] = '\0';
}

static void mark_changed(editor_t *e) {
    e->dirty = true;
    set_msg(e, "Modified");
}

/* Start of the line that holds `pos`: scan back to just after the previous '\n'. */
static uint32_t line_start_of(const editor_t *e, uint32_t pos) {
    uint32_t i = pos;
    while (i > 0 && e->text[i - 1] != '\n') i--;
    return i;
}

void editor_load_file(editor_t *e) {
    if (e->loaded) return;
    e->loaded = true;
    e->len = 0;
    e->pos = 0;
    e->top = 0;
    e->dirty = false;

    ext4_fs_t *fs = ext4_get_current();
    if (!fs) {
        set_msg(e, "No disk: empty text");
        return;
    }
    ext4_stat_t st;
    if (!ext4_lookup(fs, e->targeted ? e->dir : ext4_root(fs), editor_title(e), &st)) {
        set_msg(e, "New file");
        return;
    }
    /* Leave room for the cursor's end-of-text position: len must stay below EDITOR_BYTES. */
    if (st.size >= EDITOR_BYTES) {
        set_msg(e, "File too big to open");
        return;
    }
    e->len = (uint32_t)ext4_read(fs, st.ino, 0, e->text, st.size);
    set_msg(e, "Loaded");
}

const char *editor_title(const editor_t *e) {
    return e->targeted ? e->file : EDITOR_NAME;
}

bool editor_open_file(editor_t *e, uint64_t dir, const char *name) {
    if (e->dirty) {
        set_msg(e, "Save the open file first");
        return false;
    }
    uint32_t k = 0;
    while (name[k] != '\0' && k + 1 < sizeof e->file) {
        e->file[k] = name[k];
        k++;
    }
    e->file[k] = '\0';
    e->targeted = true;
    e->dir = dir;
    e->loaded = false;
    editor_load_file(e);
    return true;
}

bool editor_save(editor_t *e) {
    ext4_fs_t *fs = ext4_get_current();
    if (!fs) {
        set_msg(e, "Not saved: no disk");
        return false;
    }
    /* Replace the file's contents: truncate to zero, then write the whole buffer. */
    uint64_t dir = e->targeted ? e->dir : ext4_root(fs);
    uint64_t ino;
    ext4_stat_t st;
    if (ext4_lookup(fs, dir, editor_title(e), &st)) {
        ino = st.ino;
        if (!ext4_truncate(fs, ino, 0)) {
            set_msg(e, "Not saved: truncate failed");
            return false;
        }
    } else if (!ext4_create(fs, dir, editor_title(e), EXT4_S_IFREG | 0644, &ino)) {
        set_msg(e, "Not saved: create failed");
        return false;
    }
    if (ext4_write(fs, ino, 0, e->text, e->len) != e->len) {
        set_msg(e, "Not saved: write failed");
        return false;
    }
    e->dirty = false;
    set_msg(e, "Saved");
    return true;
}

bool editor_insert(editor_t *e, char c) {
    if (e->len + 1 >= EDITOR_BYTES) {
        set_msg(e, "File is full");
        return false;
    }
    for (uint32_t i = e->len; i > e->pos; i--) e->text[i] = e->text[i - 1];
    e->text[e->pos] = c;
    e->len++;
    e->pos++;
    mark_changed(e);
    return true;
}

bool editor_backspace(editor_t *e) {
    if (e->pos == 0) return false;
    /* Remove the byte before the cursor by shifting the rest left one place. */
    for (uint32_t i = e->pos - 1; i + 1 < e->len; i++) e->text[i] = e->text[i + 1];
    e->pos--;
    e->len--;
    mark_changed(e);
    return true;
}

bool editor_delete(editor_t *e) {
    if (e->pos >= e->len) return false;
    for (uint32_t i = e->pos; i + 1 < e->len; i++) e->text[i] = e->text[i + 1];
    e->len--;
    mark_changed(e);
    return true;
}

uint32_t editor_line_end(const editor_t *e, uint32_t line_start) {
    uint32_t i = line_start;
    while (i < e->len && e->text[i] != '\n') i++;
    return i;
}

uint32_t editor_line_count(const editor_t *e) {
    uint32_t n = 1;
    for (uint32_t i = 0; i < e->len; i++) {
        if (e->text[i] == '\n') n++;
    }
    return n;
}

uint32_t editor_offset_of_line(const editor_t *e, uint32_t line) {
    uint32_t cur = 0, off = 0;
    for (uint32_t i = 0; i < e->len && cur < line; i++) {
        if (e->text[i] == '\n') {
            cur++;
            off = i + 1;
        }
    }
    return off;
}

uint32_t editor_line_of(const editor_t *e, uint32_t pos) {
    uint32_t line = 0;
    for (uint32_t i = 0; i < pos && i < e->len; i++) {
        if (e->text[i] == '\n') line++;
    }
    return line;
}

/* Cursor movement. Up and Down keep the column when the line is long enough,
 * and stop at the line's end otherwise. Returns true if the cursor moved. */
bool editor_move(editor_t *e, editor_dir_t dir) {
    uint32_t old = e->pos;
    uint32_t ls = line_start_of(e, e->pos);
    switch (dir) {
    case ED_LEFT:
        if (e->pos > 0) e->pos--;
        break;
    case ED_RIGHT:
        if (e->pos < e->len) e->pos++;
        break;
    case ED_HOME:
        e->pos = ls;
        break;
    case ED_END:
        e->pos = editor_line_end(e, ls);
        break;
    case ED_UP:
    case ED_DOWN: {
        uint32_t col = e->pos - ls;
        uint32_t line = editor_line_of(e, e->pos);
        if (dir == ED_UP) {
            if (line == 0) break;
            uint32_t pls = editor_offset_of_line(e, line - 1);
            uint32_t plen = editor_line_end(e, pls) - pls;
            e->pos = pls + (col < plen ? col : plen);
        } else {
            uint32_t end = editor_line_end(e, ls);
            if (end >= e->len) break;          /* already on the last line */
            uint32_t nls = end + 1;
            uint32_t nlen = editor_line_end(e, nls) - nls;
            e->pos = nls + (col < nlen ? col : nlen);
        }
        break;
    }
    }
    return e->pos != old;
}

void editor_scroll_to_cursor(editor_t *e, uint32_t rows) {
    if (rows == 0) return;
    uint32_t line = editor_line_of(e, e->pos);
    if (line < e->top) {
        e->top = line;
    } else if (line >= e->top + rows) {
        e->top = line - rows + 1;
    }
}

void editor_goto(editor_t *e, uint32_t line, uint32_t col) {
    uint32_t last = editor_line_count(e) - 1;
    if (line > last) line = last;
    uint32_t off = editor_offset_of_line(e, line);
    uint32_t len = editor_line_end(e, off) - off;
    e->pos = off + (col < len ? col : len);
}
