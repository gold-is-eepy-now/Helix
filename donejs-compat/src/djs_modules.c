/* djs_modules.c — the require() host layer and the builtin modules.
 *
 * The runtime ships a small Node-style module surface: require() of the
 * builtin modules below, plus the host objects the donejs port needs
 * (process, fs, timers, ...). Every module is created lazily by
 * djs_require() and cached in rt->module_registry, so the same exports
 * object comes back for every require() of the same name.
 *
 * The file builds twice. The hosted build uses POSIX file I/O; the
 * freestanding build (Helix) talks to the kernel through the small user
 * space API declared below and supports only the operations the kernel
 * actually implements.
 */

#include "djs_internal.h"

#if defined(DJS_FREESTANDING)

/* Helix user space API (the ring-3 program links libhelix.c). Declared
 * here so this file stays free of libc headers. */
int helix_open(const char *path, int flags);
int64_t helix_read(int fd, void *buf, size_t count);
int64_t helix_write(int fd, const void *buf, size_t count);
int helix_close(int fd);
int64_t helix_lseek(int fd, int64_t offset, int whence);

#define HELIX_O_RDONLY 0x0
#define HELIX_O_WRONLY 0x1
#define HELIX_O_CREAT  0x40
#define HELIX_O_TRUNC  0x200
#define HELIX_SEEK_SET 0
#define HELIX_SEEK_CUR 1
#define HELIX_SEEK_END 2

#else

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#endif

/* ------------------------------------------------------------------ buffers */

/* A growable text buffer. The bytes live in the runtime arena, so a buffer
 * that grows leaves the earlier blocks behind; that is fine for the short
 * strings the modules produce. */
typedef struct {
    djs_runtime_t *rt;
    char *data;
    size_t length;
    size_t capacity;
} djs_buf_t;

static void buf_init(djs_buf_t *buf, djs_runtime_t *rt) {
    buf->rt = rt;
    buf->data = NULL;
    buf->length = 0;
    buf->capacity = 0;
}

static void buf_reserve(djs_buf_t *buf, size_t extra) {
    size_t needed = buf->length + extra + 1;
    if (needed <= buf->capacity) return;
    size_t capacity = buf->capacity == 0 ? 64 : buf->capacity;
    while (capacity < needed) capacity *= 2;
    char *data = (char *)djs_arena_alloc(buf->rt, capacity);
    if (data == NULL) return;
    if (buf->length > 0) djs_memcpy(data, buf->data, buf->length);
    buf->data = data;
    buf->capacity = capacity;
}

static void buf_append(djs_buf_t *buf, const char *text, size_t length) {
    buf_reserve(buf, length);
    if (buf->data == NULL) return;
    djs_memcpy(buf->data + buf->length, text, length);
    buf->length += length;
    buf->data[buf->length] = '\0';
}

static void buf_append_cstr(djs_buf_t *buf, const char *text) {
    if (text == NULL) return;
    buf_append(buf, text, djs_strlen(text));
}

static void buf_append_char(djs_buf_t *buf, char c) {
    buf_append(buf, &c, 1);
}

static const char *buf_text(djs_buf_t *buf) {
    if (buf->data == NULL) return "";
    buf->data[buf->length] = '\0';
    return buf->data;
}

/* ------------------------------------------------------------------- values */

static djs_value_t str_value(djs_runtime_t *rt, const char *text) {
    return djs_string_value(djs_string_new_cstr(rt, text ? text : ""));
}

static djs_value_t num_value(double number) {
    return djs_number(number);
}

static djs_value_t bool_value(bool value) {
    return djs_bool(value);
}

/* The argument at `index`, or an empty string. */
static const char *arg_cstr(djs_runtime_t *rt, djs_value_t *args, size_t arg_count,
                            size_t index) {
    if (args == NULL || index >= arg_count) return "";
    return djs_value_cstr(rt, args[index]);
}

/* Whether `haystack` contains `needle`. */
static bool text_contains(const char *haystack, const char *needle) {
    if (haystack == NULL || needle == NULL) return false;
    if (needle[0] == '\0') return true;
    size_t needle_length = djs_strlen(needle);
    size_t haystack_length = djs_strlen(haystack);
    if (needle_length > haystack_length) return false;
    for (size_t i = 0; i + needle_length <= haystack_length; i++) {
        if (djs_memcmp_helper(haystack + i, needle, needle_length) == 0) return true;
    }
    return false;
}

/* ---------------------------------------------------------------- path core */

typedef struct {
    bool win32;
    char sep;
} djs_path_style_t;

static const djs_path_style_t DJS_PATH_POSIX = { false, '/' };
static const djs_path_style_t DJS_PATH_WIN32 = { true, '\\' };

static bool path_is_sep(const djs_path_style_t *style, char c) {
    if (c == style->sep) return true;
    return style->win32 && c == '/';
}

static bool path_is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Split `input` into its root prefix and the remaining body. POSIX roots
 * are "/", win32 roots are a drive ("C:", "C:\\") or a leading separator. */
static void path_split_root(const djs_path_style_t *style, const char *input,
                            char *root, size_t root_size, const char **body_out) {
    size_t length = djs_strlen(input);
    size_t root_length = 0;
    size_t index = 0;

    if (style->win32 && length >= 2 && path_is_letter(input[0]) && input[1] == ':') {
        root[root_length++] = input[0];
        root[root_length++] = ':';
        index = 2;
        if (index < length && path_is_sep(style, input[index])) {
            root[root_length++] = style->sep;
            index++;
        }
    } else if (length > 0 && path_is_sep(style, input[0])) {
        root[root_length++] = style->sep;
        index = 1;
    }
    root[root_length] = '\0';
    *body_out = input + index;
}

static bool path_is_absolute(const djs_path_style_t *style, const char *path) {
    if (path == NULL || path[0] == '\0') return false;
    if (!style->win32) return path[0] == '/';
    if (path_is_sep(style, path[0])) return true;
    if (path_is_letter(path[0]) && path[1] == ':') {
        return path[2] == '\0' || path_is_sep(style, path[2]);
    }
    return false;
}

/* Collapse separators and resolve "." and "..". */
static char *path_normalize(djs_runtime_t *rt, const djs_path_style_t *style,
                            const char *input) {
    char root[8];
    const char *body = NULL;
    path_split_root(style, input, root, sizeof(root), &body);

    size_t body_length = djs_strlen(body);
    bool trailing = body_length > 0 && path_is_sep(style, body[body_length - 1]);

    size_t capacity = body_length / 2 + 2;
    char **segments = (char **)djs_arena_calloc(rt, capacity, sizeof(char *));
    if (segments == NULL) return djs_arena_strdup(rt, input);
    size_t count = 0;

    size_t index = 0;
    while (index < body_length) {
        if (path_is_sep(style, body[index])) {
            index++;
            continue;
        }
        size_t start = index;
        while (index < body_length && !path_is_sep(style, body[index])) index++;
        size_t length = index - start;
        const char *segment = body + start;
        if (length == 0 || (length == 1 && segment[0] == '.')) continue;
        if (length == 2 && segment[0] == '.' && segment[1] == '.') {
            bool last_is_dotdot = count > 0 && djs_strlen(segments[count - 1]) == 2 &&
                                  segments[count - 1][0] == '.' && segments[count - 1][1] == '.';
            if (count > 0 && !last_is_dotdot) {
                count--;
                continue;
            }
            if (root[0] != '\0') continue; /* ".." above the root vanishes */
        }
        if (count >= capacity) break;
        char *copy = djs_arena_strndup(rt, segment, length);
        if (copy == NULL) break;
        segments[count++] = copy;
    }

    djs_buf_t buf;
    buf_init(&buf, rt);
    buf_append_cstr(&buf, root);
    bool root_ends_sep = root[0] != '\0' && path_is_sep(style, root[djs_strlen(root) - 1]);
    bool drive_relative = style->win32 && root[0] != '\0' && root[1] == ':' && !root_ends_sep;
    for (size_t i = 0; i < count; i++) {
        if (i > 0 || (root[0] != '\0' && !root_ends_sep && !drive_relative)) {
            buf_append_char(&buf, style->sep);
        }
        buf_append_cstr(&buf, segments[i]);
    }

    if (count == 0 && root[0] == '\0') return djs_arena_strdup(rt, ".");
    if (count == 0 && root_ends_sep) return djs_arena_strdup(rt, root);
    if (trailing) buf_append_char(&buf, style->sep);
    return djs_arena_strdup(rt, buf_text(&buf));
}

static char *path_join(djs_runtime_t *rt, const djs_path_style_t *style,
                       const char **parts, size_t count) {
    djs_buf_t buf;
    buf_init(&buf, rt);
    bool any = false;
    for (size_t i = 0; i < count; i++) {
        if (parts[i] == NULL || parts[i][0] == '\0') continue;
        if (any) buf_append_char(&buf, style->sep);
        buf_append_cstr(&buf, parts[i]);
        any = true;
    }
    if (!any) return djs_arena_strdup(rt, ".");
    return path_normalize(rt, style, buf_text(&buf));
}

static char *path_resolve(djs_runtime_t *rt, const djs_path_style_t *style,
                          const char **parts, size_t count) {
    size_t start = count;
    for (size_t i = 0; i < count; i++) {
        if (parts[i] != NULL && parts[i][0] != '\0' && path_is_absolute(style, parts[i])) {
            start = i;
        }
    }

    djs_buf_t buf;
    buf_init(&buf, rt);
    bool any = false;
    for (size_t i = start; i < count; i++) {
        if (parts[i] == NULL || parts[i][0] == '\0') continue;
        if (any) buf_append_char(&buf, style->sep);
        buf_append_cstr(&buf, parts[i]);
        any = true;
    }

    if (start == count) {
        /* Nothing absolute: anchor the result at the working directory. */
        djs_buf_t full;
        buf_init(&full, rt);
        buf_append_cstr(&full, rt->cwd ? rt->cwd : "/");
        if (any) {
            buf_append_char(&full, style->sep);
            buf_append(&full, buf_text(&buf), buf.length);
        }
        return path_normalize(rt, style, buf_text(&full));
    }
    return path_normalize(rt, style, buf_text(&buf));
}

static char *path_dirname(djs_runtime_t *rt, const djs_path_style_t *style,
                           const char *path) {
    size_t length = djs_strlen(path);
    size_t end = length;
    while (end > 0 && path_is_sep(style, path[end - 1])) end--;
    if (end == 0) return djs_arena_strdup(rt, style->win32 ? "\\" : "/");
    if (style->win32 && end == 2 && path_is_letter(path[0]) && path[1] == ':') {
        djs_buf_t buf;
        buf_init(&buf, rt);
        buf_append(&buf, path, 2);
        buf_append_char(&buf, style->sep);
        return djs_arena_strdup(rt, buf_text(&buf));
    }
    size_t index = end;
    while (index > 0 && !path_is_sep(style, path[index - 1])) index--;
    if (index == 0) return djs_arena_strdup(rt, ".");
    size_t dir_end = index - 1;
    while (dir_end > 0 && path_is_sep(style, path[dir_end - 1])) dir_end--;
    if (dir_end == 0) return djs_arena_strdup(rt, style->win32 ? "\\" : "/");
    return djs_arena_strndup(rt, path, dir_end);
}

static char *path_basename(djs_runtime_t *rt, const djs_path_style_t *style,
                            const char *path, const char *ext) {
    size_t length = djs_strlen(path);
    size_t end = length;
    while (end > 0 && path_is_sep(style, path[end - 1])) end--;
    size_t start = end;
    while (start > 0 && !path_is_sep(style, path[start - 1])) start--;
    if (end == start) return djs_arena_strdup(rt, "");
    char *base = djs_arena_strndup(rt, path + start, end - start);
    if (base == NULL || ext == NULL || ext[0] == '\0') return base;
    size_t base_length = djs_strlen(base);
    size_t ext_length = djs_strlen(ext);
    if (base_length > ext_length &&
        djs_memcmp_helper(base + base_length - ext_length, ext, ext_length) == 0) {
        return djs_arena_strndup(rt, base, base_length - ext_length);
    }
    return base;
}

static char *path_extname(djs_runtime_t *rt, const djs_path_style_t *style,
                          const char *path) {
    size_t length = djs_strlen(path);
    size_t end = length;
    while (end > 0 && path_is_sep(style, path[end - 1])) end--;
    size_t start = end;
    while (start > 0 && !path_is_sep(style, path[start - 1])) start--;
    if (end == start) return djs_arena_strdup(rt, "");
    size_t dot = 0;
    bool found = false;
    for (size_t i = start; i < end; i++) {
        if (path[i] == '.') {
            dot = i;
            found = true;
        }
    }
    if (!found || dot == start) return djs_arena_strdup(rt, "");
    return djs_arena_strdup(rt, path + dot);
}

/* Split a path into its non-empty segments (the root is dropped). */
static size_t path_segments(djs_runtime_t *rt, const djs_path_style_t *style,
                            const char *path, char ***out) {
    char root[8];
    const char *body = NULL;
    path_split_root(style, path, root, sizeof(root), &body);
    size_t length = djs_strlen(body);
    char **segments = (char **)djs_arena_calloc(rt, length + 2, sizeof(char *));
    if (segments == NULL) {
        *out = NULL;
        return 0;
    }
    size_t count = 0;
    size_t index = 0;
    while (index < length) {
        while (index < length && path_is_sep(style, body[index])) index++;
        size_t start = index;
        while (index < length && !path_is_sep(style, body[index])) index++;
        if (index > start) {
            char *copy = djs_arena_strndup(rt, body + start, index - start);
            if (copy == NULL) break;
            segments[count++] = copy;
        }
    }
    *out = segments;
    return count;
}

static char *path_relative(djs_runtime_t *rt, const djs_path_style_t *style,
                           const char *from, const char *to) {
    const char *from_parts[1];
    const char *to_parts[1];
    from_parts[0] = from;
    to_parts[0] = to;
    char *from_abs = path_resolve(rt, style, from_parts, 1);
    char *to_abs = path_resolve(rt, style, to_parts, 1);

    char **from_segments = NULL;
    char **to_segments = NULL;
    size_t from_count = path_segments(rt, style, from_abs, &from_segments);
    size_t to_count = path_segments(rt, style, to_abs, &to_segments);

    size_t common = 0;
    while (common < from_count && common < to_count &&
           djs_strcmp(from_segments[common], to_segments[common]) == 0) {
        common++;
    }

    djs_buf_t buf;
    buf_init(&buf, rt);
    for (size_t i = common; i < from_count; i++) {
        if (buf.length > 0) buf_append_char(&buf, style->sep);
        buf_append_cstr(&buf, "..");
    }
    for (size_t i = common; i < to_count; i++) {
        if (buf.length > 0) buf_append_char(&buf, style->sep);
        buf_append_cstr(&buf, to_segments[i]);
    }
    if (buf.length == 0) return djs_arena_strdup(rt, ".");
    return djs_arena_strdup(rt, buf_text(&buf));
}

/* The style of the object a path method was called on: the win32 flavour
 * carries a backslash separator. */
static const djs_path_style_t *path_style_for(djs_runtime_t *rt, djs_value_t this_value) {
    if (this_value.tag == DJS_TAG_OBJECT && this_value.as.object != NULL) {
        djs_value_t sep = djs_object_get(rt, this_value.as.object, "sep");
        if (sep.tag == DJS_TAG_STRING && djs_strcmp(sep.as.string->bytes, "\\") == 0) {
            return &DJS_PATH_WIN32;
        }
    }
    return &DJS_PATH_POSIX;
}

/* ------------------------------------------------------------- path module */

static const char **arg_strings(djs_runtime_t *rt, djs_value_t *args, size_t arg_count) {
    const char **parts = (const char **)djs_arena_calloc(rt, arg_count + 1, sizeof(char *));
    if (parts == NULL) return NULL;
    for (size_t i = 0; i < arg_count; i++) parts[i] = djs_value_cstr(rt, args[i]);
    return parts;
}

static djs_value_t path_join_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    const djs_path_style_t *style = path_style_for(rt, this_value);
    const char **parts = arg_strings(rt, args, arg_count);
    if (parts == NULL) return str_value(rt, ".");
    return str_value(rt, path_join(rt, style, parts, arg_count));
}

static djs_value_t path_resolve_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    const djs_path_style_t *style = path_style_for(rt, this_value);
    const char **parts = arg_strings(rt, args, arg_count);
    if (parts == NULL) return str_value(rt, "/");
    return str_value(rt, path_resolve(rt, style, parts, arg_count));
}

static djs_value_t path_normalize_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    const djs_path_style_t *style = path_style_for(rt, this_value);
    return str_value(rt, path_normalize(rt, style, arg_cstr(rt, args, arg_count, 0)));
}

static djs_value_t path_dirname_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    const djs_path_style_t *style = path_style_for(rt, this_value);
    return str_value(rt, path_dirname(rt, style, arg_cstr(rt, args, arg_count, 0)));
}

static djs_value_t path_basename_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    const djs_path_style_t *style = path_style_for(rt, this_value);
    const char *ext = arg_count > 1 ? arg_cstr(rt, args, arg_count, 1) : NULL;
    return str_value(rt, path_basename(rt, style, arg_cstr(rt, args, arg_count, 0), ext));
}

static djs_value_t path_extname_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    const djs_path_style_t *style = path_style_for(rt, this_value);
    return str_value(rt, path_extname(rt, style, arg_cstr(rt, args, arg_count, 0)));
}

static djs_value_t path_relative_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    const djs_path_style_t *style = path_style_for(rt, this_value);
    return str_value(rt, path_relative(rt, style, arg_cstr(rt, args, arg_count, 0),
                                       arg_cstr(rt, args, arg_count, 1)));
}

static djs_value_t path_is_absolute_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    const djs_path_style_t *style = path_style_for(rt, this_value);
    return bool_value(path_is_absolute(style, arg_cstr(rt, args, arg_count, 0)));
}

static djs_value_t path_parse_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    const djs_path_style_t *style = path_style_for(rt, this_value);
    const char *input = arg_cstr(rt, args, arg_count, 0);

    char root[8];
    const char *body = NULL;
    path_split_root(style, input, root, sizeof(root), &body);

    djs_object_t *object = djs_object_new(rt, rt->object_prototype);
    if (object == NULL) return DJS_UNDEFINED;

    djs_property_set(rt, object, "root", str_value(rt, root), 0);
    djs_property_set(rt, object, "dir",
                     str_value(rt, path_dirname(rt, style, input)), 0);
    char *base = path_basename(rt, style, input, NULL);
    char *ext = path_extname(rt, style, input);
    djs_property_set(rt, object, "base", str_value(rt, base), 0);
    djs_property_set(rt, object, "ext", str_value(rt, ext), 0);

    size_t base_length = djs_strlen(base);
    size_t ext_length = djs_strlen(ext);
    if (ext_length > 0 && base_length > ext_length &&
        djs_memcmp_helper(base + base_length - ext_length, ext, ext_length) == 0) {
        base_length -= ext_length;
    }
    djs_property_set(rt, object, "name",
                     str_value(rt, djs_arena_strndup(rt, base, base_length)), 0);
    return djs_object_value(object);
}

static void path_install_methods(djs_runtime_t *rt, djs_object_t *target) {
    djs_define_method(rt, target, "join", path_join_fn, 2);
    djs_define_method(rt, target, "resolve", path_resolve_fn, 2);
    djs_define_method(rt, target, "normalize", path_normalize_fn, 1);
    djs_define_method(rt, target, "dirname", path_dirname_fn, 1);
    djs_define_method(rt, target, "basename", path_basename_fn, 2);
    djs_define_method(rt, target, "extname", path_extname_fn, 1);
    djs_define_method(rt, target, "parse", path_parse_fn, 1);
    djs_define_method(rt, target, "relative", path_relative_fn, 2);
    djs_define_method(rt, target, "isAbsolute", path_is_absolute_fn, 1);
}

static void path_install_flavour(djs_runtime_t *rt, djs_object_t *target, bool win32) {
    path_install_methods(rt, target);
    djs_define_value(rt, target, "sep", str_value(rt, win32 ? "\\" : "/"));
    djs_define_value(rt, target, "delimiter", str_value(rt, win32 ? ";" : ":"));
}

static djs_object_t *path_module_new(djs_runtime_t *rt) {
    djs_object_t *module = djs_object_new(rt, rt->object_prototype);
    if (module == NULL) return NULL;
    path_install_flavour(rt, module, false);

    djs_object_t *posix = djs_object_new(rt, rt->object_prototype);
    if (posix != NULL) {
        path_install_flavour(rt, posix, false);
        djs_property_set(rt, module, "posix", djs_object_value(posix), 0);
    }
    djs_object_t *win32 = djs_object_new(rt, rt->object_prototype);
    if (win32 != NULL) {
        path_install_flavour(rt, win32, true);
        djs_property_set(rt, module, "win32", djs_object_value(win32), 0);
    }
    return module;
}

/* --------------------------------------------------------------- os module */

static djs_value_t os_platform_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    return str_value(rt, rt->platform ? rt->platform : "helix");
}

static djs_value_t os_arch_fn(djs_runtime_t *rt, djs_value_t this_value,
                              djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    return str_value(rt, rt->arch ? rt->arch : "x64");
}

static djs_value_t os_hostname_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    return str_value(rt, "helix");
}

static djs_value_t os_homedir_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    return str_value(rt, "/");
}

static djs_value_t os_tmpdir_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    return str_value(rt, "/tmp");
}

static djs_value_t os_release_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    return str_value(rt, rt->version ? rt->version : "0.1.0-helix");
}

static djs_value_t os_endianness_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    return str_value(rt, "LE");
}

static djs_value_t os_type_fn(djs_runtime_t *rt, djs_value_t this_value,
                              djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    return str_value(rt, "Helix OS");
}

static djs_object_t *os_module_new(djs_runtime_t *rt) {
    djs_object_t *module = djs_object_new(rt, rt->object_prototype);
    if (module == NULL) return NULL;
    djs_define_method(rt, module, "platform", os_platform_fn, 0);
    djs_define_method(rt, module, "arch", os_arch_fn, 0);
    djs_define_method(rt, module, "hostname", os_hostname_fn, 0);
    djs_define_method(rt, module, "homedir", os_homedir_fn, 0);
    djs_define_method(rt, module, "tmpdir", os_tmpdir_fn, 0);
    djs_define_method(rt, module, "release", os_release_fn, 0);
    djs_define_method(rt, module, "endianness", os_endianness_fn, 0);
    djs_define_method(rt, module, "type", os_type_fn, 0);
    djs_define_value(rt, module, "EOL", str_value(rt, "\n"));
    return module;
}

/* ---------------------------------------------------------- process module */

static djs_value_t process_cwd_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    return str_value(rt, rt->cwd ? rt->cwd : "/");
}

static djs_value_t process_chdir_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *path = arg_cstr(rt, args, arg_count, 0);
    if (path[0] == '\0') djs_throw_error(rt, "Error", "path argument is required");
    rt->cwd = djs_arena_strdup(rt, path);
    return DJS_UNDEFINED;
}

static djs_value_t process_exit_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)this_value;
    int code = 0;
    if (arg_count > 0 && djs_is_number(args[0])) code = (int)args[0].as.number;
    rt->exit_status = code;
    rt->has_exit_code = true;
    rt->exiting = true;
    /* The NULL buffer is the runtime's "stop the script" path. */
    djs_longjmp(NULL, 1);
}

static djs_value_t process_next_tick_fn(djs_runtime_t *rt, djs_value_t this_value,
                                        djs_value_t *args, size_t arg_count) {
    (void)this_value;
    if (arg_count == 0) djs_throw_type_error(rt, "callback is required");
    djs_queue_tick(rt, args[0], arg_count > 1 ? args + 1 : NULL,
                   arg_count > 1 ? arg_count - 1 : 0);
    return DJS_UNDEFINED;
}

static djs_value_t process_hrtime_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    double seconds = djs_host_time_ms() / 1000.0;
    double whole = seconds < 0.0 ? 0.0 : (double)(long long)seconds;
    djs_value_t items[2];
    items[0] = num_value(whole);
    items[1] = num_value((seconds - whole) * 1e9);
    return djs_make_array(rt, items, 2);
}

static djs_value_t process_uptime_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    return num_value(djs_host_time_ms() - rt->start_time_ms);
}

static djs_value_t process_memory_usage_fn(djs_runtime_t *rt, djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    size_t total = 0;
    for (djs_arena_block_t *block = rt->arena.first; block != NULL; block = block->next) {
        total += block->capacity;
    }
    djs_object_t *object = djs_object_new(rt, rt->object_prototype);
    if (object == NULL) return DJS_UNDEFINED;
    djs_define_value(rt, object, "rss", num_value((double)total));
    djs_define_value(rt, object, "total", num_value((double)total));
    return djs_object_value(object);
}

static djs_value_t process_umask_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    return num_value(18.0);
}

static djs_value_t process_kill_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    djs_throw_error(rt, "Error", "kill is not supported");
}

static djs_value_t process_emit_warning_fn(djs_runtime_t *rt, djs_value_t this_value,
                                            djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *message = arg_cstr(rt, args, arg_count, 0);
    djs_write_cstr(rt, message);
    djs_write_cstr(rt, "\n");
    return DJS_UNDEFINED;
}

static djs_value_t process_exit_code_getter(djs_runtime_t *rt, djs_value_t this_value,
                                            djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    return num_value((double)rt->exit_status);
}

static djs_value_t stream_write_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_write_cstr(rt, arg_cstr(rt, args, arg_count, 0));
    return DJS_UNDEFINED;
}

static djs_value_t stdin_write_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)rt; (void)this_value; (void)args; (void)arg_count;
    return DJS_UNDEFINED;
}

static djs_object_t *process_module_new(djs_runtime_t *rt) {
    djs_object_t *module = djs_object_new(rt, rt->object_prototype);
    if (module == NULL) return NULL;

    const char *exec_path = rt->exec_path ? rt->exec_path : "donejs";
    djs_value_t argv[1];
    argv[0] = str_value(rt, exec_path);
    djs_define_value(rt, module, "argv", djs_make_array(rt, argv, 1));
    djs_define_value(rt, module, "argv0", str_value(rt, exec_path));
    djs_define_value(rt, module, "execPath", str_value(rt, exec_path));
    djs_define_value(rt, module, "title", str_value(rt, "donejs"));
    djs_define_value(rt, module, "platform", str_value(rt, rt->platform ? rt->platform : "helix"));
    djs_define_value(rt, module, "arch", str_value(rt, rt->arch ? rt->arch : "x64"));
    djs_define_value(rt, module, "version", str_value(rt, "v22.0.0-donejs"));

    djs_object_t *versions = djs_object_new(rt, rt->object_prototype);
    if (versions != NULL) {
        djs_define_value(rt, versions, "node", str_value(rt, "22.0.0-donejs"));
        djs_define_value(rt, versions, "donejs", str_value(rt, "22.0.0-donejs"));
        djs_define_value(rt, versions, "helix",
                         str_value(rt, rt->version ? rt->version : "0.1.0-helix"));
        djs_define_value(rt, module, "versions", djs_object_value(versions));
    }

#if defined(DJS_FREESTANDING)
    djs_define_value(rt, module, "pid", num_value(1.0));
#else
    djs_define_value(rt, module, "pid", num_value((double)getpid()));
#endif
    djs_define_value(rt, module, "ppid", num_value(0.0));
    djs_define_value(rt, module, "env", djs_object_value(djs_object_new(rt, rt->object_prototype)));

    djs_define_accessor(rt, module, "exitCode", process_exit_code_getter, NULL);

    djs_define_method(rt, module, "cwd", process_cwd_fn, 0);
    djs_define_method(rt, module, "chdir", process_chdir_fn, 1);
    djs_define_method(rt, module, "exit", process_exit_fn, 1);
    djs_define_method(rt, module, "nextTick", process_next_tick_fn, 1);
    djs_define_method(rt, module, "hrtime", process_hrtime_fn, 0);
    djs_define_method(rt, module, "uptime", process_uptime_fn, 0);
    djs_define_method(rt, module, "memoryUsage", process_memory_usage_fn, 0);
    djs_define_method(rt, module, "umask", process_umask_fn, 1);
    djs_define_method(rt, module, "kill", process_kill_fn, 2);
    djs_define_method(rt, module, "emitWarning", process_emit_warning_fn, 1);

    djs_object_t *stdout = djs_object_new(rt, rt->object_prototype);
    djs_object_t *stderr = djs_object_new(rt, rt->object_prototype);
    djs_object_t *stdin = djs_object_new(rt, rt->object_prototype);
    if (stdout != NULL) {
        djs_define_method(rt, stdout, "write", stream_write_fn, 1);
        djs_define_value(rt, module, "stdout", djs_object_value(stdout));
    }
    if (stderr != NULL) {
        djs_define_method(rt, stderr, "write", stream_write_fn, 1);
        djs_define_value(rt, module, "stderr", djs_object_value(stderr));
    }
    if (stdin != NULL) {
        djs_define_method(rt, stdin, "write", stdin_write_fn, 1);
        djs_define_value(rt, module, "stdin", djs_object_value(stdin));
    }
    return module;
}

/* -------------------------------------------------------------- util module */

static const char DJS_HEX_DIGITS[] = "0123456789abcdef";

static void util_escape_string(djs_buf_t *buf, const char *text, char quote) {
    buf_append_char(buf, quote);
    for (size_t i = 0; text[i] != '\0'; i++) {
        char c = text[i];
        switch (c) {
        case '\n': buf_append_cstr(buf, "\\n"); break;
        case '\t': buf_append_cstr(buf, "\\t"); break;
        case '\r': buf_append_cstr(buf, "\\r"); break;
        case '\b': buf_append_cstr(buf, "\\b"); break;
        case '\f': buf_append_cstr(buf, "\\f"); break;
        case '\\': buf_append_cstr(buf, "\\\\"); break;
        default:
            if (c == quote) {
                buf_append_char(buf, '\\');
                buf_append_char(buf, c);
            } else if ((unsigned char)c < 0x20) {
                buf_append_cstr(buf, "\\x");
                buf_append_char(buf, DJS_HEX_DIGITS[(unsigned char)c >> 4]);
                buf_append_char(buf, DJS_HEX_DIGITS[(unsigned char)c & 0x0F]);
            } else {
                buf_append_char(buf, c);
            }
        }
    }
    buf_append_char(buf, quote);
}

/* A JSON-ish rendering: enough for util.format('%j'). */
static void util_json_value(djs_runtime_t *rt, djs_buf_t *buf, djs_value_t value,
                            int depth) {
    switch (value.tag) {
    case DJS_TAG_UNDEFINED:
        buf_append_cstr(buf, "undefined");
        return;
    case DJS_TAG_NULL:
        buf_append_cstr(buf, "null");
        return;
    case DJS_TAG_BOOL:
        buf_append_cstr(buf, value.as.boolean ? "true" : "false");
        return;
    case DJS_TAG_NUMBER: {
        char text[64];
        djs_number_to_string(value.as.number, text, sizeof(text));
        buf_append_cstr(buf, text);
        return;
    }
    case DJS_TAG_STRING:
        util_escape_string(buf, value.as.string->bytes, '"');
        return;
    case DJS_TAG_OBJECT:
        break;
    }

    djs_object_t *object = value.as.object;
    if (object == NULL) {
        buf_append_cstr(buf, "null");
        return;
    }
    if (object->function != NULL) {
        buf_append_cstr(buf, "undefined");
        return;
    }
    if (depth > 4) {
        buf_append_cstr(buf, "[Object]");
        return;
    }
    if (object->class == DJS_CLASS_ARRAY) {
        buf_append_char(buf, '[');
        size_t length = djs_array_length(object);
        for (size_t i = 0; i < length; i++) {
            if (i > 0) buf_append_char(buf, ',');
            util_json_value(rt, buf, djs_array_get(rt, object, i), depth + 1);
        }
        buf_append_char(buf, ']');
        return;
    }
    buf_append_char(buf, '{');
    bool first = true;
    for (djs_property_t *property = djs_property_first(object); property != NULL;
         property = property->next) {
        if (property->flags & DJS_PROP_PRIVATE) continue;
        if (!first) buf_append_char(buf, ',');
        first = false;
        const char *key = property->key.tag == DJS_TAG_STRING
                              ? property->key.as.string->bytes
                              : djs_value_cstr(rt, property->key);
        util_escape_string(buf, key, '"');
        buf_append_char(buf, ':');
        util_json_value(rt, buf, property->value, depth + 1);
    }
    buf_append_char(buf, '}');
}

/* A Node-ish inspection: strings quoted, arrays as [ 1, 2 ], objects as
 * { a: 1 }. */
static void util_inspect_value(djs_runtime_t *rt, djs_buf_t *buf, djs_value_t value,
                               int depth) {
    switch (value.tag) {
    case DJS_TAG_UNDEFINED:
        buf_append_cstr(buf, "undefined");
        return;
    case DJS_TAG_NULL:
        buf_append_cstr(buf, "null");
        return;
    case DJS_TAG_BOOL:
        buf_append_cstr(buf, value.as.boolean ? "true" : "false");
        return;
    case DJS_TAG_NUMBER: {
        char text[64];
        djs_number_to_string(value.as.number, text, sizeof(text));
        buf_append_cstr(buf, text);
        return;
    }
    case DJS_TAG_STRING:
        util_escape_string(buf, value.as.string->bytes, '\'');
        return;
    case DJS_TAG_OBJECT:
        break;
    }

    djs_object_t *object = value.as.object;
    if (object == NULL) {
        buf_append_cstr(buf, "null");
        return;
    }
    if (object->function != NULL) {
        buf_append_cstr(buf, "[Function");
        const char *name = "";
        if (object->function->name.tag == DJS_TAG_STRING) {
            name = object->function->name.as.string->bytes;
        }
        if (name[0] != '\0') {
            buf_append_cstr(buf, ": ");
            buf_append_cstr(buf, name);
        } else {
            buf_append_cstr(buf, " (anonymous)");
        }
        buf_append_char(buf, ']');
        return;
    }
    if (depth > 4) {
        buf_append_cstr(buf, "[Object]");
        return;
    }
    if (object->class == DJS_CLASS_ARRAY) {
        buf_append_cstr(buf, "[ ");
        size_t length = djs_array_length(object);
        for (size_t i = 0; i < length; i++) {
            if (i > 0) buf_append_cstr(buf, ", ");
            util_inspect_value(rt, buf, djs_array_get(rt, object, i), depth + 1);
        }
        buf_append_cstr(buf, " ]");
        return;
    }
    if (object->class == DJS_CLASS_DATE) {
        buf_append_cstr(buf, "[Date]");
        return;
    }
    if (object->class == DJS_CLASS_REGEXP) {
        buf_append_char(buf, '/');
        buf_append_cstr(buf, object->regexp_source ? object->regexp_source : "");
        buf_append_char(buf, '/');
        buf_append_cstr(buf, object->regexp_flags ? object->regexp_flags : "");
        return;
    }
    if (object->class == DJS_CLASS_PROMISE) {
        buf_append_cstr(buf, "[Promise]");
        return;
    }

    buf_append_cstr(buf, "{ ");
    bool first = true;
    for (djs_property_t *property = djs_property_first(object); property != NULL;
         property = property->next) {
        if (property->flags & DJS_PROP_PRIVATE) continue;
        if (!first) buf_append_cstr(buf, ", ");
        first = false;
        const char *key = property->key.tag == DJS_TAG_STRING
                              ? property->key.as.string->bytes
                              : djs_value_cstr(rt, property->key);
        buf_append_cstr(buf, key);
        buf_append_cstr(buf, ": ");
        util_inspect_value(rt, buf, property->value, depth + 1);
    }
    buf_append_cstr(buf, " }");
}

static djs_value_t util_inspect_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_buf_t buf;
    buf_init(&buf, rt);
    util_inspect_value(rt, &buf, arg_count > 0 ? args[0] : DJS_UNDEFINED, 0);
    return str_value(rt, buf_text(&buf));
}

static djs_value_t util_format_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_buf_t buf;
    buf_init(&buf, rt);
    if (arg_count == 0) return str_value(rt, "");

    if (args[0].tag != DJS_TAG_STRING) {
        for (size_t i = 0; i < arg_count; i++) {
            if (i > 0) buf_append_char(&buf, ' ');
            util_inspect_value(rt, &buf, args[i], 0);
        }
        return str_value(rt, buf_text(&buf));
    }

    const char *format = args[0].as.string->bytes;
    size_t length = djs_strlen(format);
    size_t index = 0;
    size_t next = 1;
    while (index < length) {
        char c = format[index];
        if (c != '%' || index + 1 >= length) {
            buf_append_char(&buf, c);
            index++;
            continue;
        }
        char spec = format[index + 1];
        djs_value_t value = next < arg_count ? args[next] : DJS_UNDEFINED;
        switch (spec) {
        case '%':
            buf_append_char(&buf, '%');
            index += 2;
            continue;
        case 's': {
            djs_value_t text = djs_to_string(rt, value);
            if (text.tag == DJS_TAG_STRING) buf_append_cstr(&buf, text.as.string->bytes);
            next++;
            break;
        }
        case 'd':
        case 'i': {
            double number = djs_to_number(rt, value).as.number;
            if (number != number) number = 0.0;
            double truncated = (double)(long long)number;
            char text[64];
            djs_number_to_string(truncated, text, sizeof(text));
            buf_append_cstr(&buf, text);
            next++;
            break;
        }
        case 'f': {
            double number = djs_to_number(rt, value).as.number;
            char text[64];
            djs_number_to_string(number, text, sizeof(text));
            buf_append_cstr(&buf, text);
            next++;
            break;
        }
        case 'j':
            util_json_value(rt, &buf, value, 0);
            next++;
            break;
        case 'o':
        case 'O':
            util_inspect_value(rt, &buf, value, 0);
            next++;
            break;
        default:
            buf_append_char(&buf, '%');
            index++;
            continue;
        }
        index += 2;
    }

    for (; next < arg_count; next++) {
        buf_append_char(&buf, ' ');
        util_inspect_value(rt, &buf, args[next], 0);
    }
    return str_value(rt, buf_text(&buf));
}

static djs_value_t util_is_array_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)rt; (void)this_value;
    return bool_value(arg_count > 0 && djs_value_is_array(args[0]));
}

static djs_value_t util_types_is_date_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)rt; (void)this_value;
    return bool_value(arg_count > 0 && args[0].tag == DJS_TAG_OBJECT &&
                      args[0].as.object != NULL &&
                      args[0].as.object->class == DJS_CLASS_DATE);
}

static djs_value_t util_types_is_regexp_fn(djs_runtime_t *rt, djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    (void)rt; (void)this_value;
    return bool_value(arg_count > 0 && args[0].tag == DJS_TAG_OBJECT &&
                      args[0].as.object != NULL &&
                      args[0].as.object->class == DJS_CLASS_REGEXP);
}

static djs_object_t *util_module_new(djs_runtime_t *rt) {
    djs_object_t *module = djs_object_new(rt, rt->object_prototype);
    if (module == NULL) return NULL;
    djs_define_method(rt, module, "inspect", util_inspect_fn, 1);
    djs_define_method(rt, module, "format", util_format_fn, 1);
    djs_define_method(rt, module, "isArray", util_is_array_fn, 1);

    djs_object_t *types = djs_object_new(rt, rt->object_prototype);
    if (types != NULL) {
        djs_define_method(rt, types, "isDate", util_types_is_date_fn, 1);
        djs_define_method(rt, types, "isRegExp", util_types_is_regexp_fn, 1);
        djs_define_value(rt, module, "types", djs_object_value(types));
    }
    return module;
}

/* -------------------------------------------------------- querystring module */

static void query_escape(djs_buf_t *buf, const char *text) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; text[i] != '\0'; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' ||
            c == '\'' || c == '(' || c == ')') {
            buf_append_char(buf, (char)c);
        } else {
            buf_append_char(buf, '%');
            buf_append_char(buf, hex[c >> 4]);
            buf_append_char(buf, hex[c & 0x0F]);
        }
    }
}

static int query_hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *query_unescape(djs_runtime_t *rt, const char *text, size_t length,
                            bool plus_as_space) {
    djs_buf_t buf;
    buf_init(&buf, rt);
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (c == '%' && i + 2 < length) {
            int high = query_hex_value(text[i + 1]);
            int low = query_hex_value(text[i + 2]);
            if (high >= 0 && low >= 0) {
                buf_append_char(&buf, (char)((high << 4) | low));
                i += 2;
                continue;
            }
        }
        if (plus_as_space && c == '+') {
            buf_append_char(&buf, ' ');
            continue;
        }
        buf_append_char(&buf, c);
    }
    return djs_arena_strdup(rt, buf_text(&buf));
}

static djs_value_t query_parse_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_object_t *object = djs_object_new(rt, rt->object_prototype);
    if (object == NULL) return DJS_UNDEFINED;

    const char *text = arg_cstr(rt, args, arg_count, 0);
    size_t length = djs_strlen(text);
    size_t index = 0;
    while (index < length) {
        size_t start = index;
        while (index < length && text[index] != '&') index++;
        size_t pair_length = index - start;
        if (index < length) index++; /* skip the separator */
        if (pair_length == 0) continue;

        const char *pair = text + start;
        size_t split = pair_length;
        for (size_t i = 0; i < pair_length; i++) {
            if (pair[i] == '=') {
                split = i;
                break;
            }
        }
        char *key = query_unescape(rt, pair, split, true);
        char *value = query_unescape(rt, pair + split + (split < pair_length ? 1 : 0),
                                     pair_length - split - (split < pair_length ? 1 : 0), true);

        djs_property_t *existing = djs_property_get(object, key);
        if (existing == NULL) {
            djs_property_set(rt, object, key, str_value(rt, value), 0);
        } else if (existing->value.tag == DJS_TAG_OBJECT &&
                   existing->value.as.object->class == DJS_CLASS_ARRAY) {
            djs_array_push(rt, existing->value.as.object, str_value(rt, value));
        } else {
            djs_value_t items[2];
            items[0] = existing->value;
            items[1] = str_value(rt, value);
            djs_property_set(rt, object, key, djs_make_array(rt, items, 2), 0);
        }
    }
    return djs_object_value(object);
}

static djs_value_t query_stringify_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_buf_t buf;
    buf_init(&buf, rt);
    if (arg_count == 0 || args[0].tag != DJS_TAG_OBJECT || args[0].as.object == NULL) {
        return str_value(rt, "");
    }

    djs_object_t *object = args[0].as.object;
    bool first = true;
    for (djs_property_t *property = djs_property_first(object); property != NULL;
         property = property->next) {
        if (property->flags & DJS_PROP_PRIVATE) continue;
        const char *key = property->key.tag == DJS_TAG_STRING
                              ? property->key.as.string->bytes
                              : djs_value_cstr(rt, property->key);
        if (property->value.tag == DJS_TAG_OBJECT &&
            property->value.as.object->class == DJS_CLASS_ARRAY) {
            djs_object_t *items = property->value.as.object;
            for (size_t i = 0; i < djs_array_length(items); i++) {
                if (!first) buf_append_char(&buf, '&');
                first = false;
                query_escape(&buf, key);
                buf_append_char(&buf, '=');
                query_escape(&buf, djs_value_cstr(rt, djs_array_get(rt, items, i)));
            }
            continue;
        }
        if (!first) buf_append_char(&buf, '&');
        first = false;
        query_escape(&buf, key);
        buf_append_char(&buf, '=');
        query_escape(&buf, djs_value_cstr(rt, property->value));
    }
    return str_value(rt, buf_text(&buf));
}

static djs_value_t query_escape_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_buf_t buf;
    buf_init(&buf, rt);
    query_escape(&buf, arg_cstr(rt, args, arg_count, 0));
    return str_value(rt, buf_text(&buf));
}

static djs_value_t query_unescape_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *text = arg_cstr(rt, args, arg_count, 0);
    return str_value(rt, query_unescape(rt, text, djs_strlen(text), false));
}

static djs_object_t *querystring_module_new(djs_runtime_t *rt) {
    djs_object_t *module = djs_object_new(rt, rt->object_prototype);
    if (module == NULL) return NULL;
    djs_define_method(rt, module, "parse", query_parse_fn, 1);
    djs_define_method(rt, module, "stringify", query_stringify_fn, 1);
    djs_define_method(rt, module, "escape", query_escape_fn, 1);
    djs_define_method(rt, module, "unescape", query_unescape_fn, 1);
    return module;
}

/* ----------------------------------------------------------------- fs module */

#if defined(DJS_FREESTANDING)
#define FS_O_RDONLY HELIX_O_RDONLY
#define FS_O_WRONLY HELIX_O_WRONLY
#define FS_O_CREAT  HELIX_O_CREAT
#define FS_O_TRUNC  HELIX_O_TRUNC
#define FS_SEEK_SET HELIX_SEEK_SET
#define FS_SEEK_CUR HELIX_SEEK_CUR
#define FS_SEEK_END HELIX_SEEK_END
#define FS_ERRNO_NOENT  2
#define FS_ERRNO_ACCES  13
#define FS_ERRNO_EXIST  17
#define FS_ERRNO_NOTDIR 20
#define FS_ERRNO_ISDIR  21
#define FS_ERRNO_INVAL  22
#define FS_ERRNO_NOSPC  28
#define FS_ERRNO_NOSYS  38
#else
#define FS_O_RDONLY O_RDONLY
#define FS_O_WRONLY O_WRONLY
#define FS_O_RDWR   O_RDWR
#define FS_O_CREAT  O_CREAT
#define FS_O_TRUNC  O_TRUNC
#define FS_O_APPEND O_APPEND
#define FS_SEEK_SET SEEK_SET
#define FS_SEEK_CUR SEEK_CUR
#define FS_SEEK_END SEEK_END
#define FS_ERRNO_NOENT  ENOENT
#define FS_ERRNO_ACCES  EACCES
#define FS_ERRNO_EXIST  EEXIST
#define FS_ERRNO_NOTDIR ENOTDIR
#define FS_ERRNO_ISDIR  EISDIR
#define FS_ERRNO_INVAL  EINVAL
#define FS_ERRNO_NOSPC  ENOSPC
#define FS_ERRNO_NOSYS  ENOSYS
#endif

static const char *fs_error_code(int number) {
    if (number == FS_ERRNO_NOENT) return "ENOENT";
    if (number == FS_ERRNO_ACCES) return "EACCES";
    if (number == FS_ERRNO_EXIST) return "EEXIST";
    if (number == FS_ERRNO_NOTDIR) return "ENOTDIR";
    if (number == FS_ERRNO_ISDIR) return "EISDIR";
    if (number == FS_ERRNO_NOSYS) return "ENOSYS";
    if (number == FS_ERRNO_INVAL) return "EINVAL";
    if (number == FS_ERRNO_NOSPC) return "ENOSPC";
    return "EIO";
}

static const char *fs_error_text(int number) {
    if (number == FS_ERRNO_NOENT) return "no such file or directory";
    if (number == FS_ERRNO_ACCES) return "permission denied";
    if (number == FS_ERRNO_EXIST) return "file already exists";
    if (number == FS_ERRNO_NOTDIR) return "not a directory";
    if (number == FS_ERRNO_ISDIR) return "is a directory";
    if (number == FS_ERRNO_NOSYS) return "operation not supported";
    if (number == FS_ERRNO_INVAL) return "invalid argument";
    if (number == FS_ERRNO_NOSPC) return "no space left on device";
    return "i/o error";
}

/* An fs error: the message reads "ENOENT: no such file or directory,
 * stat '/tmp/x'" and the object carries errno, code, syscall and path. */
static djs_value_t fs_make_error(djs_runtime_t *rt, int number, const char *syscall,
                                 const char *path) {
    djs_buf_t buf;
    buf_init(&buf, rt);
    buf_append_cstr(&buf, fs_error_code(number));
    buf_append_cstr(&buf, ": ");
    buf_append_cstr(&buf, fs_error_text(number));
    buf_append_cstr(&buf, ", ");
    buf_append_cstr(&buf, syscall);
    buf_append_cstr(&buf, " '");
    buf_append_cstr(&buf, path);
    buf_append_char(&buf, '\'');

    djs_value_t error = djs_make_error(rt, "Error", buf_text(&buf));
    if (error.tag == DJS_TAG_OBJECT && error.as.object != NULL) {
        djs_object_t *object = error.as.object;
        djs_property_set(rt, object, "errno", num_value((double)number), 0);
        djs_property_set(rt, object, "code", str_value(rt, fs_error_code(number)), 0);
        djs_property_set(rt, object, "syscall", str_value(rt, syscall), 0);
        djs_property_set(rt, object, "path", str_value(rt, path), 0);
    }
    return error;
}

/* ------------------------------------------------------------ file plumbing */

#if defined(DJS_FREESTANDING)

static int fs_platform_open(const char *path, int flags, int *error) {
    int fd = helix_open(path, flags);
    if (fd < 0) *error = FS_ERRNO_NOENT;
    return fd;
}

static int64_t fs_platform_read(int fd, void *buffer, size_t count, int *error) {
    int64_t got = helix_read(fd, buffer, count);
    if (got < 0) *error = FS_ERRNO_NOENT;
    return got;
}

static int64_t fs_platform_write(int fd, const void *buffer, size_t count, int *error) {
    int64_t written = helix_write(fd, buffer, count);
    if (written < 0) *error = FS_ERRNO_NOENT;
    return written;
}

static int fs_platform_close(int fd, int *error) {
    int result = helix_close(fd);
    if (result < 0) *error = FS_ERRNO_NOENT;
    return result;
}

static int64_t fs_platform_seek(int fd, int64_t offset, int whence, int *error) {
    int64_t position = helix_lseek(fd, offset, whence);
    if (position < 0) *error = FS_ERRNO_NOENT;
    return position;
}

#else

static int fs_platform_open(const char *path, int flags, int *error) {
    int fd = open(path, flags, 0644);
    if (fd < 0) *error = errno;
    return fd;
}

static int64_t fs_platform_read(int fd, void *buffer, size_t count, int *error) {
    ssize_t got = read(fd, buffer, count);
    if (got < 0) *error = errno;
    return (int64_t)got;
}

static int64_t fs_platform_write(int fd, const void *buffer, size_t count, int *error) {
    ssize_t written = write(fd, buffer, count);
    if (written < 0) *error = errno;
    return (int64_t)written;
}

static int fs_platform_close(int fd, int *error) {
    int result = close(fd);
    if (result < 0) *error = errno;
    return result;
}

static int64_t fs_platform_seek(int fd, int64_t offset, int whence, int *error) {
    off_t position = lseek(fd, (off_t)offset, whence);
    if (position < 0) *error = errno;
    return (int64_t)position;
}

#endif

/* A growable list of values (used for directory entries). */
typedef struct {
    djs_runtime_t *rt;
    djs_value_t *items;
    size_t count;
    size_t capacity;
} djs_values_t;

static void values_init(djs_values_t *list, djs_runtime_t *rt) {
    list->rt = rt;
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void values_push(djs_values_t *list, djs_value_t value) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        djs_value_t *items = (djs_value_t *)djs_arena_alloc(list->rt,
                                                            capacity * sizeof(*items));
        if (items == NULL) return;
        if (list->count > 0) djs_memcpy(items, list->items, list->count * sizeof(*items));
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = value;
}

/* ------------------------------------------------------------- fs operations */

static bool fs_op_read_file(djs_runtime_t *rt, const char *path, djs_value_t *out_value,
                            djs_value_t *error_out) {
    int error = 0;
    int fd = fs_platform_open(path, FS_O_RDONLY, &error);
    if (fd < 0) {
        *error_out = fs_make_error(rt, error, "open", path);
        return false;
    }
    int64_t size = fs_platform_seek(fd, 0, FS_SEEK_END, &error);
    if (size < 0) {
        fs_platform_close(fd, &error);
        *error_out = fs_make_error(rt, error, "fstat", path);
        return false;
    }
    fs_platform_seek(fd, 0, FS_SEEK_SET, &error);

    char *buffer = (char *)djs_arena_alloc(rt, (size_t)size + 1);
    if (buffer == NULL) {
        fs_platform_close(fd, &error);
        *error_out = fs_make_error(rt, FS_ERRNO_NOSPC, "read", path);
        return false;
    }
    int64_t total = 0;
    while (total < size) {
        int64_t got = fs_platform_read(fd, buffer + total, (size_t)(size - total), &error);
        if (got < 0) {
            fs_platform_close(fd, &error);
            *error_out = fs_make_error(rt, error, "read", path);
            return false;
        }
        if (got == 0) break;
        total += got;
    }
    fs_platform_close(fd, &error);
    buffer[total] = '\0';
    *out_value = djs_string_value(djs_string_new(rt, buffer, (size_t)total));
    return true;
}

static bool fs_op_write_file(djs_runtime_t *rt, const char *path, const char *data,
                             size_t length, bool append, djs_value_t *error_out) {
    int error = 0;
#if defined(DJS_FREESTANDING)
    int flags = FS_O_WRONLY | FS_O_CREAT | (append ? 0 : FS_O_TRUNC);
#else
    int flags = FS_O_WRONLY | FS_O_CREAT | (append ? FS_O_APPEND : FS_O_TRUNC);
#endif
    int fd = fs_platform_open(path, flags, &error);
    if (fd < 0) {
        *error_out = fs_make_error(rt, error, "open", path);
        return false;
    }
#if defined(DJS_FREESTANDING)
    if (append) fs_platform_seek(fd, 0, FS_SEEK_END, &error);
#endif
    int64_t total = 0;
    while (total < (int64_t)length) {
        int64_t written = fs_platform_write(fd, data + total, length - (size_t)total, &error);
        if (written < 0) {
            fs_platform_close(fd, &error);
            *error_out = fs_make_error(rt, error, "write", path);
            return false;
        }
        if (written == 0) break;
        total += written;
    }
    fs_platform_close(fd, &error);
    return true;
}

#if !defined(DJS_FREESTANDING)

static djs_value_t fs_stat_is_file_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count);
static djs_value_t fs_stat_is_directory_fn(djs_runtime_t *rt, djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count);

#endif

static bool fs_op_stat(djs_runtime_t *rt, const char *path, djs_value_t *out_value,
                       djs_value_t *error_out) {
#if defined(DJS_FREESTANDING)
    *error_out = fs_make_error(rt, FS_ERRNO_NOSYS, "stat", path);
    return false;
#else
    struct stat info;
    if (stat(path, &info) != 0) {
        *error_out = fs_make_error(rt, errno, "stat", path);
        return false;
    }
    djs_object_t *object = djs_object_new(rt, rt->object_prototype);
    if (object == NULL) {
        *error_out = fs_make_error(rt, FS_ERRNO_NOSPC, "stat", path);
        return false;
    }
    djs_define_value(rt, object, "size", num_value((double)info.st_size));
    djs_define_value(rt, object, "mtimeMs", num_value((double)info.st_mtime * 1000.0));
    djs_property_set(rt, object, "#isFile", bool_value(S_ISREG(info.st_mode)),
                      DJS_PROP_PRIVATE);
    djs_property_set(rt, object, "#isDirectory", bool_value(S_ISDIR(info.st_mode)),
                      DJS_PROP_PRIVATE);
    djs_define_method(rt, object, "isFile", fs_stat_is_file_fn, 0);
    djs_define_method(rt, object, "isDirectory", fs_stat_is_directory_fn, 0);
    *out_value = djs_object_value(object);
    return true;
#endif
}

static bool fs_op_readdir(djs_runtime_t *rt, const char *path, djs_value_t *out_value,
                          djs_value_t *error_out) {
#if defined(DJS_FREESTANDING)
    *error_out = fs_make_error(rt, FS_ERRNO_NOSYS, "scandir", path);
    return false;
#else
    DIR *directory = opendir(path);
    if (directory == NULL) {
        *error_out = fs_make_error(rt, errno, "scandir", path);
        return false;
    }
    djs_values_t names;
    values_init(&names, rt);
    struct dirent *entry = readdir(directory);
    while (entry != NULL) {
        if (djs_strcmp(entry->d_name, ".") != 0 && djs_strcmp(entry->d_name, "..") != 0) {
            values_push(&names, str_value(rt, entry->d_name));
        }
        entry = readdir(directory);
    }
    closedir(directory);
    *out_value = djs_make_array(rt, names.items, names.count);
    return true;
#endif
}

static bool fs_op_mkdir(djs_runtime_t *rt, const char *path, bool recursive,
                        djs_value_t *error_out) {
#if defined(DJS_FREESTANDING)
    *error_out = fs_make_error(rt, FS_ERRNO_NOSYS, "mkdir", path);
    return false;
#else
    if (mkdir(path, 0777) == 0) return true;
    int number = errno;
    if (recursive && number == FS_ERRNO_NOENT) {
        /* Create the missing parents first. */
        size_t length = djs_strlen(path);
        size_t index = length;
        while (index > 0 && path[index - 1] != '/') index--;
        if (index > 1) {
            char *parent = djs_arena_strndup(rt, path, index - 1);
            if (parent != NULL) {
                djs_value_t nested;
                if (fs_op_mkdir(rt, parent, true, &nested)) {
                    if (mkdir(path, 0777) == 0) return true;
                    number = errno;
                } else {
                    *error_out = nested;
                    return false;
                }
            }
        }
    }
    *error_out = fs_make_error(rt, number, "mkdir", path);
    return false;
#endif
}

static bool fs_op_rmdir(djs_runtime_t *rt, const char *path, djs_value_t *error_out) {
#if defined(DJS_FREESTANDING)
    *error_out = fs_make_error(rt, FS_ERRNO_NOSYS, "rmdir", path);
    return false;
#else
    if (rmdir(path) != 0) {
        *error_out = fs_make_error(rt, errno, "rmdir", path);
        return false;
    }
    return true;
#endif
}

static bool fs_op_unlink(djs_runtime_t *rt, const char *path, djs_value_t *error_out) {
#if defined(DJS_FREESTANDING)
    *error_out = fs_make_error(rt, FS_ERRNO_NOSYS, "unlink", path);
    return false;
#else
    if (unlink(path) != 0) {
        *error_out = fs_make_error(rt, errno, "unlink", path);
        return false;
    }
    return true;
#endif
}

static bool fs_op_rename(djs_runtime_t *rt, const char *from, const char *to,
                         djs_value_t *error_out) {
#if defined(DJS_FREESTANDING)
    *error_out = fs_make_error(rt, FS_ERRNO_NOSYS, "rename", from);
    return false;
#else
    if (rename(from, to) != 0) {
        *error_out = fs_make_error(rt, errno, "rename", from);
        return false;
    }
    return true;
#endif
}

/* -------------------------------------------------------------- fs functions */

#if !defined(DJS_FREESTANDING)

static djs_value_t fs_stat_is_file_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    if (this_value.tag != DJS_TAG_OBJECT || this_value.as.object == NULL) return bool_value(false);
    djs_value_t flag = djs_object_get(rt, this_value.as.object, "#isFile");
    return bool_value(flag.tag == DJS_TAG_BOOL && flag.as.boolean);
}

static djs_value_t fs_stat_is_directory_fn(djs_runtime_t *rt, djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    if (this_value.tag != DJS_TAG_OBJECT || this_value.as.object == NULL) return bool_value(false);
    djs_value_t flag = djs_object_get(rt, this_value.as.object, "#isDirectory");
    return bool_value(flag.tag == DJS_TAG_BOOL && flag.as.boolean);
}

#endif

static djs_value_t fs_read_file_sync_fn(djs_runtime_t *rt, djs_value_t this_value,
                                        djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *path = arg_cstr(rt, args, arg_count, 0);
    djs_value_t value = DJS_UNDEFINED;
    djs_value_t error = DJS_UNDEFINED;
    if (!fs_op_read_file(rt, path, &value, &error)) djs_throw(rt, error);
    return value;
}

static djs_value_t fs_write_file_sync_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *path = arg_cstr(rt, args, arg_count, 0);
    const char *data = arg_cstr(rt, args, arg_count, 1);
    djs_value_t error = DJS_UNDEFINED;
    if (!fs_op_write_file(rt, path, data, djs_strlen(data), false, &error)) djs_throw(rt, error);
    return DJS_UNDEFINED;
}

static djs_value_t fs_append_file_sync_fn(djs_runtime_t *rt, djs_value_t this_value,
                                          djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *path = arg_cstr(rt, args, arg_count, 0);
    const char *data = arg_cstr(rt, args, arg_count, 1);
    djs_value_t error = DJS_UNDEFINED;
    if (!fs_op_write_file(rt, path, data, djs_strlen(data), true, &error)) djs_throw(rt, error);
    return DJS_UNDEFINED;
}

static djs_value_t fs_exists_sync_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *path = arg_cstr(rt, args, arg_count, 0);
    int error = 0;
    int fd = fs_platform_open(path, FS_O_RDONLY, &error);
    if (fd < 0) return bool_value(false);
    fs_platform_close(fd, &error);
    return bool_value(true);
}

static djs_value_t fs_stat_sync_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *path = arg_cstr(rt, args, arg_count, 0);
    djs_value_t value = DJS_UNDEFINED;
    djs_value_t error = DJS_UNDEFINED;
    if (!fs_op_stat(rt, path, &value, &error)) djs_throw(rt, error);
    return value;
}

static djs_value_t fs_readdir_sync_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *path = arg_cstr(rt, args, arg_count, 0);
    djs_value_t value = DJS_UNDEFINED;
    djs_value_t error = DJS_UNDEFINED;
    if (!fs_op_readdir(rt, path, &value, &error)) djs_throw(rt, error);
    return value;
}

/* The options argument may be an object with a "recursive" flag. */
static bool fs_options_recursive(djs_runtime_t *rt, djs_value_t *args, size_t arg_count) {
    if (arg_count < 2 || args[1].tag != DJS_TAG_OBJECT || args[1].as.object == NULL) {
        return false;
    }
    djs_value_t flag = djs_object_get(rt, args[1].as.object, "recursive");
    return flag.tag == DJS_TAG_BOOL && flag.as.boolean;
}

static djs_value_t fs_mkdir_sync_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *path = arg_cstr(rt, args, arg_count, 0);
    djs_value_t error = DJS_UNDEFINED;
    if (!fs_op_mkdir(rt, path, fs_options_recursive(rt, args, arg_count), &error)) {
        djs_throw(rt, error);
    }
    return DJS_UNDEFINED;
}

static djs_value_t fs_rmdir_sync_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *path = arg_cstr(rt, args, arg_count, 0);
    djs_value_t error = DJS_UNDEFINED;
    if (!fs_op_rmdir(rt, path, &error)) djs_throw(rt, error);
    return DJS_UNDEFINED;
}

static djs_value_t fs_unlink_sync_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *path = arg_cstr(rt, args, arg_count, 0);
    djs_value_t error = DJS_UNDEFINED;
    if (!fs_op_unlink(rt, path, &error)) djs_throw(rt, error);
    return DJS_UNDEFINED;
}

static djs_value_t fs_rename_sync_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *from = arg_cstr(rt, args, arg_count, 0);
    const char *to = arg_cstr(rt, args, arg_count, 1);
    djs_value_t error = DJS_UNDEFINED;
    if (!fs_op_rename(rt, from, to, &error)) djs_throw(rt, error);
    return DJS_UNDEFINED;
}

/* The flags of openSync(): "r", "w" or "a" (the "+" variants are not
 * supported by the freestanding kernel API). */
static int fs_open_flags(djs_runtime_t *rt, const char *flags) {
    bool writing = false;
    bool appending = false;
    bool updating = false;
    for (size_t i = 0; flags[i] != '\0'; i++) {
        if (flags[i] == 'w') writing = true;
        else if (flags[i] == 'a') appending = true;
        else if (flags[i] == '+') updating = true;
    }
#if defined(DJS_FREESTANDING)
    (void)updating;
    if (writing) return FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC;
    if (appending) return FS_O_WRONLY | FS_O_CREAT;
    return FS_O_RDONLY;
#else
    if (writing) {
        return (updating ? FS_O_RDWR : FS_O_WRONLY) | FS_O_CREAT | FS_O_TRUNC;
    }
    if (appending) {
        return (updating ? FS_O_RDWR : FS_O_WRONLY) | FS_O_CREAT | FS_O_APPEND;
    }
    return updating ? FS_O_RDWR : FS_O_RDONLY;
#endif
}

static djs_value_t fs_open_sync_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *path = arg_cstr(rt, args, arg_count, 0);
    const char *flags = arg_count > 1 ? arg_cstr(rt, args, arg_count, 1) : "r";
    int error = 0;
    int fd = fs_platform_open(path, fs_open_flags(rt, flags), &error);
    if (fd < 0) djs_throw(rt, fs_make_error(rt, error, "open", path));
    return num_value((double)fd);
}

static djs_value_t fs_close_sync_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value;
    int fd = arg_count > 0 && djs_is_number(args[0]) ? (int)args[0].as.number : -1;
    int error = 0;
    if (fs_platform_close(fd, &error) < 0) {
        djs_throw(rt, fs_make_error(rt, error, "close", ""));
    }
    return DJS_UNDEFINED;
}

/* ponytail: readSync(fd, length) returns the bytes as a string; the engine
 * has no Buffer type. Add buffers when one is needed. */
static djs_value_t fs_read_sync_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)this_value;
    int fd = arg_count > 0 && djs_is_number(args[0]) ? (int)args[0].as.number : -1;
    size_t length = arg_count > 1 && djs_is_number(args[1])
                        ? (size_t)args[1].as.number
                        : 0;
    if (length == 0) return str_value(rt, "");
    char *buffer = (char *)djs_arena_alloc(rt, length + 1);
    if (buffer == NULL) return str_value(rt, "");
    int error = 0;
    int64_t got = fs_platform_read(fd, buffer, length, &error);
    if (got < 0) djs_throw(rt, fs_make_error(rt, error, "read", ""));
    return djs_string_value(djs_string_new(rt, buffer, (size_t)got));
}

static djs_value_t fs_write_sync_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value;
    int fd = arg_count > 0 && djs_is_number(args[0]) ? (int)args[0].as.number : -1;
    const char *data = arg_cstr(rt, args, arg_count, 1);
    size_t length = djs_strlen(data);
    int error = 0;
    int64_t written = fs_platform_write(fd, data, length, &error);
    if (written < 0) djs_throw(rt, fs_make_error(rt, error, "write", ""));
    return num_value((double)written);
}

/* --------------------------------------------------------- fs.promises */

static djs_value_t fs_promises_read_file_fn(djs_runtime_t *rt, djs_value_t this_value,
                                            djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t promise = djs_promise_new(rt);
    const char *path = arg_cstr(rt, args, arg_count, 0);
    djs_value_t value = DJS_UNDEFINED;
    djs_value_t error = DJS_UNDEFINED;
    if (fs_op_read_file(rt, path, &value, &error)) {
        djs_promise_resolve(rt, promise, value);
    } else {
        djs_promise_reject(rt, promise, error);
    }
    return promise;
}

static djs_value_t fs_promises_write_file_fn(djs_runtime_t *rt, djs_value_t this_value,
                                             djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t promise = djs_promise_new(rt);
    const char *path = arg_cstr(rt, args, arg_count, 0);
    const char *data = arg_cstr(rt, args, arg_count, 1);
    djs_value_t error = DJS_UNDEFINED;
    if (fs_op_write_file(rt, path, data, djs_strlen(data), false, &error)) {
        djs_promise_resolve(rt, promise, DJS_UNDEFINED);
    } else {
        djs_promise_reject(rt, promise, error);
    }
    return promise;
}

static djs_value_t fs_promises_stat_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t promise = djs_promise_new(rt);
    const char *path = arg_cstr(rt, args, arg_count, 0);
    djs_value_t value = DJS_UNDEFINED;
    djs_value_t error = DJS_UNDEFINED;
    if (fs_op_stat(rt, path, &value, &error)) {
        djs_promise_resolve(rt, promise, value);
    } else {
        djs_promise_reject(rt, promise, error);
    }
    return promise;
}

static djs_value_t fs_promises_readdir_fn(djs_runtime_t *rt, djs_value_t this_value,
                                          djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t promise = djs_promise_new(rt);
    const char *path = arg_cstr(rt, args, arg_count, 0);
    djs_value_t value = DJS_UNDEFINED;
    djs_value_t error = DJS_UNDEFINED;
    if (fs_op_readdir(rt, path, &value, &error)) {
        djs_promise_resolve(rt, promise, value);
    } else {
        djs_promise_reject(rt, promise, error);
    }
    return promise;
}

static djs_value_t fs_promises_mkdir_fn(djs_runtime_t *rt, djs_value_t this_value,
                                        djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t promise = djs_promise_new(rt);
    const char *path = arg_cstr(rt, args, arg_count, 0);
    djs_value_t error = DJS_UNDEFINED;
    if (fs_op_mkdir(rt, path, fs_options_recursive(rt, args, arg_count), &error)) {
        djs_promise_resolve(rt, promise, DJS_UNDEFINED);
    } else {
        djs_promise_reject(rt, promise, error);
    }
    return promise;
}

static djs_value_t fs_promises_unlink_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t promise = djs_promise_new(rt);
    const char *path = arg_cstr(rt, args, arg_count, 0);
    djs_value_t error = DJS_UNDEFINED;
    if (fs_op_unlink(rt, path, &error)) {
        djs_promise_resolve(rt, promise, DJS_UNDEFINED);
    } else {
        djs_promise_reject(rt, promise, error);
    }
    return promise;
}

static djs_object_t *fs_module_new(djs_runtime_t *rt) {
    djs_object_t *module = djs_object_new(rt, rt->object_prototype);
    if (module == NULL) return NULL;
    djs_define_method(rt, module, "readFileSync", fs_read_file_sync_fn, 2);
    djs_define_method(rt, module, "writeFileSync", fs_write_file_sync_fn, 2);
    djs_define_method(rt, module, "appendFileSync", fs_append_file_sync_fn, 2);
    djs_define_method(rt, module, "existsSync", fs_exists_sync_fn, 1);
    djs_define_method(rt, module, "statSync", fs_stat_sync_fn, 1);
    djs_define_method(rt, module, "readdirSync", fs_readdir_sync_fn, 1);
    djs_define_method(rt, module, "mkdirSync", fs_mkdir_sync_fn, 2);
    djs_define_method(rt, module, "rmdirSync", fs_rmdir_sync_fn, 1);
    djs_define_method(rt, module, "unlinkSync", fs_unlink_sync_fn, 1);
    djs_define_method(rt, module, "renameSync", fs_rename_sync_fn, 2);
    djs_define_method(rt, module, "openSync", fs_open_sync_fn, 2);
    djs_define_method(rt, module, "closeSync", fs_close_sync_fn, 1);
    djs_define_method(rt, module, "readSync", fs_read_sync_fn, 2);
    djs_define_method(rt, module, "writeSync", fs_write_sync_fn, 2);

    djs_object_t *promises = djs_object_new(rt, rt->object_prototype);
    if (promises != NULL) {
        djs_define_method(rt, promises, "readFile", fs_promises_read_file_fn, 2);
        djs_define_method(rt, promises, "writeFile", fs_promises_write_file_fn, 2);
        djs_define_method(rt, promises, "stat", fs_promises_stat_fn, 2);
        djs_define_method(rt, promises, "readdir", fs_promises_readdir_fn, 2);
        djs_define_method(rt, promises, "mkdir", fs_promises_mkdir_fn, 2);
        djs_define_method(rt, promises, "unlink", fs_promises_unlink_fn, 2);
        djs_define_value(rt, module, "promises", djs_object_value(promises));
    }
    return module;
}

/* ------------------------------------------------------------ events module */

/* The listeners of an emitter live in a hidden "#events" object: the event
 * name maps to an array of { fn, once } entries. */
static djs_object_t *emitter_events_object(djs_runtime_t *rt, djs_value_t this_value) {
    if (this_value.tag != DJS_TAG_OBJECT || this_value.as.object == NULL) return NULL;
    djs_value_t events = djs_object_get(rt, this_value.as.object, "#events");
    if (events.tag != DJS_TAG_OBJECT || events.as.object == NULL) return NULL;
    return events.as.object;
}

static djs_object_t *emitter_list(djs_runtime_t *rt, djs_object_t *events,
                                  const char *name, bool create) {
    if (events == NULL) return NULL;
    djs_value_t list = djs_object_get(rt, events, name);
    if (list.tag == DJS_TAG_OBJECT && list.as.object != NULL &&
        list.as.object->class == DJS_CLASS_ARRAY) {
        return list.as.object;
    }
    if (!create) return NULL;
    djs_object_t *array = djs_array_new(rt);
    if (array == NULL) return NULL;
    djs_property_set(rt, events, name, djs_object_value(array), 0);
    return array;
}

static djs_value_t emitter_entry_fn(djs_runtime_t *rt, djs_value_t entry) {
    if (entry.tag != DJS_TAG_OBJECT || entry.as.object == NULL) return DJS_UNDEFINED;
    return djs_object_get(rt, entry.as.object, "fn");
}

static bool emitter_entry_once(djs_runtime_t *rt, djs_value_t entry) {
    if (entry.tag != DJS_TAG_OBJECT || entry.as.object == NULL) return false;
    djs_value_t flag = djs_object_get(rt, entry.as.object, "once");
    return flag.tag == DJS_TAG_BOOL && flag.as.boolean;
}

static djs_value_t events_on_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    if (arg_count < 2) djs_throw_type_error(rt, "listener must be a function");
    djs_object_t *events = emitter_events_object(rt, this_value);
    if (events == NULL) djs_throw_type_error(rt, "not an emitter");
    const char *name = arg_cstr(rt, args, arg_count, 0);
    djs_object_t *list = emitter_list(rt, events, name, true);
    if (list == NULL) return this_value;

    djs_object_t *entry = djs_object_new(rt, rt->object_prototype);
    if (entry == NULL) return this_value;
    djs_property_set(rt, entry, "fn", args[1], 0);
    djs_property_set(rt, entry, "once", bool_value(false), 0);
    djs_array_push(rt, list, djs_object_value(entry));
    return this_value;
}

static djs_value_t events_once_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    if (arg_count < 2) djs_throw_type_error(rt, "listener must be a function");
    djs_object_t *events = emitter_events_object(rt, this_value);
    if (events == NULL) djs_throw_type_error(rt, "not an emitter");
    const char *name = arg_cstr(rt, args, arg_count, 0);
    djs_object_t *list = emitter_list(rt, events, name, true);
    if (list == NULL) return this_value;

    djs_object_t *entry = djs_object_new(rt, rt->object_prototype);
    if (entry == NULL) return this_value;
    djs_property_set(rt, entry, "fn", args[1], 0);
    djs_property_set(rt, entry, "once", bool_value(true), 0);
    djs_array_push(rt, list, djs_object_value(entry));
    return this_value;
}

static djs_value_t events_remove_listener_fn(djs_runtime_t *rt, djs_value_t this_value,
                                             djs_value_t *args, size_t arg_count) {
    djs_object_t *events = emitter_events_object(rt, this_value);
    if (events == NULL) return this_value;
    const char *name = arg_cstr(rt, args, arg_count, 0);
    djs_object_t *list = emitter_list(rt, events, name, false);
    if (list == NULL) return this_value;

    djs_value_t listener = arg_count > 1 ? args[1] : DJS_UNDEFINED;
    djs_values_t kept;
    values_init(&kept, rt);
    bool removed = false;
    for (size_t i = 0; i < djs_array_length(list); i++) {
        djs_value_t entry = djs_array_get(rt, list, i);
        if (!removed && djs_strict_equals(emitter_entry_fn(rt, entry), listener)) {
            removed = true;
            continue;
        }
        values_push(&kept, entry);
    }
    if (removed) {
        djs_property_set(rt, events, name, djs_make_array(rt, kept.items, kept.count), 0);
    }
    return this_value;
}

static djs_value_t events_emit_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    djs_object_t *events = emitter_events_object(rt, this_value);
    if (events == NULL) return bool_value(false);
    const char *name = arg_cstr(rt, args, arg_count, 0);
    djs_object_t *list = emitter_list(rt, events, name, false);
    if (list == NULL) return bool_value(false);

    size_t length = djs_array_length(list);
    djs_value_t *snapshot = (djs_value_t *)djs_arena_alloc(rt, (length + 1) * sizeof(*snapshot));
    if (snapshot == NULL) return bool_value(false);

    /* The once listeners are removed before they run. */
    djs_values_t kept;
    values_init(&kept, rt);
    for (size_t i = 0; i < length; i++) {
        djs_value_t entry = djs_array_get(rt, list, i);
        snapshot[i] = entry;
        if (!emitter_entry_once(rt, entry)) values_push(&kept, entry);
    }
    djs_property_set(rt, events, name, djs_make_array(rt, kept.items, kept.count), 0);

    bool ran = false;
    for (size_t i = 0; i < length; i++) {
        djs_value_t listener = emitter_entry_fn(rt, snapshot[i]);
        if (!djs_is_callable(listener)) continue;
        djs_call(rt, listener, this_value, arg_count > 1 ? args + 1 : NULL,
                 arg_count > 1 ? arg_count - 1 : 0);
        ran = true;
    }
    return bool_value(ran);
}

static djs_value_t events_remove_all_listeners_fn(djs_runtime_t *rt,
                                                  djs_value_t this_value,
                                                  djs_value_t *args, size_t arg_count) {
    djs_object_t *events = emitter_events_object(rt, this_value);
    if (events == NULL) return this_value;
    if (arg_count > 0) {
        const char *name = arg_cstr(rt, args, arg_count, 0);
        djs_property_delete(rt, events, name);
        return this_value;
    }
    for (djs_property_t *property = djs_property_first(events); property != NULL;) {
        djs_property_t *next = property->next;
        if (property->key.tag == DJS_TAG_STRING) {
            djs_property_delete(rt, events, property->key.as.string->bytes);
        }
        property = next;
    }
    return this_value;
}

static djs_value_t events_listener_count_fn(djs_runtime_t *rt, djs_value_t this_value,
                                            djs_value_t *args, size_t arg_count) {
    djs_object_t *events = emitter_events_object(rt, this_value);
    if (events == NULL) return num_value(0.0);
    const char *name = arg_cstr(rt, args, arg_count, 0);
    djs_object_t *list = emitter_list(rt, events, name, false);
    return num_value(list == NULL ? 0.0 : (double)djs_array_length(list));
}

static djs_value_t events_listeners_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    djs_object_t *events = emitter_events_object(rt, this_value);
    if (events == NULL) return djs_make_array(rt, NULL, 0);
    const char *name = arg_cstr(rt, args, arg_count, 0);
    djs_object_t *list = emitter_list(rt, events, name, false);
    djs_values_t listeners;
    values_init(&listeners, rt);
    if (list != NULL) {
        for (size_t i = 0; i < djs_array_length(list); i++) {
            values_push(&listeners, emitter_entry_fn(rt, djs_array_get(rt, list, i)));
        }
    }
    return djs_make_array(rt, listeners.items, listeners.count);
}

static void emitter_install_methods(djs_runtime_t *rt, djs_object_t *emitter) {
    djs_define_method(rt, emitter, "on", events_on_fn, 2);
    djs_define_method(rt, emitter, "once", events_once_fn, 2);
    djs_define_method(rt, emitter, "off", events_remove_listener_fn, 2);
    djs_define_method(rt, emitter, "removeListener", events_remove_listener_fn, 2);
    djs_define_method(rt, emitter, "emit", events_emit_fn, 1);
    djs_define_method(rt, emitter, "removeAllListeners", events_remove_all_listeners_fn, 1);
    djs_define_method(rt, emitter, "listeners", events_listeners_fn, 1);
    djs_define_method(rt, emitter, "listenerCount", events_listener_count_fn, 1);
}

static void emitter_install(djs_runtime_t *rt, djs_object_t *emitter) {
    if (emitter == NULL) return;
    emitter_install_methods(rt, emitter);
    if (!djs_object_has(emitter, "#events")) {
        djs_property_set(rt, emitter, "#events",
                         djs_object_value(djs_object_new(rt, rt->object_prototype)),
                         DJS_PROP_PRIVATE);
    }
}

static djs_value_t events_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *emitter = NULL;
    if (this_value.tag == DJS_TAG_OBJECT && this_value.as.object != NULL) {
        emitter = this_value.as.object;
    } else {
        emitter = djs_object_new(rt, rt->object_prototype);
    }
    if (emitter == NULL) return DJS_UNDEFINED;
    emitter_install(rt, emitter);
    return djs_object_value(emitter);
}

static djs_object_t *events_module_new(djs_runtime_t *rt) {
    djs_object_t *module = djs_object_new(rt, rt->object_prototype);
    if (module == NULL) return NULL;
    emitter_install(rt, module);

    djs_object_t *constructor = djs_function_new(rt, events_constructor_fn, "EventEmitter", 0);
    if (constructor != NULL) {
        djs_object_t *prototype = djs_object_new(rt, rt->object_prototype);
        if (prototype != NULL) {
            emitter_install_methods(rt, prototype);
            djs_property_set(rt, prototype, "constructor", djs_object_value(constructor), 0);
        }
        djs_property_set(rt, constructor, "prototype", djs_object_value(prototype), 0);
        djs_property_set(rt, module, "EventEmitter", djs_object_value(constructor), 0);
    }
    return module;
}

/* ----------------------------------------------------------- assert module */

static void assert_fail(djs_runtime_t *rt, const char *message) {
    djs_throw(rt, djs_make_error(rt, "AssertionError", message));
}

static djs_value_t assert_fn(djs_runtime_t *rt, djs_value_t this_value,
                             djs_value_t *args, size_t arg_count) {
    (void)this_value;
    bool ok = arg_count > 0 && djs_to_boolean(args[0]);
    if (!ok) {
        const char *message = arg_count > 1 ? arg_cstr(rt, args, arg_count, 1) : "";
        assert_fail(rt, message[0] != '\0' ? message : "Assertion failed");
    }
    return DJS_UNDEFINED;
}

static djs_value_t assert_equal_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t left = arg_count > 0 ? args[0] : DJS_UNDEFINED;
    djs_value_t right = arg_count > 1 ? args[1] : DJS_UNDEFINED;
    if (!djs_loose_equals(rt, left, right)) {
        const char *message = arg_count > 2 ? arg_cstr(rt, args, arg_count, 2) : "";
        assert_fail(rt, message[0] != '\0' ? message
                                         : "Expected values to be loosely equal");
    }
    return DJS_UNDEFINED;
}

static djs_value_t assert_strict_equal_fn(djs_runtime_t *rt, djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    (void)rt; (void)this_value;
    djs_value_t left = arg_count > 0 ? args[0] : DJS_UNDEFINED;
    djs_value_t right = arg_count > 1 ? args[1] : DJS_UNDEFINED;
    if (!djs_strict_equals(left, right)) {
        const char *message = arg_count > 2 ? arg_cstr(rt, args, arg_count, 2) : "";
        assert_fail(rt, message[0] != '\0' ? message
                                         : "Expected values to be strictly equal");
    }
    return DJS_UNDEFINED;
}

static djs_value_t assert_not_strict_equal_fn(djs_runtime_t *rt, djs_value_t this_value,
                                               djs_value_t *args, size_t arg_count) {
    (void)rt; (void)this_value;
    djs_value_t left = arg_count > 0 ? args[0] : DJS_UNDEFINED;
    djs_value_t right = arg_count > 1 ? args[1] : DJS_UNDEFINED;
    if (djs_strict_equals(left, right)) {
        const char *message = arg_count > 2 ? arg_cstr(rt, args, arg_count, 2) : "";
        assert_fail(rt, message[0] != '\0' ? message
                                         : "Expected values to be not strictly equal");
    }
    return DJS_UNDEFINED;
}

/* A recursive comparison of plain objects and arrays. */
static bool assert_deep_equal(djs_runtime_t *rt, djs_value_t left, djs_value_t right,
                              int depth) {
    if (djs_strict_equals(left, right)) return true;
    if (depth > 8) return false;
    if (left.tag != DJS_TAG_OBJECT || right.tag != DJS_TAG_OBJECT) return false;
    djs_object_t *left_object = left.as.object;
    djs_object_t *right_object = right.as.object;
    if (left_object == NULL || right_object == NULL) return false;

    if (left_object->class == DJS_CLASS_ARRAY || right_object->class == DJS_CLASS_ARRAY) {
        if (left_object->class != DJS_CLASS_ARRAY || right_object->class != DJS_CLASS_ARRAY) {
            return false;
        }
        size_t length = djs_array_length(left_object);
        if (length != djs_array_length(right_object)) return false;
        for (size_t i = 0; i < length; i++) {
            if (!assert_deep_equal(rt, djs_array_get(rt, left_object, i),
                                   djs_array_get(rt, right_object, i), depth + 1)) {
                return false;
            }
        }
        return true;
    }

    for (djs_property_t *property = djs_property_first(left_object); property != NULL;
         property = property->next) {
        if (property->flags & DJS_PROP_PRIVATE) continue;
        const char *key = property->key.tag == DJS_TAG_STRING
                              ? property->key.as.string->bytes
                              : djs_value_cstr(rt, property->key);
        if (!assert_deep_equal(rt, property->value, djs_object_get(rt, right_object, key),
                               depth + 1)) {
            return false;
        }
    }
    for (djs_property_t *property = djs_property_first(right_object); property != NULL;
         property = property->next) {
        if (property->flags & DJS_PROP_PRIVATE) continue;
        const char *key = property->key.tag == DJS_TAG_STRING
                              ? property->key.as.string->bytes
                              : djs_value_cstr(rt, property->key);
        if (djs_property_get(left_object, key) == NULL) return false;
    }
    return true;
}

static djs_value_t assert_deep_strict_equal_fn(djs_runtime_t *rt, djs_value_t this_value,
                                                djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t left = arg_count > 0 ? args[0] : DJS_UNDEFINED;
    djs_value_t right = arg_count > 1 ? args[1] : DJS_UNDEFINED;
    if (!assert_deep_equal(rt, left, right, 0)) {
        const char *message = arg_count > 2 ? arg_cstr(rt, args, arg_count, 2) : "";
        assert_fail(rt, message[0] != '\0' ? message
                                         : "Expected values to be deeply equal");
    }
    return DJS_UNDEFINED;
}

static djs_value_t assert_throws_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value;
    if (arg_count == 0 || !djs_is_callable(args[0])) {
        djs_throw_type_error(rt, "function is required");
    }

    volatile bool threw = false;
    volatile djs_value_t thrown = DJS_UNDEFINED;
    djs_catch_frame_t frame;
    frame.previous = rt->catch_frame;
    frame.error = DJS_UNDEFINED;
    rt->catch_frame = &frame;
    if (djs_setjmp(&frame.saved) == 0) {
        djs_call(rt, args[0], DJS_UNDEFINED, NULL, 0);
        rt->catch_frame = frame.previous;
    } else {
        rt->catch_frame = frame.previous;
        threw = true;
        thrown = frame.error;
    }

    if (!threw) {
        const char *message = arg_count > 1 ? arg_cstr(rt, args, arg_count, 1) : "";
        assert_fail(rt, message[0] != '\0' ? message : "Missing expected exception");
        return DJS_UNDEFINED;
    }
    if (arg_count > 1 && args[1].tag == DJS_TAG_STRING && thrown.tag == DJS_TAG_OBJECT) {
        djs_value_t error_message = djs_object_get(rt, thrown.as.object, "message");
        const char *text = error_message.tag == DJS_TAG_STRING
                               ? error_message.as.string->bytes
                               : "";
        if (!text_contains(text, args[1].as.string->bytes)) {
            assert_fail(rt, "The error message does not match the expectation");
        }
    }
    return DJS_UNDEFINED;
}

static djs_value_t assert_fail_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *message = arg_count > 0 ? arg_cstr(rt, args, arg_count, 0) : "";
    assert_fail(rt, message[0] != '\0' ? message : "Failed");
    return DJS_UNDEFINED;
}

static djs_object_t *assert_module_new(djs_runtime_t *rt) {
    /* The module is callable: assert(value, message). */
    djs_object_t *module = djs_function_new(rt, assert_fn, "assert", 2);
    if (module == NULL) return NULL;
    djs_define_method(rt, module, "ok", assert_fn, 2);
    djs_define_method(rt, module, "equal", assert_equal_fn, 3);
    djs_define_method(rt, module, "strictEqual", assert_strict_equal_fn, 3);
    djs_define_method(rt, module, "notStrictEqual", assert_not_strict_equal_fn, 3);
    djs_define_method(rt, module, "deepStrictEqual", assert_deep_strict_equal_fn, 3);
    djs_define_method(rt, module, "throws", assert_throws_fn, 2);
    djs_define_method(rt, module, "fail", assert_fail_fn, 1);
    return module;
}

/* ----------------------------------------------------------- timers module */

static djs_value_t timers_set_timeout_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)this_value;
    if (arg_count == 0) djs_throw_type_error(rt, "callback is required");
    double delay = arg_count > 1 ? djs_to_number(rt, args[1]).as.number : 0.0;
    if (!(delay > 0.0)) delay = 0.0;
    uint64_t id = djs_timer_add(rt, args[0], delay, false, arg_count > 2 ? args + 2 : NULL,
                                 arg_count > 2 ? arg_count - 2 : 0);
    return num_value((double)id);
}

static djs_value_t timers_set_interval_fn(djs_runtime_t *rt, djs_value_t this_value,
                                          djs_value_t *args, size_t arg_count) {
    (void)this_value;
    if (arg_count == 0) djs_throw_type_error(rt, "callback is required");
    double delay = arg_count > 1 ? djs_to_number(rt, args[1]).as.number : 1.0;
    if (!(delay >= 1.0)) delay = 1.0;
    uint64_t id = djs_timer_add(rt, args[0], delay, true, arg_count > 2 ? args + 2 : NULL,
                                 arg_count > 2 ? arg_count - 2 : 0);
    return num_value((double)id);
}

static djs_value_t timers_set_immediate_fn(djs_runtime_t *rt, djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    (void)this_value;
    if (arg_count == 0) djs_throw_type_error(rt, "callback is required");
    uint64_t id = djs_timer_add(rt, args[0], 0.0, false, arg_count > 1 ? args + 1 : NULL,
                                 arg_count > 1 ? arg_count - 1 : 0);
    return num_value((double)id);
}

static djs_value_t timers_clear_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)this_value;
    if (arg_count == 0) return DJS_UNDEFINED;
    double id = djs_to_number(rt, args[0]).as.number;
    if (id < 0.0) return DJS_UNDEFINED;
    djs_timer_clear(rt, (uint64_t)id);
    return DJS_UNDEFINED;
}

static djs_object_t *timers_module_new(djs_runtime_t *rt) {
    djs_object_t *module = djs_object_new(rt, rt->object_prototype);
    if (module == NULL) return NULL;
    djs_define_method(rt, module, "setTimeout", timers_set_timeout_fn, 2);
    djs_define_method(rt, module, "setInterval", timers_set_interval_fn, 2);
    djs_define_method(rt, module, "setImmediate", timers_set_immediate_fn, 1);
    djs_define_method(rt, module, "clearTimeout", timers_clear_fn, 1);
    djs_define_method(rt, module, "clearInterval", timers_clear_fn, 1);
    djs_define_method(rt, module, "clearImmediate", timers_clear_fn, 1);
    return module;
}

/* ---------------------------------------------------------- module registry */

static bool module_is_builtin(const char *name) {
    return djs_strcmp(name, "path") == 0 || djs_strcmp(name, "os") == 0 ||
           djs_strcmp(name, "process") == 0 || djs_strcmp(name, "util") == 0 ||
           djs_strcmp(name, "querystring") == 0 || djs_strcmp(name, "fs") == 0 ||
           djs_strcmp(name, "events") == 0 || djs_strcmp(name, "assert") == 0 ||
           djs_strcmp(name, "timers") == 0;
}

static djs_object_t *module_create(djs_runtime_t *rt, const char *name) {
    if (djs_strcmp(name, "path") == 0) return path_module_new(rt);
    if (djs_strcmp(name, "os") == 0) return os_module_new(rt);
    if (djs_strcmp(name, "process") == 0) return process_module_new(rt);
    if (djs_strcmp(name, "util") == 0) return util_module_new(rt);
    if (djs_strcmp(name, "querystring") == 0) return querystring_module_new(rt);
    if (djs_strcmp(name, "fs") == 0) return fs_module_new(rt);
    if (djs_strcmp(name, "events") == 0) return events_module_new(rt);
    if (djs_strcmp(name, "assert") == 0) return assert_module_new(rt);
    if (djs_strcmp(name, "timers") == 0) return timers_module_new(rt);
    return NULL;
}

/* The exports of a builtin module; the same object comes back every time. */
djs_value_t djs_require(djs_runtime_t *rt, const char *name) {
    if (name == NULL) name = "";
    if (rt->module_registry != NULL) {
        djs_value_t cached = djs_object_get(rt, rt->module_registry, name);
        if (cached.tag == DJS_TAG_OBJECT && cached.as.object != NULL) {
            return cached;
        }
    }
    djs_object_t *module = module_create(rt, name);
    if (module == NULL) {
        djs_buf_t buf;
        buf_init(&buf, rt);
        buf_append_cstr(&buf, "Cannot find module '");
        buf_append_cstr(&buf, name);
        buf_append_char(&buf, '\'');
        djs_throw_error(rt, "Error", buf_text(&buf));
    }
    if (rt->module_registry != NULL) {
        djs_property_set(rt, rt->module_registry, name, djs_object_value(module), 0);
    }
    return djs_object_value(module);
}

/* ------------------------------------------------------------------ require */

static djs_value_t require_fn(djs_runtime_t *rt, djs_value_t this_value,
                              djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_require(rt, arg_cstr(rt, args, arg_count, 0));
}

static djs_value_t require_resolve_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    (void)this_value;
    const char *name = arg_cstr(rt, args, arg_count, 0);
    if (module_is_builtin(name)) return str_value(rt, name);

    /* A file path: resolve it against the working directory. */
    if (name[0] == '/') return str_value(rt, path_normalize(rt, &DJS_PATH_POSIX, name));
    const char *cwd = rt->cwd ? rt->cwd : "/";
    djs_buf_t buf;
    buf_init(&buf, rt);
    buf_append_cstr(&buf, cwd);
    buf_append_char(&buf, '/');
    buf_append_cstr(&buf, name);
    return str_value(rt, path_normalize(rt, &DJS_PATH_POSIX, buf_text(&buf)));
}

void djs_install_modules(djs_runtime_t *rt) {
    if (rt == NULL) return;
    if (rt->module_registry == NULL) {
        rt->module_registry = djs_object_new(rt, rt->object_prototype);
    }

    /* The require object is the require function itself, with the Node
     * extras attached to it. */
    djs_object_t *require_object = djs_function_new(rt, require_fn, "require", 1);
    if (require_object == NULL) return;
    rt->require_object = require_object;

    djs_define_method(rt, require_object, "resolve", require_resolve_fn, 1);
    djs_define_value(rt, require_object, "cache",
                     djs_object_value(djs_object_new(rt, rt->object_prototype)));

    djs_object_t *extensions = djs_object_new(rt, rt->object_prototype);
    if (extensions != NULL) {
        djs_define_value(rt, extensions, ".js", str_value(rt, "JavaScript source"));
        djs_define_value(rt, extensions, ".json", str_value(rt, "JSON source"));
        djs_define_value(rt, extensions, ".node", str_value(rt, "native addon"));
        djs_define_value(rt, require_object, "extensions", djs_object_value(extensions));
    }

    djs_object_t *main = djs_object_new(rt, rt->object_prototype);
    if (main != NULL) {
        djs_define_value(rt, main, "filename", str_value(rt, ""));
        djs_define_value(rt, require_object, "main", djs_object_value(main));
    }

    if (rt->global != NULL) {
        if (!djs_object_has(rt->global, "require")) {
            djs_property_set(rt, rt->global, "require", djs_object_value(require_object), 0);
        }
        if (!djs_object_has(rt->global, "process")) {
            djs_property_set(rt, rt->global, "process", djs_require(rt, "process"), 0);
        }
    }
}
