/* djs_builtins.c — standard objects, constructors and global functions.
 *
 * ponytail: builtins use the runtime arena and keep native callbacks
 * stateless. Closure-like state lives in function closure environments or in
 * private object properties.
 */

#include "djs_internal.h"

extern double djs_host_time_ms(void);

#define DJS_PI 3.141592653589793238462643383279502884
#define DJS_E 2.718281828459045235360287471352662498
#define DJS_LN2 0.693147180559945309417232121458176568
#define DJS_LN10 2.302585092994045684017991454684364208

/* --------------------------------------------------------------- utilities */

typedef struct djs_string_chunk {
    struct djs_string_chunk *next;
    size_t length;
    char bytes[1];
} djs_string_chunk_t;

typedef struct {
    djs_runtime_t *rt;
    djs_string_chunk_t *first;
    djs_string_chunk_t *last;
} djs_string_builder_t;

static void sb_init(djs_string_builder_t *builder, djs_runtime_t *rt) {
    builder->rt = rt;
    builder->first = NULL;
    builder->last = NULL;
}

static void sb_append_bytes(djs_string_builder_t *builder, const char *bytes,
                            size_t length) {
    if (length == 0) return;
    size_t capacity = length > 256 ? length : 256;
    djs_string_chunk_t *chunk = (djs_string_chunk_t *)djs_arena_alloc(
        builder->rt, sizeof(*chunk) + capacity);
    if (chunk == NULL) return;
    chunk->next = NULL;
    chunk->length = length;
    djs_memcpy(chunk->bytes, bytes, length);
    if (builder->last == NULL) {
        builder->first = chunk;
    } else {
        builder->last->next = chunk;
    }
    builder->last = chunk;
}

static void sb_append_cstr(djs_string_builder_t *builder, const char *text) {
    sb_append_bytes(builder, text, djs_strlen(text));
}

static void sb_append_char(djs_string_builder_t *builder, char c) {
    sb_append_bytes(builder, &c, 1);
}

static char *sb_finish(djs_string_builder_t *builder) {
    size_t total = 0;
    for (djs_string_chunk_t *chunk = builder->first; chunk != NULL; chunk = chunk->next) {
        total += chunk->length;
    }
    char *result = (char *)djs_arena_alloc(builder->rt, total + 1);
    if (result == NULL) return NULL;
    size_t offset = 0;
    for (djs_string_chunk_t *chunk = builder->first; chunk != NULL; chunk = chunk->next) {
        if (chunk->length > 0) djs_memcpy(result + offset, chunk->bytes, chunk->length);
        offset += chunk->length;
    }
    result[offset] = '\0';
    return result;
}

static bool builtins_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

static bool builtins_digit(char c) { return c >= '0' && c <= '9'; }

static bool same_value_zero(djs_value_t a, djs_value_t b) {
    if (a.tag == DJS_TAG_NUMBER && b.tag == DJS_TAG_NUMBER &&
        a.as.number != a.as.number && b.as.number != b.as.number) {
        return true;
    }
    return djs_strict_equals(a, b);
}

static djs_value_t arg_at(djs_value_t *args, size_t arg_count, size_t index) {
    return index < arg_count ? args[index] : DJS_UNDEFINED;
}

static djs_object_t *require_object(djs_runtime_t *rt, djs_value_t value,
                                    const char *message) {
    if (!djs_is_object(value)) djs_throw_type_error(rt, message);
    return value.as.object;
}

static void define_raw(djs_runtime_t *rt, djs_object_t *object, const char *name,
                       djs_value_t value, uint32_t flags) {
    djs_property_set(rt, object, name, value, flags);
    djs_property_t *property = djs_property_get(object, name);
    if (property != NULL) property->flags = flags;
}

static void define_builtin_method(djs_runtime_t *rt, djs_object_t *object,
                                  const char *name, djs_native_fn fn, int arity) {
    djs_define_method(rt, object, name, fn, arity);
    djs_property_t *property = djs_property_get(object, name);
    if (property != NULL) {
        property->flags = DJS_PROP_WRITABLE | DJS_PROP_CONFIGURABLE;
    }
}

static void define_builtin_value(djs_runtime_t *rt, djs_object_t *object,
                                 const char *name, djs_value_t value) {
    define_raw(rt, object, name, value,
               DJS_PROP_WRITABLE | DJS_PROP_CONFIGURABLE);
}

static djs_value_t make_cstring(djs_runtime_t *rt, const char *text) {
    return djs_string_value(djs_string_new_cstr(rt, text));
}

/* Decode one UTF-8 code point. Returns its byte width, or zero on error. */
static size_t utf8_decode(const char *text, size_t length, size_t offset,
                          uint32_t *code_point) {
    if (offset >= length) return 0;
    unsigned char first = (unsigned char)text[offset];
    if (first < 0x80) {
        *code_point = first;
        return 1;
    }
    if ((first & 0xE0) == 0xC0 && offset + 1 < length) {
        unsigned char second = (unsigned char)text[offset + 1];
        if ((second & 0xC0) != 0x80) return 0;
        *code_point = ((uint32_t)(first & 0x1F) << 6) | (uint32_t)(second & 0x3F);
        return *code_point < 0x80 ? 0 : 2;
    }
    if ((first & 0xF0) == 0xE0 && offset + 2 < length) {
        unsigned char second = (unsigned char)text[offset + 1];
        unsigned char third = (unsigned char)text[offset + 2];
        if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80) return 0;
        *code_point = ((uint32_t)(first & 0x0F) << 12) |
                      ((uint32_t)(second & 0x3F) << 6) | (uint32_t)(third & 0x3F);
        return *code_point < 0x800 ? 0 : 3;
    }
    if ((first & 0xF8) == 0xF0 && offset + 3 < length) {
        unsigned char second = (unsigned char)text[offset + 1];
        unsigned char third = (unsigned char)text[offset + 2];
        unsigned char fourth = (unsigned char)text[offset + 3];
        if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80 ||
            (fourth & 0xC0) != 0x80) {
            return 0;
        }
        *code_point = ((uint32_t)(first & 0x07) << 18) |
                      ((uint32_t)(second & 0x3F) << 12) |
                      ((uint32_t)(third & 0x3F) << 6) | (uint32_t)(fourth & 0x3F);
        return *code_point < 0x10000 || *code_point > 0x10FFFF ? 0 : 4;
    }
    return 0;
}

static size_t utf8_encode(uint32_t code_point, char output[4]) {
    if (code_point < 0x80) {
        output[0] = (char)code_point;
        return 1;
    }
    if (code_point < 0x800) {
        output[0] = (char)(0xC0 | (code_point >> 6));
        output[1] = (char)(0x80 | (code_point & 0x3F));
        return 2;
    }
    if (code_point < 0x10000) {
        output[0] = (char)(0xE0 | (code_point >> 12));
        output[1] = (char)(0x80 | ((code_point >> 6) & 0x3F));
        output[2] = (char)(0x80 | (code_point & 0x3F));
        return 3;
    }
    output[0] = (char)(0xF0 | (code_point >> 18));
    output[1] = (char)(0x80 | ((code_point >> 12) & 0x3F));
    output[2] = (char)(0x80 | ((code_point >> 6) & 0x3F));
    output[3] = (char)(0x80 | (code_point & 0x3F));
    return 4;
}

/* Return true for a canonical array index. */
bool djs_canonical_index(const char *text, size_t *index_out) {
    if (text == NULL || text[0] == '\0') return false;
    if (text[0] == '0' && text[1] != '\0') return false;
    uint64_t value = 0;
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (!builtins_digit(text[i])) return false;
        value = value * 10 + (uint64_t)(text[i] - '0');
        if (value > 0xFFFFFFFEu) return false;
    }
    if (index_out != NULL) *index_out = (size_t)value;
    return true;
}

/* ---------------------------------------------------------- object helpers */

static djs_value_t array_length_of(djs_runtime_t *rt, djs_object_t *object) {
    if (object->class == DJS_CLASS_ARRAY) {
        return djs_number((double)djs_array_length(object));
    }
    return djs_object_get(rt, object, "length");
}

static size_t length_index(djs_runtime_t *rt, djs_object_t *object) {
    djs_value_t length = array_length_of(rt, object);
    if (length.tag != DJS_TAG_NUMBER || length.as.number < 0.0 ||
        length.as.number != (double)(size_t)length.as.number) {
        djs_throw_type_error(rt, "invalid array length");
    }
    return (size_t)length.as.number;
}

static djs_value_t indexed_get(djs_runtime_t *rt, djs_object_t *object,
                               size_t index) {
    if (object->class == DJS_CLASS_ARRAY) {
        return djs_array_get(rt, object, index);
    }
    return djs_get_prop(rt, djs_object_value(object), djs_number((double)index));
}

/* ----------------------------------------------------------- closure state */

static djs_env_t *make_closure(djs_runtime_t *rt) {
    return djs_env_new(rt, rt->global_env);
}

static void closure_set(djs_runtime_t *rt, djs_env_t *closure, const char *name,
                        djs_value_t value) {
    djs_env_declare(rt, closure, name, value, false);
}

static djs_value_t closure_get(djs_runtime_t *rt, const char *name) {
    djs_env_t *closure = NULL;
    if (rt->frame != NULL && rt->frame->function != NULL) {
        closure = rt->frame->function->closure;
    }
    if (closure == NULL) return DJS_UNDEFINED;
    djs_binding_t *binding = djs_env_lookup(closure, name);
    return binding == NULL ? DJS_UNDEFINED : binding->value;
}

static djs_object_t *function_with_closure(djs_runtime_t *rt, djs_native_fn fn,
                                           const char *name, int arity,
                                           djs_env_t *closure) {
    djs_object_t *function = djs_function_new(rt, fn, name, arity);
    if (function != NULL && function->function != NULL) {
        function->function->closure = closure;
    }
    return function;
}

/* -------------------------------------------------------------- iterators */

static djs_value_t iterator_result(djs_runtime_t *rt, djs_value_t value,
                                   bool done) {
    djs_value_t result = djs_make_object(rt);
    if (result.tag != DJS_TAG_OBJECT) return result;
    djs_object_set(rt, result.as.object, "value", value);
    djs_object_set(rt, result.as.object, "done", djs_bool(done));
    return result;
}

static djs_value_t make_iterator(djs_runtime_t *rt, djs_native_fn next,
                                 const char *name, djs_env_t *closure) {
    djs_object_t *iterator = djs_object_new(rt, rt->object_prototype);
    if (iterator == NULL) return DJS_UNDEFINED;
    djs_object_t *function = function_with_closure(rt, next, name, 0, closure);
    if (function != NULL) {
        djs_object_set(rt, iterator, "next", djs_object_value(function));
    }
    return djs_object_value(iterator);
}

/* Generic array iterator. The closure holds the source and next index. */
static djs_value_t array_iterator_next(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    djs_value_t source = closure_get(rt, "source");
    djs_value_t index_value = closure_get(rt, "index");
    if (source.tag != DJS_TAG_OBJECT || index_value.tag != DJS_TAG_NUMBER) {
        return iterator_result(rt, DJS_UNDEFINED, true);
    }
    size_t index = (size_t)index_value.as.number;
    size_t length = length_index(rt, source.as.object);
    closure_set(rt, rt->frame->function->closure, "index",
                djs_number((double)(index + 1)));
    if (index >= length) return iterator_result(rt, DJS_UNDEFINED, true);
    djs_value_t mode = closure_get(rt, "mode");
    if (mode.tag == DJS_TAG_STRING && djs_strcmp(mode.as.string->bytes, "keys") == 0) {
        return iterator_result(rt, djs_number((double)index), false);
    }
    djs_value_t value = indexed_get(rt, source.as.object, index);
    if (mode.tag == DJS_TAG_STRING && djs_strcmp(mode.as.string->bytes, "entries") == 0) {
        djs_value_t pair[2] = { djs_number((double)index), value };
        value = djs_make_array(rt, pair, 2);
    }
    return iterator_result(rt, value, false);
}

/* String iterator over UTF-8 code points. */
static djs_value_t string_iterator_next(djs_runtime_t *rt, djs_value_t this_value,
                                        djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    djs_value_t source = closure_get(rt, "source");
    djs_value_t index_value = closure_get(rt, "index");
    if (source.tag != DJS_TAG_STRING || index_value.tag != DJS_TAG_NUMBER) {
        return iterator_result(rt, DJS_UNDEFINED, true);
    }
    size_t index = (size_t)index_value.as.number;
    if (index >= source.as.string->length) {
        return iterator_result(rt, DJS_UNDEFINED, true);
    }
    uint32_t code_point = 0;
    size_t width = utf8_decode(source.as.string->bytes, source.as.string->length,
                               index, &code_point);
    if (width == 0) width = 1;
    closure_set(rt, rt->frame->function->closure, "index",
                djs_number((double)(index + width)));
    char encoded[4];
    size_t encoded_length = utf8_encode(code_point, encoded);
    return iterator_result(rt,
        djs_string_value(djs_string_new(rt, encoded, encoded_length)), false);
}

static djs_value_t make_array_iterator_mode(djs_runtime_t *rt, djs_value_t source,
                                           const char *mode) {
    djs_env_t *closure = make_closure(rt);
    closure_set(rt, closure, "source", source);
    closure_set(rt, closure, "index", djs_number(0.0));
    closure_set(rt, closure, "mode", make_cstring(rt, mode));
    return make_iterator(rt, array_iterator_next, "next", closure);
}

static djs_value_t make_array_iterator(djs_runtime_t *rt, djs_value_t source) {
    return make_array_iterator_mode(rt, source, "values");
}

static djs_value_t make_string_iterator(djs_runtime_t *rt, djs_value_t source) {
    djs_env_t *closure = make_closure(rt);
    closure_set(rt, closure, "source", source);
    closure_set(rt, closure, "index", djs_number(0.0));
    return make_iterator(rt, string_iterator_next, "next", closure);
}

/* Collect an iterable into an array. */
static djs_value_t iterable_to_array(djs_runtime_t *rt, djs_value_t iterable) {
    djs_value_t result = djs_make_array(rt, NULL, 0);
    if (result.tag != DJS_TAG_OBJECT) return result;
    if (!djs_is_object(iterable)) {
        djs_throw_type_error(rt, "value is not iterable");
    }
    djs_value_t iterator_method = djs_get_prop(rt, iterable,
                                               make_cstring(rt, DJS_ITERATOR_KEY));
    if (!djs_value_is_callable(iterator_method)) {
        djs_throw_type_error(rt, "value is not iterable");
    }
    djs_value_t iterator = djs_call(rt, iterator_method, iterable, NULL, 0);
    for (;;) {
        djs_value_t next = djs_get_prop(rt, iterator, make_cstring(rt, "next"));
        djs_value_t step = djs_call(rt, next, iterator, NULL, 0);
        djs_value_t done = djs_get_prop(rt, step, make_cstring(rt, "done"));
        djs_value_t value = djs_get_prop(rt, step, make_cstring(rt, "value"));
        if (djs_to_boolean(done)) break;
        djs_array_push(rt, result.as.object, value);
    }
    return result;
}

/* ------------------------------------------------------------------ Object */

static djs_value_t object_keys_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_object_t *object = require_object(rt, arg_at(args, arg_count, 0),
                                          "Object.keys called on non-object");
    char **keys = NULL;
    size_t count = djs_object_keys(rt, object, &keys);
    djs_value_t result = djs_make_array(rt, NULL, 0);
    if (result.tag != DJS_TAG_OBJECT) return result;
    for (size_t i = 0; i < count; i++) {
        djs_array_push(rt, result.as.object,
                       make_cstring(rt, keys[i]));
    }
    return result;
}

static djs_value_t object_values_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_object_t *object = require_object(rt, arg_at(args, arg_count, 0),
                                          "Object.values called on non-object");
    char **keys = NULL;
    size_t count = djs_object_keys(rt, object, &keys);
    djs_value_t result = djs_make_array(rt, NULL, 0);
    if (result.tag != DJS_TAG_OBJECT) return result;
    for (size_t i = 0; i < count; i++) {
        djs_array_push(rt, result.as.object,
                       djs_object_get(rt, object, keys[i]));
    }
    return result;
}

static djs_value_t object_entries_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_object_t *object = require_object(rt, arg_at(args, arg_count, 0),
                                          "Object.entries called on non-object");
    char **keys = NULL;
    size_t count = djs_object_keys(rt, object, &keys);
    djs_value_t result = djs_make_array(rt, NULL, 0);
    if (result.tag != DJS_TAG_OBJECT) return result;
    for (size_t i = 0; i < count; i++) {
        djs_value_t pair[2] = { make_cstring(rt, keys[i]),
                                djs_object_get(rt, object, keys[i]) };
        djs_array_push(rt, result.as.object, djs_make_array(rt, pair, 2));
    }
    return result;
}

static djs_value_t object_assign_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value;
    if (arg_count == 0) return DJS_UNDEFINED;
    djs_value_t target = args[0];
    require_object(rt, target, "Object.assign target must be an object");
    for (size_t i = 1; i < arg_count; i++) {
        if (!djs_is_object(args[i])) continue;
        char **keys = NULL;
        size_t count = djs_object_keys(rt, args[i].as.object, &keys);
        for (size_t j = 0; j < count; j++) {
            djs_set_prop(rt, target, make_cstring(rt, keys[j]),
                         djs_object_get(rt, args[i].as.object, keys[j]));
        }
    }
    return target;
}

static djs_value_t object_freeze_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t value = arg_at(args, arg_count, 0);
    if (!djs_is_object(value)) return value;
    djs_object_t *object = value.as.object;
    object->frozen = true;
    object->extensible = false;
    for (djs_property_t *property = object->first_property; property != NULL;
         property = property->next) {
        property->flags &= ~DJS_PROP_WRITABLE;
    }
    return value;
}

static djs_value_t object_is_frozen_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)rt; (void)this_value;
    djs_value_t value = arg_at(args, arg_count, 0);
    return djs_bool(djs_is_object(value) && value.as.object->frozen);
}

static void define_descriptor(djs_runtime_t *rt, djs_object_t *object,
                              const char *key, djs_value_t descriptor) {
    if (!djs_is_object(descriptor)) {
        djs_throw_type_error(rt, "property descriptor must be an object");
    }
    bool enumerable = djs_to_boolean(djs_object_get(rt, descriptor.as.object, "enumerable"));
    bool configurable = djs_to_boolean(djs_object_get(rt, descriptor.as.object, "configurable"));
    djs_value_t getter = djs_object_get(rt, descriptor.as.object, "get");
    djs_value_t setter = djs_object_get(rt, descriptor.as.object, "set");
    if (djs_value_is_callable(getter) || djs_value_is_callable(setter)) {
        djs_define_accessor(rt, object, key,
            djs_value_is_callable(getter) ? getter.as.object->function->native : NULL,
            djs_value_is_callable(setter) ? setter.as.object->function->native : NULL);
        djs_property_t *property = djs_property_get(object, key);
        if (property != NULL) {
            property->flags = (enumerable ? DJS_PROP_ENUMERABLE : 0u) |
                              (configurable ? DJS_PROP_CONFIGURABLE : 0u);
        }
        return;
    }
    bool writable = djs_to_boolean(djs_object_get(rt, descriptor.as.object, "writable"));
    djs_value_t value = djs_object_get(rt, descriptor.as.object, "value");
    uint32_t flags = (enumerable ? DJS_PROP_ENUMERABLE : 0u) |
                     (writable ? DJS_PROP_WRITABLE : 0u) |
                     (configurable ? DJS_PROP_CONFIGURABLE : 0u);
    define_raw(rt, object, key, value, flags);
}

static djs_value_t object_create_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t prototype = arg_at(args, arg_count, 0);
    if (!djs_is_null(prototype) && !djs_is_object(prototype)) {
        djs_throw_type_error(rt, "Object prototype must be an object or null");
    }
    djs_object_t *object = djs_object_new(rt,
        djs_is_object(prototype) ? prototype.as.object : NULL);
    if (object == NULL) return DJS_UNDEFINED;
    if (arg_count > 1 && djs_is_object(args[1])) {
        djs_object_t *properties = args[1].as.object;
        for (djs_property_t *property = properties->first_property; property != NULL;
             property = property->next) {
            if (property->key.tag != DJS_TAG_STRING) continue;
            djs_value_t descriptor = djs_object_get(rt, properties,
                                                    property->key.as.string->bytes);
            define_descriptor(rt, object, property->key.as.string->bytes, descriptor);
        }
    }
    return djs_object_value(object);
}

static djs_value_t object_from_entries_fn(djs_runtime_t *rt, djs_value_t this_value,
                                          djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t result = djs_make_object(rt);
    djs_value_t entries = iterable_to_array(rt, arg_at(args, arg_count, 0));
    for (size_t i = 0; i < djs_array_length(entries.as.object); i++) {
        djs_value_t entry = djs_array_get(rt, entries.as.object, i);
        if (!djs_is_object(entry)) continue;
        djs_value_t key = indexed_get(rt, entry.as.object, 0);
        djs_value_t value = indexed_get(rt, entry.as.object, 1);
        djs_set_prop(rt, result, key, value);
    }
    return result;
}

static djs_value_t object_get_prototype_fn(djs_runtime_t *rt,
                                           djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t value = arg_at(args, arg_count, 0);
    if (!djs_is_object(value)) return DJS_NULL;
    return value.as.object->prototype;
}

static djs_value_t object_set_prototype_fn(djs_runtime_t *rt,
                                           djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t value = arg_at(args, arg_count, 0);
    djs_value_t prototype = arg_at(args, arg_count, 1);
    if (!djs_is_object(value)) djs_throw_type_error(rt, "Object.setPrototypeOf target must be an object");
    if (!djs_is_null(prototype) && !djs_is_object(prototype)) {
        djs_throw_type_error(rt, "Object.setPrototypeOf prototype must be an object or null");
    }
    value.as.object->prototype = prototype;
    return value;
}

static djs_value_t object_get_own_names_fn(djs_runtime_t *rt,
                                            djs_value_t this_value,
                                            djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_object_t *object = require_object(rt, arg_at(args, arg_count, 0),
                                          "Object.getOwnPropertyNames called on non-object");
    djs_value_t result = djs_make_array(rt, NULL, 0);
    if (object->class == DJS_CLASS_ARRAY) {
        for (size_t i = 0; i < djs_array_length(object); i++) {
            char buffer[32];
            size_t length = 0;
            size_t value = i;
            do {
                buffer[length++] = (char)('0' + value % 10);
                value /= 10;
            } while (value != 0);
            char key[32];
            for (size_t j = 0; j < length; j++) key[j] = buffer[length - j - 1];
            key[length] = '\0';
            djs_array_push(rt, result.as.object, make_cstring(rt, key));
        }
        djs_array_push(rt, result.as.object, make_cstring(rt, "length"));
    }
    for (djs_property_t *property = object->first_property; property != NULL;
         property = property->next) {
        if (property->key.tag == DJS_TAG_STRING) {
            djs_array_push(rt, result.as.object,
                djs_string_value(property->key.as.string));
        }
    }
    return result;
}

static djs_value_t object_define_property_fn(djs_runtime_t *rt,
                                             djs_value_t this_value,
                                             djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_object_t *object = require_object(rt, arg_at(args, arg_count, 0),
                                          "Object.defineProperty target must be an object");
    djs_value_t key = arg_at(args, arg_count, 1);
    djs_value_t descriptor = arg_at(args, arg_count, 2);
    const char *name = djs_value_cstr(rt, key);
    define_descriptor(rt, object, name, descriptor);
    return djs_object_value(object);
}

static djs_value_t object_get_descriptor_fn(djs_runtime_t *rt,
                                            djs_value_t this_value,
                                            djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_object_t *object = require_object(rt, arg_at(args, arg_count, 0),
                                          "Object.getOwnPropertyDescriptor called on non-object");
    djs_value_t key = arg_at(args, arg_count, 1);
    const char *name = djs_value_cstr(rt, key);
    djs_property_t *property = djs_property_get(object, name);
    if (property == NULL && object->class == DJS_CLASS_ARRAY) {
        size_t index;
        if (djs_canonical_index(name, &index) && index < djs_array_length(object)) {
            djs_value_t result = djs_make_object(rt);
            djs_object_set(rt, result.as.object, "value",
                           djs_array_get(rt, object, index));
            djs_object_set(rt, result.as.object, "writable", djs_bool(true));
            djs_object_set(rt, result.as.object, "enumerable", djs_bool(true));
            djs_object_set(rt, result.as.object, "configurable", djs_bool(true));
            return result;
        }
    }
    if (property == NULL) return DJS_UNDEFINED;
    djs_value_t result = djs_make_object(rt);
    if (property->flags & DJS_PROP_GETTER) {
        djs_object_set(rt, result.as.object, "get", property->value);
    } else {
        djs_object_set(rt, result.as.object, "value", property->value);
        djs_object_set(rt, result.as.object, "writable",
                       djs_bool((property->flags & DJS_PROP_WRITABLE) != 0));
    }
    djs_object_set(rt, result.as.object, "enumerable",
                   djs_bool((property->flags & DJS_PROP_ENUMERABLE) != 0));
    djs_object_set(rt, result.as.object, "configurable",
                   djs_bool((property->flags & DJS_PROP_CONFIGURABLE) != 0));
    return result;
}

static djs_value_t object_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)this_value;
    if (arg_count > 0 && djs_is_object(args[0])) return args[0];
    return djs_make_object(rt);
}

/* ------------------------------------------------------------------- Array */

static djs_object_t *array_this(djs_runtime_t *rt, djs_value_t this_value) {
    if (!djs_is_object(this_value)) {
        djs_throw_type_error(rt, "Array method called on non-object");
    }
    return this_value.as.object;
}

static djs_value_t array_push_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    for (size_t i = 0; i < arg_count; i++) {
        djs_array_push(rt, array, args[i]);
    }
    return djs_number((double)djs_array_length(array));
}

static djs_value_t array_pop_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *array = array_this(rt, this_value);
    size_t length = djs_array_length(array);
    if (length == 0) return DJS_UNDEFINED;
    djs_value_t value = djs_array_get(rt, array, length - 1);
    djs_array_set_length(rt, array, length - 1);
    return value;
}

static djs_value_t array_shift_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *array = array_this(rt, this_value);
    size_t length = djs_array_length(array);
    if (length == 0) return DJS_UNDEFINED;
    djs_value_t value = djs_array_get(rt, array, 0);
    for (size_t i = 1; i < length; i++) {
        djs_array_set(rt, array, i - 1, djs_array_get(rt, array, i));
    }
    djs_array_set_length(rt, array, length - 1);
    return value;
}

static djs_value_t array_unshift_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    size_t length = djs_array_length(array);
    djs_value_t *values = NULL;
    if (arg_count > 0) {
        values = (djs_value_t *)djs_arena_alloc(rt, arg_count * sizeof(*values));
        if (values == NULL) return djs_number((double)length);
        for (size_t i = 0; i < arg_count; i++) values[i] = args[i];
    }
    for (size_t i = length; i > 0; i--) {
        djs_array_set(rt, array, i + arg_count - 1, djs_array_get(rt, array, i - 1));
    }
    for (size_t i = 0; i < arg_count; i++) djs_array_set(rt, array, i, values[i]);
    return djs_number((double)(length + arg_count));
}

static size_t relative_index(double value, size_t length, bool default_end) {
    if (value != value) return default_end ? length : 0;
    double integer = value < 0.0 ? -__builtin_floor(-value) : __builtin_floor(value);
    if (integer < 0.0) {
        integer += (double)length;
        if (integer < 0.0) integer = 0.0;
    } else if (integer > (double)length) {
        integer = (double)length;
    }
    return (size_t)integer;
}

static djs_value_t array_slice_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    size_t length = djs_array_length(array);
    size_t start = 0;
    size_t end = length;
    if (arg_count > 0) start = relative_index(djs_to_number(rt, args[0]).as.number, length, false);
    if (arg_count > 1) end = relative_index(djs_to_number(rt, args[1]).as.number, length, true);
    if (end < start) end = start;
    djs_value_t result = djs_make_array(rt, NULL, 0);
    for (size_t i = start; i < end; i++) {
        djs_array_push(rt, result.as.object, djs_array_get(rt, array, i));
    }
    return result;
}

static djs_value_t array_splice_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    size_t length = djs_array_length(array);
    size_t start = arg_count > 0
        ? relative_index(djs_to_number(rt, args[0]).as.number, length, false)
        : 0;
    size_t remove = arg_count > 1
        ? (size_t)djs_to_number(rt, args[1]).as.number
        : length - start;
    if (remove > length - start) remove = length - start;
    djs_value_t result = djs_make_array(rt, NULL, 0);
    for (size_t i = 0; i < remove; i++) {
        djs_array_push(rt, result.as.object, djs_array_get(rt, array, start + i));
    }
    size_t insert_count = arg_count > 2 ? arg_count - 2 : 0;
    size_t tail = length - remove;
    for (size_t i = tail; i > start; i--) {
        djs_value_t value = i - 1 < length
            ? djs_array_get(rt, array, i - 1)
            : DJS_UNDEFINED;
        djs_array_set(rt, array, i + insert_count - 1, value);
    }
    for (size_t i = 0; i < insert_count; i++) {
        djs_array_set(rt, array, start + i, args[i + 2]);
    }
    djs_array_set_length(rt, array, tail + insert_count);
    return result;
}

static djs_value_t array_concat_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    djs_value_t result = djs_make_array(rt, NULL, 0);
    djs_value_t source = this_value;
    if (djs_is_object(source) && source.as.object->class == DJS_CLASS_ARRAY) {
        size_t length = djs_array_length(source.as.object);
        for (size_t i = 0; i < length; i++) {
            djs_array_push(rt, result.as.object, djs_array_get(rt, source.as.object, i));
        }
    }
    for (size_t i = 0; i < arg_count; i++) {
        if (djs_is_object(args[i]) && args[i].as.object->class == DJS_CLASS_ARRAY) {
            size_t length = djs_array_length(args[i].as.object);
            for (size_t j = 0; j < length; j++) {
                djs_array_push(rt, result.as.object,
                               djs_array_get(rt, args[i].as.object, j));
            }
        } else {
            djs_array_push(rt, result.as.object, args[i]);
        }
    }
    return result;
}

static djs_value_t array_join_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    const char *separator = ",";
    if (arg_count > 0 && !djs_is_undefined(args[0])) {
        separator = djs_value_cstr(rt, args[0]);
    }
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    size_t length = djs_array_length(array);
    for (size_t i = 0; i < length; i++) {
        if (i > 0) sb_append_cstr(&builder, separator);
        djs_value_t value = djs_array_get(rt, array, i);
        if (!djs_is_nullish(value)) sb_append_cstr(&builder, djs_value_cstr(rt, value));
    }
    return make_cstring(rt, sb_finish(&builder));
}

static djs_value_t array_index_of_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    djs_value_t target = arg_at(args, arg_count, 0);
    size_t length = djs_array_length(array);
    size_t start = arg_count > 1
        ? relative_index(djs_to_number(rt, args[1]).as.number, length, false)
        : 0;
    for (size_t i = start; i < length; i++) {
        if (djs_strict_equals(djs_array_get(rt, array, i), target)) {
            return djs_number((double)i);
        }
    }
    return djs_number(-1.0);
}

static djs_value_t array_last_index_of_fn(djs_runtime_t *rt, djs_value_t this_value,
                                          djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    djs_value_t target = arg_at(args, arg_count, 0);
    size_t length = djs_array_length(array);
    size_t start = arg_count > 1
        ? relative_index(djs_to_number(rt, args[1]).as.number, length, true)
        : length;
    for (size_t i = start + 1; i > 0; i--) {
        if (djs_strict_equals(djs_array_get(rt, array, i - 1), target)) {
            return djs_number((double)(i - 1));
        }
    }
    return djs_number(-1.0);
}

static djs_value_t array_includes_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    djs_value_t target = arg_at(args, arg_count, 0);
    size_t length = djs_array_length(array);
    for (size_t i = 0; i < length; i++) {
        if (same_value_zero(djs_array_get(rt, array, i), target)) return djs_bool(true);
    }
    return djs_bool(false);
}

static djs_value_t call_callback(djs_runtime_t *rt, djs_value_t callback,
                                 djs_value_t this_arg, djs_value_t value,
                                 size_t index, djs_value_t array) {
    if (!djs_value_is_callable(callback)) {
        djs_throw_type_error(rt, "callback is not a function");
    }
    djs_value_t callback_args[3] = { value, djs_number((double)index), array };
    return djs_call(rt, callback, this_arg, callback_args, 3);
}

static djs_value_t array_find_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    djs_value_t callback = arg_at(args, arg_count, 0);
    djs_value_t this_arg = arg_at(args, arg_count, 1);
    for (size_t i = 0; i < djs_array_length(array); i++) {
        djs_value_t value = djs_array_get(rt, array, i);
        if (djs_to_boolean(call_callback(rt, callback, this_arg, value, i,
                                              djs_object_value(array)))) {
            return value;
        }
    }
    return DJS_UNDEFINED;
}

static djs_value_t array_find_index_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    djs_value_t callback = arg_at(args, arg_count, 0);
    djs_value_t this_arg = arg_at(args, arg_count, 1);
    for (size_t i = 0; i < djs_array_length(array); i++) {
        if (djs_to_boolean(call_callback(rt, callback, this_arg,
                                            djs_array_get(rt, array, i), i,
                                            djs_object_value(array)))) {
            return djs_number((double)i);
        }
    }
    return djs_number(-1.0);
}

static djs_value_t array_filter_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    djs_value_t callback = arg_at(args, arg_count, 0);
    djs_value_t this_arg = arg_at(args, arg_count, 1);
    djs_value_t result = djs_make_array(rt, NULL, 0);
    for (size_t i = 0; i < djs_array_length(array); i++) {
        djs_value_t value = djs_array_get(rt, array, i);
        if (djs_to_boolean(call_callback(rt, callback, this_arg, value, i,
                                              djs_object_value(array)))) {
            djs_array_push(rt, result.as.object, value);
        }
    }
    return result;
}

static djs_value_t array_map_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    djs_value_t callback = arg_at(args, arg_count, 0);
    djs_value_t this_arg = arg_at(args, arg_count, 1);
    djs_value_t result = djs_make_array(rt, NULL, 0);
    for (size_t i = 0; i < djs_array_length(array); i++) {
        djs_array_push(rt, result.as.object,
            call_callback(rt, callback, this_arg, djs_array_get(rt, array, i), i,
                          djs_object_value(array)));
    }
    return result;
}

static djs_value_t array_for_each_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    djs_value_t callback = arg_at(args, arg_count, 0);
    djs_value_t this_arg = arg_at(args, arg_count, 1);
    for (size_t i = 0; i < djs_array_length(array); i++) {
        call_callback(rt, callback, this_arg, djs_array_get(rt, array, i), i,
                      djs_object_value(array));
    }
    return DJS_UNDEFINED;
}

static djs_value_t array_reduce_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    djs_value_t callback = arg_at(args, arg_count, 0);
    size_t length = djs_array_length(array);
    size_t index = 0;
    djs_value_t accumulator;
    if (arg_count > 1) {
        accumulator = args[1];
    } else {
        if (length == 0) djs_throw_type_error(rt, "reduce of empty array with no initial value");
        accumulator = djs_array_get(rt, array, 0);
        index = 1;
    }
    for (; index < length; index++) {
        djs_value_t callback_args[4] = {
            accumulator, djs_array_get(rt, array, index),
            djs_number((double)index), djs_object_value(array)
        };
        accumulator = djs_call(rt, callback, DJS_UNDEFINED, callback_args, 4);
    }
    return accumulator;
}

static djs_value_t array_reduce_right_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    djs_value_t callback = arg_at(args, arg_count, 0);
    size_t length = djs_array_length(array);
    size_t index = length;
    djs_value_t accumulator;
    if (arg_count > 1) {
        accumulator = args[1];
    } else {
        if (length == 0) djs_throw_type_error(rt, "reduce of empty array with no initial value");
        index--;
        accumulator = djs_array_get(rt, array, index);
    }
    while (index > 0) {
        index--;
        djs_value_t callback_args[4] = {
            accumulator, djs_array_get(rt, array, index),
            djs_number((double)index), djs_object_value(array)
        };
        accumulator = djs_call(rt, callback, DJS_UNDEFINED, callback_args, 4);
    }
    return accumulator;
}

static djs_value_t array_some_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    djs_value_t callback = arg_at(args, arg_count, 0);
    djs_value_t this_arg = arg_at(args, arg_count, 1);
    for (size_t i = 0; i < djs_array_length(array); i++) {
        if (djs_to_boolean(call_callback(rt, callback, this_arg,
                                            djs_array_get(rt, array, i), i,
                                            djs_object_value(array)))) {
            return djs_bool(true);
        }
    }
    return djs_bool(false);
}

static djs_value_t array_every_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    djs_value_t callback = arg_at(args, arg_count, 0);
    djs_value_t this_arg = arg_at(args, arg_count, 1);
    for (size_t i = 0; i < djs_array_length(array); i++) {
        if (!djs_to_boolean(call_callback(rt, callback, this_arg,
                                         djs_array_get(rt, array, i), i,
                                         djs_object_value(array)))) {
            return djs_bool(false);
        }
    }
    return djs_bool(true);
}

static int compare_array_values(djs_runtime_t *rt, djs_value_t left,
                                djs_value_t right, djs_value_t comparator) {
    if (djs_value_is_callable(comparator)) {
        djs_value_t callback_args[2] = { left, right };
        djs_value_t result = djs_call(rt, comparator, DJS_UNDEFINED,
                                       callback_args, 2);
        double number = djs_to_number(rt, result).as.number;
        if (number != number) return 0;
        return number < 0.0 ? -1 : number > 0.0 ? 1 : 0;
    }
    djs_value_t left_text = djs_to_string(rt, left);
    djs_value_t right_text = djs_to_string(rt, right);
    return djs_strcmp(left_text.as.string->bytes, right_text.as.string->bytes);
}

static djs_value_t array_sort_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    size_t length = djs_array_length(array);
    for (size_t i = 1; i < length; i++) {
        djs_value_t value = djs_array_get(rt, array, i);
        size_t j = i;
        while (j > 0 && compare_array_values(rt, djs_array_get(rt, array, j - 1),
                                              value, arg_at(args, arg_count, 0)) > 0) {
            djs_array_set(rt, array, j, djs_array_get(rt, array, j - 1));
            j--;
        }
        djs_array_set(rt, array, j, value);
    }
    return djs_object_value(array);
}

static djs_value_t array_reverse_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *array = array_this(rt, this_value);
    size_t length = djs_array_length(array);
    for (size_t i = 0; i < length / 2; i++) {
        djs_value_t left = djs_array_get(rt, array, i);
        djs_array_set(rt, array, i, djs_array_get(rt, array, length - i - 1));
        djs_array_set(rt, array, length - i - 1, left);
    }
    return djs_object_value(array);
}

static djs_value_t array_fill_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    size_t length = djs_array_length(array);
    djs_value_t value = arg_at(args, arg_count, 0);
    size_t start = arg_count > 1
        ? relative_index(djs_to_number(rt, args[1]).as.number, length, false) : 0;
    size_t end = arg_count > 2
        ? relative_index(djs_to_number(rt, args[2]).as.number, length, true) : length;
    for (size_t i = start; i < end; i++) djs_array_set(rt, array, i, value);
    return djs_object_value(array);
}

static void flatten_array(djs_runtime_t *rt, djs_object_t *source,
                          djs_object_t *target, size_t depth) {
    for (size_t i = 0; i < djs_array_length(source); i++) {
        djs_value_t value = djs_array_get(rt, source, i);
        if (depth > 0 && djs_is_object(value) &&
            value.as.object->class == DJS_CLASS_ARRAY) {
            flatten_array(rt, value.as.object, target, depth - 1);
        } else {
            djs_array_push(rt, target, value);
        }
    }
}

static djs_value_t array_flat_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    double requested = arg_count > 0 ? djs_to_number(rt, args[0]).as.number : 1.0;
    size_t depth = requested <= 0.0 ? 0 : (size_t)requested;
    djs_value_t result = djs_make_array(rt, NULL, 0);
    flatten_array(rt, array, result.as.object, depth);
    return result;
}

static djs_value_t array_flat_map_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    djs_value_t callback = arg_at(args, arg_count, 0);
    djs_value_t this_arg = arg_at(args, arg_count, 1);
    djs_value_t result = djs_make_array(rt, NULL, 0);
    for (size_t i = 0; i < djs_array_length(array); i++) {
        djs_value_t mapped = call_callback(rt, callback, this_arg,
                                           djs_array_get(rt, array, i), i,
                                           djs_object_value(array));
        if (djs_is_object(mapped) && mapped.as.object->class == DJS_CLASS_ARRAY) {
            flatten_array(rt, mapped.as.object, result.as.object, 0);
        } else {
            djs_array_push(rt, result.as.object, mapped);
        }
    }
    return result;
}

static djs_value_t array_at_fn(djs_runtime_t *rt, djs_value_t this_value,
                               djs_value_t *args, size_t arg_count) {
    djs_object_t *array = array_this(rt, this_value);
    size_t length = djs_array_length(array);
    double index = djs_to_number(rt, arg_at(args, arg_count, 0)).as.number;
    if (index < 0.0) index += (double)length;
    if (index < 0.0 || index >= (double)length || index != index) return DJS_UNDEFINED;
    return djs_array_get(rt, array, (size_t)index);
}

static djs_value_t array_keys_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    return make_array_iterator_mode(rt, this_value, "keys");
}

static djs_value_t array_values_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    return make_array_iterator_mode(rt, this_value, "values");
}

static djs_value_t array_entries_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    return make_array_iterator_mode(rt, this_value, "entries");
}

static djs_value_t array_from_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t source = arg_at(args, arg_count, 0);
    djs_value_t callback = arg_at(args, arg_count, 1);
    djs_value_t result = djs_make_array(rt, NULL, 0);
    djs_value_t items;
    if (djs_is_object(source)) {
        djs_value_t iterator = djs_get_prop(rt, source,
                                             make_cstring(rt, DJS_ITERATOR_KEY));
        if (djs_value_is_callable(iterator)) {
            items = iterable_to_array(rt, source);
        } else {
            size_t length = length_index(rt, source.as.object);
            items = djs_make_array(rt, NULL, 0);
            for (size_t i = 0; i < length; i++) {
                djs_array_push(rt, items.as.object,
                               indexed_get(rt, source.as.object, i));
            }
        }
    } else {
        items = djs_make_array(rt, NULL, 0);
    }
    for (size_t i = 0; i < djs_array_length(items.as.object); i++) {
        djs_value_t value = djs_array_get(rt, items.as.object, i);
        if (djs_value_is_callable(callback)) {
            djs_value_t callback_args[2] = { value, djs_number((double)i) };
            value = djs_call(rt, callback, DJS_UNDEFINED, callback_args, 2);
        }
        djs_array_push(rt, result.as.object, value);
    }
    return result;
}

static djs_value_t array_of_fn(djs_runtime_t *rt, djs_value_t this_value,
                               djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t result = djs_make_array(rt, NULL, 0);
    for (size_t i = 0; i < arg_count; i++) djs_array_push(rt, result.as.object, args[i]);
    return result;
}

static djs_value_t array_is_array_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)rt; (void)this_value;
    djs_value_t value = arg_at(args, arg_count, 0);
    return djs_bool(djs_is_object(value) && value.as.object->class == DJS_CLASS_ARRAY);
}

static djs_value_t array_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                        djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t result = djs_make_array(rt, NULL, 0);
    if (arg_count == 1 && djs_is_number(args[0])) {
        double length = args[0].as.number;
        if (length < 0.0 || length != (double)(size_t)length ||
            length > 4294967295.0) {
            djs_throw_error(rt, "RangeError", "invalid array length");
        }
        djs_array_set_length(rt, result.as.object, (size_t)length);
    } else {
        for (size_t i = 0; i < arg_count; i++) {
            djs_array_push(rt, result.as.object, args[i]);
        }
    }
    return result;
}

/* ------------------------------------------------------------------ String */

static djs_string_t *string_this(djs_runtime_t *rt, djs_value_t this_value) {
    djs_value_t value = djs_to_string(rt, this_value);
    if (value.tag != DJS_TAG_STRING) {
        djs_throw_type_error(rt, "String method called on incompatible receiver");
    }
    return value.as.string;
}

static djs_value_t string_char_at_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    double index = djs_to_number(rt, arg_at(args, arg_count, 0)).as.number;
    if (index < 0.0 || index >= (double)string->length || index != index) {
        return make_cstring(rt, "");
    }
    uint32_t code_point = 0;
    size_t width = utf8_decode(string->bytes, string->length, (size_t)index,
                               &code_point);
    if (width == 0) width = 1;
    return djs_string_value(djs_string_new(rt, string->bytes + (size_t)index, width));
}

static djs_value_t string_char_code_at_fn(djs_runtime_t *rt, djs_value_t this_value,
                                          djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    double index = djs_to_number(rt, arg_at(args, arg_count, 0)).as.number;
    if (index < 0.0 || index >= (double)string->length || index != index) {
        return djs_number(0.0 / 0.0);
    }
    uint32_t code_point = 0;
    size_t width = utf8_decode(string->bytes, string->length, (size_t)index,
                               &code_point);
    if (width == 0) return djs_number(0.0 / 0.0);
    return djs_number((double)code_point);
}

static djs_value_t string_code_point_at_fn(djs_runtime_t *rt, djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    return string_char_code_at_fn(rt, this_value, args, arg_count);
}

static size_t string_find(djs_string_t *string, const char *needle, size_t from) {
    if (needle[0] == '\0') return from <= string->length ? from : string->length;
    size_t needle_length = djs_strlen(needle);
    if (needle_length > string->length) return string->length;
    for (size_t i = from; i + needle_length <= string->length; i++) {
        if (djs_memcmp_helper(string->bytes + i, needle, needle_length) == 0) return i;
    }
    return string->length;
}

static djs_value_t string_index_of_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    const char *needle = djs_value_cstr(rt, arg_at(args, arg_count, 0));
    double from = arg_count > 1 ? djs_to_number(rt, args[1]).as.number : 0.0;
    if (from < 0.0) from = 0.0;
    if (from > (double)string->length) return djs_number(-1.0);
    size_t found = string_find(string, needle, (size_t)from);
    return djs_number(found == string->length ? -1.0 : (double)found);
}

static djs_value_t string_last_index_of_fn(djs_runtime_t *rt, djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    const char *needle = djs_value_cstr(rt, arg_at(args, arg_count, 0));
    size_t needle_length = djs_strlen(needle);
    double from = arg_count > 1 ? djs_to_number(rt, args[1]).as.number
                                : (double)string->length;
    if (from < 0.0) from = 0.0;
    if (from > (double)string->length) from = (double)string->length;
    if (needle_length > string->length) return djs_number(-1.0);
    for (size_t i = (size_t)from + 1; i > 0; i--) {
        if (i <= string->length &&
            djs_memcmp_helper(string->bytes + i - 1, needle, needle_length) == 0) {
            return djs_number((double)(i - 1));
        }
    }
    return djs_number(-1.0);
}

static djs_value_t string_includes_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    const char *needle = djs_value_cstr(rt, arg_at(args, arg_count, 0));
    double from = arg_count > 1 ? djs_to_number(rt, args[1]).as.number : 0.0;
    if (from < 0.0) from = 0.0;
    size_t found = string_find(string, needle, (size_t)from);
    return djs_bool(found < string->length);
}

static djs_value_t string_starts_with_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    const char *prefix = djs_value_cstr(rt, arg_at(args, arg_count, 0));
    size_t prefix_length = djs_strlen(prefix);
    double from = arg_count > 1 ? djs_to_number(rt, args[1]).as.number : 0.0;
    if (from < 0.0) from = 0.0;
    if ((size_t)from + prefix_length > string->length) return djs_bool(false);
    return djs_bool(djs_memcmp_helper(string->bytes + (size_t)from, prefix,
                                     prefix_length) == 0);
}

static djs_value_t string_ends_with_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    const char *suffix = djs_value_cstr(rt, arg_at(args, arg_count, 0));
    size_t suffix_length = djs_strlen(suffix);
    double end = arg_count > 1 ? djs_to_number(rt, args[1]).as.number
                               : (double)string->length;
    if (end < 0.0) end = 0.0;
    if (end > (double)string->length) end = (double)string->length;
    if ((size_t)end < suffix_length) return djs_bool(false);
    return djs_bool(djs_memcmp_helper(string->bytes + (size_t)end - suffix_length,
                                     suffix, suffix_length) == 0);
}

static djs_value_t string_slice_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    size_t length = string->length;
    size_t start = arg_count > 0
        ? relative_index(djs_to_number(rt, args[0]).as.number, length, false) : 0;
    size_t end = arg_count > 1
        ? relative_index(djs_to_number(rt, args[1]).as.number, length, true) : length;
    if (end < start) end = start;
    return djs_string_value(djs_string_new(rt, string->bytes + start, end - start));
}

static djs_value_t string_substring_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    size_t length = string->length;
    double first = arg_count > 0 ? djs_to_number(rt, args[0]).as.number : 0.0;
    double second = arg_count > 1 ? djs_to_number(rt, args[1]).as.number
                                  : (double)length;
    if (first != first || first < 0.0) first = 0.0;
    if (second != second || second < 0.0) second = 0.0;
    if (first > (double)length) first = (double)length;
    if (second > (double)length) second = (double)length;
    size_t start = (size_t)(first < second ? first : second);
    size_t end = (size_t)(first < second ? second : first);
    return djs_string_value(djs_string_new(rt, string->bytes + start, end - start));
}

static djs_value_t string_substr_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    size_t length = string->length;
    double start = arg_count > 0 ? djs_to_number(rt, args[0]).as.number : 0.0;
    if (start < 0.0) start += (double)length;
    if (start < 0.0) start = 0.0;
    if (start > (double)length) return make_cstring(rt, "");
    size_t count = arg_count > 1 ? (size_t)djs_to_number(rt, args[1]).as.number
                                : length - (size_t)start;
    if (count > length - (size_t)start) count = length - (size_t)start;
    return djs_string_value(djs_string_new(rt, string->bytes + (size_t)start, count));
}

static djs_value_t string_case_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count, bool upper) {
    (void)args; (void)arg_count;
    djs_string_t *string = string_this(rt, this_value);
    char *copy = (char *)djs_arena_alloc(rt, string->length + 1);
    if (copy == NULL) return DJS_UNDEFINED;
    for (size_t i = 0; i < string->length; i++) {
        char c = string->bytes[i];
        if (upper && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (!upper && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        copy[i] = c;
    }
    copy[string->length] = '\0';
    return djs_string_value(djs_string_new(rt, copy, string->length));
}

static djs_value_t string_to_upper_case_fn(djs_runtime_t *rt, djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    return string_case_fn(rt, this_value, args, arg_count, true);
}

static djs_value_t string_to_lower_case_fn(djs_runtime_t *rt, djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    return string_case_fn(rt, this_value, args, arg_count, false);
}

static djs_value_t string_trim_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count, int mode) {
    (void)args; (void)arg_count;
    djs_string_t *string = string_this(rt, this_value);
    size_t start = 0;
    size_t end = string->length;
    if (mode != 2) while (start < end && builtins_space(string->bytes[start])) start++;
    if (mode != 1) while (end > start && builtins_space(string->bytes[end - 1])) end--;
    return djs_string_value(djs_string_new(rt, string->bytes + start, end - start));
}

static djs_value_t string_trim_start_fn(djs_runtime_t *rt, djs_value_t this_value,
                                        djs_value_t *args, size_t arg_count) {
    return string_trim_fn(rt, this_value, args, arg_count, 1);
}

static djs_value_t string_trim_end_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    return string_trim_fn(rt, this_value, args, arg_count, 2);
}

static djs_value_t string_trim_default_fn(djs_runtime_t *rt, djs_value_t this_value,
                                          djs_value_t *args, size_t arg_count) {
    return string_trim_fn(rt, this_value, args, arg_count, 0);
}

static djs_value_t string_split_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    djs_value_t separator = arg_at(args, arg_count, 0);
    djs_value_t limit_value = arg_at(args, arg_count, 1);
    size_t limit = djs_is_undefined(limit_value)
        ? (size_t)-1
        : (size_t)djs_to_number(rt, limit_value).as.number;
    djs_value_t result = djs_make_array(rt, NULL, 0);
    if (djs_is_undefined(separator)) {
        djs_array_push(rt, result.as.object, djs_string_value(string));
        return result;
    }
    const char *separator_text = djs_value_cstr(rt, separator);
    size_t separator_length = djs_strlen(separator_text);
    size_t start = 0;
    while (limit != 0) {
        if (separator_length == 0) {
            if (start >= string->length) break;
            uint32_t code_point = 0;
            size_t width = utf8_decode(string->bytes, string->length, start,
                                       &code_point);
            if (width == 0) width = 1;
            djs_array_push(rt, result.as.object,
                djs_string_value(djs_string_new(rt, string->bytes + start, width)));
            start += width;
        } else {
            size_t found = string_find(string, separator_text, start);
            if (found == string->length) break;
            djs_array_push(rt, result.as.object,
                djs_string_value(djs_string_new(rt, string->bytes + start,
                                                found - start)));
            start = found + separator_length;
        }
        if (limit != (size_t)-1) limit--;
    }
    if (limit != 0) {
        djs_array_push(rt, result.as.object,
            djs_string_value(djs_string_new(rt, string->bytes + start,
                                            string->length - start)));
    }
    return result;
}

static djs_value_t string_repeat_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    double count = djs_to_number(rt, arg_at(args, arg_count, 0)).as.number;
    if (count < 0.0 || count != count) {
        djs_throw_error(rt, "RangeError", "invalid count value");
    }
    size_t repetitions = (size_t)count;
    size_t total = string->length * repetitions;
    char *copy = (char *)djs_arena_alloc(rt, total + 1);
    if (copy == NULL) return DJS_UNDEFINED;
    for (size_t i = 0; i < repetitions; i++) {
        if (string->length > 0) djs_memcpy(copy + i * string->length,
                                           string->bytes, string->length);
    }
    copy[total] = '\0';
    return djs_string_value(djs_string_new(rt, copy, total));
}

static djs_value_t string_pad_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count, bool start) {
    djs_string_t *string = string_this(rt, this_value);
    double requested = djs_to_number(rt, arg_at(args, arg_count, 0)).as.number;
    if (requested < 0.0 || requested != requested) {
        djs_throw_error(rt, "RangeError", "invalid target length");
    }
    size_t target = (size_t)requested;
    if (target <= string->length) return djs_string_value(string);
    const char *filler = arg_count > 1 ? djs_value_cstr(rt, args[1]) : " ";
    if (filler[0] == '\0') return djs_string_value(string);
    size_t padding = target - string->length;
    char *copy = (char *)djs_arena_alloc(rt, target + 1);
    if (copy == NULL) return DJS_UNDEFINED;
    if (start) {
        for (size_t i = 0; i < padding; i++) copy[i] = filler[i % djs_strlen(filler)];
        djs_memcpy(copy + padding, string->bytes, string->length);
    } else {
        djs_memcpy(copy, string->bytes, string->length);
        for (size_t i = 0; i < padding; i++) {
            copy[string->length + i] = filler[i % djs_strlen(filler)];
        }
    }
    copy[target] = '\0';
    return djs_string_value(djs_string_new(rt, copy, target));
}

static djs_value_t string_pad_start_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    return string_pad_fn(rt, this_value, args, arg_count, true);
}

static djs_value_t string_pad_end_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    return string_pad_fn(rt, this_value, args, arg_count, false);
}

static djs_value_t string_concat_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    djs_value_t first = djs_to_string(rt, this_value);
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    sb_append_cstr(&builder, first.as.string->bytes);
    for (size_t i = 0; i < arg_count; i++) {
        djs_value_t text = djs_to_string(rt, args[i]);
        sb_append_cstr(&builder, text.as.string->bytes);
    }
    return make_cstring(rt, sb_finish(&builder));
}

static djs_value_t replace_string(djs_runtime_t *rt, djs_string_t *string,
                                  const char *needle, const char *replacement,
                                  bool all) {
    size_t needle_length = djs_strlen(needle);
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    size_t start = 0;
    while (start <= string->length) {
        size_t found = string_find(string, needle, start);
        if (found == string->length) break;
        sb_append_bytes(&builder, string->bytes + start, found - start);
        sb_append_cstr(&builder, replacement);
        if (needle_length == 0) {
            if (found < string->length) sb_append_char(&builder, string->bytes[found]);
            start = found + 1;
        } else {
            start = found + needle_length;
        }
        if (!all) break;
    }
    if (start <= string->length) {
        sb_append_bytes(&builder, string->bytes + start, string->length - start);
    }
    return make_cstring(rt, sb_finish(&builder));
}

static djs_value_t string_replace_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    const char *needle = djs_value_cstr(rt, arg_at(args, arg_count, 0));
    const char *replacement = djs_value_cstr(rt, arg_at(args, arg_count, 1));
    return replace_string(rt, string, needle, replacement, false);
}

static djs_value_t string_replace_all_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    const char *needle = djs_value_cstr(rt, arg_at(args, arg_count, 0));
    const char *replacement = djs_value_cstr(rt, arg_at(args, arg_count, 1));
    return replace_string(rt, string, needle, replacement, true);
}

static djs_value_t string_search_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    const char *needle = djs_value_cstr(rt, arg_at(args, arg_count, 0));
    size_t found = string_find(string, needle, 0);
    return djs_number(found == string->length ? -1.0 : (double)found);
}

static djs_value_t string_at_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    djs_string_t *string = string_this(rt, this_value);
    double index = djs_to_number(rt, arg_at(args, arg_count, 0)).as.number;
    if (index < 0.0) index += (double)string->length;
    if (index < 0.0 || index >= (double)string->length || index != index) {
        return DJS_UNDEFINED;
    }
    uint32_t code_point = 0;
    size_t width = utf8_decode(string->bytes, string->length, (size_t)index,
                               &code_point);
    if (width == 0) width = 1;
    return djs_string_value(djs_string_new(rt, string->bytes + (size_t)index, width));
}

static djs_value_t string_from_char_code_fn(djs_runtime_t *rt, djs_value_t this_value,
                                            djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    for (size_t i = 0; i < arg_count; i++) {
        uint32_t code = (uint32_t)djs_to_number(rt, args[i]).as.number;
        char encoded[4];
        size_t length = utf8_encode(code & 0xFFFFu, encoded);
        sb_append_bytes(&builder, encoded, length);
    }
    return make_cstring(rt, sb_finish(&builder));
}

static djs_value_t string_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)this_value;
    if (arg_count == 0) return make_cstring(rt, "");
    return djs_to_string(rt, args[0]);
}

/* ------------------------------------------------------------------ Number */

/* Numeric helpers. The runtime links no math library, so the arithmetic the
 * builtins need lives here. */

static double num_abs(double value) {
    return value < 0.0 ? -value : value;
}

static double num_floor(double value) {
    if (value != value) return value;
    if (value >= 9.2233720368547758e18 || value <= -9.2233720368547758e18) return value;
    double truncated = (double)(long long)value;
    return truncated > value ? truncated - 1.0 : truncated;
}

static double num_ceil(double value) {
    if (value != value) return value;
    if (value >= 9.2233720368547758e18 || value <= -9.2233720368547758e18) return value;
    double truncated = (double)(long long)value;
    return truncated < value ? truncated + 1.0 : truncated;
}

static double num_trunc(double value) {
    if (value != value) return value;
    if (value >= 9.2233720368547758e18 || value <= -9.2233720368547758e18) return value;
    return (double)(long long)value;
}

static double num_round(double value) {
    if (value != value) return value;
    if (value >= 9.007199254740992e15 || value <= -9.007199254740992e15) return value;
    double floor_value = num_floor(value);
    if (value - floor_value < 0.5) return floor_value;
    double result = floor_value + 1.0;
    if (result == 0.0 && value < 0.0) return -0.0;
    return result;
}

static double num_sqrt(double value) {
    if (value != value || value < 0.0) return 0.0 / 0.0;
    if (value == 0.0 || value == 1.0 / 0.0) return value;
    double guess = value > 1.0 ? value / 2.0 : 1.0;
    for (int i = 0; i < 80; i++) {
        double next = 0.5 * (guess + value / guess);
        if (next == guess) break;
        guess = next;
    }
    return guess;
}

static double num_ln(double value) {
    if (value != value) return value;
    if (value < 0.0) return 0.0 / 0.0;
    if (value == 0.0) return -1.0 / 0.0;
    if (value > 1.7976931348623157e308) return value;
    double mantissa = value;
    int exponent = 0;
    while (mantissa >= 2.0) { mantissa *= 0.5; exponent++; }
    while (mantissa < 1.0) { mantissa *= 2.0; exponent--; }
    double t = (mantissa - 1.0) / (mantissa + 1.0);
    double squared = t * t;
    double term = t;
    double sum = 0.0;
    for (int i = 0; i < 40; i++) {
        sum += term / (double)(2 * i + 1);
        term *= squared;
    }
    return 2.0 * sum + (double)exponent * DJS_LN2;
}

static double num_exp(double value) {
    if (value != value) return value;
    if (value > 709.782712893384) return 1.0 / 0.0;
    if (value < -745.1332191019411) return 0.0;
    double quotient = num_round(value / DJS_LN2);
    double remainder = value - quotient * DJS_LN2;
    double term = 1.0;
    double sum = 1.0;
    for (int i = 1; i <= 24; i++) {
        term *= remainder / (double)i;
        sum += term;
    }
    int exponent = (int)quotient;
    double scale = 1.0;
    while (exponent > 0) { scale *= 2.0; exponent--; }
    while (exponent < 0) { scale *= 0.5; exponent++; }
    return sum * scale;
}

static double num_pow(double base, double exponent) {
    if (exponent != exponent) return exponent;
    if (exponent == 0.0) return 1.0;
    if (base != base) return base;
    if (base == 0.0) return exponent < 0.0 ? 1.0 / 0.0 : 0.0;
    if (base == 1.0) return 1.0;
    if (base == 1.0 / 0.0 || base == -1.0 / 0.0) {
        if (exponent == num_trunc(exponent) && num_abs(exponent) < 1.0e9) {
            long long power = (long long)exponent;
            bool negative = power < 0;
            if (negative) power = -power;
            double result = (power % 2) != 0 ? base : 1.0;
            return negative ? 1.0 / result : result;
        }
        return 1.0 / 0.0;
    }
    if (exponent == num_trunc(exponent) && num_abs(exponent) <= 1024.0) {
        long long power = (long long)exponent;
        bool negative = power < 0;
        if (negative) power = -power;
        double result = 1.0;
        double factor = base;
        while (power > 0) {
            if ((power & 1) != 0) result *= factor;
            factor *= factor;
            power >>= 1;
        }
        return negative ? 1.0 / result : result;
    }
    if (base < 0.0) return 0.0 / 0.0;
    return num_exp(exponent * num_ln(base));
}

/* The exponentiation operator shares the Math.pow implementation. */
double djs_math_pow(double base, double exponent) {
    return num_pow(base, exponent);
}

static double num_sin(double value) {
    if (value != value) return value;
    if (value > 1.7976931348623157e308) return 0.0 / 0.0;
    const double two_pi = 6.283185307179586476925286766559;
    double reduced = value - num_trunc(value / two_pi) * two_pi;
    double squared = reduced * reduced;
    double term = reduced;
    double sum = reduced;
    for (int i = 1; i <= 12; i++) {
        term *= -squared / (double)((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}

static double num_cos(double value) {
    if (value != value) return value;
    if (value > 1.7976931348623157e308) return 0.0 / 0.0;
    const double two_pi = 6.283185307179586476925286766559;
    double reduced = value - num_trunc(value / two_pi) * two_pi;
    double squared = reduced * reduced;
    double term = 1.0;
    double sum = 1.0;
    for (int i = 1; i <= 12; i++) {
        term *= -squared / (double)((2 * i - 1) * (2 * i));
        sum += term;
    }
    return sum;
}

static double num_tan(double value) {
    double cosine = num_cos(value);
    if (cosine == 0.0) return 0.0 / 0.0;
    return num_sin(value) / cosine;
}

static double num_atan_series(double value) {
    double squared = value * value;
    double term = value;
    double sum = 0.0;
    for (int i = 0; i < 40; i++) {
        double divisor = (double)(2 * i + 1);
        sum += (i % 2 == 0 ? term : -term) / divisor;
        term *= squared;
    }
    return sum;
}

static double num_atan(double value) {
    if (value != value) return value;
    const double half_pi = 1.5707963267948966192313216916398;
    if (value > 1.7976931348623157e308) return half_pi;
    bool negative = value < 0.0;
    double x = negative ? -value : value;
    bool inverted = false;
    if (x > 1.0) { x = 1.0 / x; inverted = true; }
    double result;
    if (x > 0.5) {
        result = 0.78539816339744830961566084581988 -
                 num_atan_series((1.0 - x) / (1.0 + x));
    } else {
        result = num_atan_series(x);
    }
    if (inverted) result = half_pi - result;
    return negative ? -result : result;
}

static double num_atan2(double y, double x) {
    if (y != y || x != x) return 0.0 / 0.0;
    const double pi = 3.1415926535897932384626433832795;
    if (x > 0.0) return num_atan(y / x);
    if (x < 0.0) return y >= 0.0 ? num_atan(y / x) + pi : num_atan(y / x) - pi;
    if (y > 0.0) return pi / 2.0;
    if (y < 0.0) return -pi / 2.0;
    return 0.0;
}

static double num_cbrt(double value) {
    if (value != value) return value;
    if (value == 0.0) return value;
    bool negative = value < 0.0;
    double x = negative ? -value : value;
    double guess = x > 1.0 ? x / 3.0 : 1.0;
    for (int i = 0; i < 100; i++) {
        double next = (2.0 * guess + x / (guess * guess)) / 3.0;
        if (next == guess) break;
        guess = next;
    }
    return negative ? -guess : guess;
}

static double num_hypot(double a, double b) {
    double x = num_abs(a);
    double y = num_abs(b);
    if (x != x || y != y) return 0.0 / 0.0;
    if (x > 1.7976931348623157e308 || y > 1.7976931348623157e308) return 1.0 / 0.0;
    double largest = x > y ? x : y;
    if (largest == 0.0) return 0.0;
    double ratio_x = x / largest;
    double ratio_y = y / largest;
    return largest * num_sqrt(ratio_x * ratio_x + ratio_y * ratio_y);
}

static int digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

static size_t format_u64(uint64_t value, char *out) {
    char reverse[24];
    size_t count = 0;
    if (value == 0) { out[0] = '0'; out[1] = '\0'; return 1; }
    while (value > 0 && count < sizeof(reverse)) {
        reverse[count++] = (char)('0' + (value % 10));
        value /= 10;
    }
    for (size_t i = 0; i < count; i++) out[i] = reverse[count - i - 1];
    out[count] = '\0';
    return count;
}

static size_t format_radix_integer(uint64_t value, int radix, char *out) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char reverse[72];
    size_t count = 0;
    if (value == 0) { out[0] = '0'; out[1] = '\0'; return 1; }
    while (value > 0 && count < sizeof(reverse)) {
        reverse[count++] = digits[value % (uint64_t)radix];
        value /= (uint64_t)radix;
    }
    for (size_t i = 0; i < count; i++) out[i] = reverse[count - i - 1];
    out[count] = '\0';
    return count;
}

static int decimal_exponent(double magnitude) {
    if (magnitude == 0.0) return 0;
    int exponent = 0;
    double value = magnitude;
    while (value >= 10.0) { value /= 10.0; exponent++; }
    while (value < 1.0) { value *= 10.0; exponent--; }
    return exponent;
}

static double pow10_int(int exponent) {
    double result = 1.0;
    if (exponent >= 0) {
        for (int i = 0; i < exponent; i++) result *= 10.0;
    } else {
        for (int i = 0; i > exponent; i--) result *= 0.1;
    }
    return result;
}

/* Extract `count` significant digits. The value is 0.digits * 10^(exponent+1). */
static size_t extract_significant(double magnitude, int count, int *exponent_out,
                                  char *digits) {
    int exponent = decimal_exponent(magnitude);
    double scaled = magnitude / pow10_int(exponent);
    for (int i = 0; i < count; i++) {
        int digit = (int)scaled;
        if (digit < 0) digit = 0;
        if (digit > 9) digit = 9;
        digits[i] = (char)('0' + digit);
        scaled = (scaled - (double)digit) * 10.0;
    }
    if (scaled >= 5.0) {
        int index = count - 1;
        while (index >= 0 && digits[index] == '9') { digits[index] = '0'; index--; }
        if (index >= 0) {
            digits[index]++;
        } else {
            digits[0] = '1';
            exponent++;
        }
    }
    *exponent_out = exponent;
    return (size_t)count;
}

static size_t format_significant_fixed(const char *digits, int count, int exponent,
                                       char *out) {
    size_t length = 0;
    if (exponent < 0) {
        out[length++] = '0';
        out[length++] = '.';
        for (int i = 0; i < -exponent - 1; i++) out[length++] = '0';
        for (int i = 0; i < count; i++) out[length++] = digits[i];
    } else {
        int integer_digits = exponent + 1;
        for (int i = 0; i < integer_digits; i++) {
            out[length++] = i < count ? digits[i] : '0';
        }
        if (count > integer_digits) {
            out[length++] = '.';
            for (int i = integer_digits; i < count; i++) out[length++] = digits[i];
        }
    }
    out[length] = '\0';
    return length;
}

static size_t format_significant_exponential(const char *digits, int count,
                                             int exponent, char *out) {
    size_t length = 0;
    out[length++] = digits[0];
    if (count > 1) {
        out[length++] = '.';
        for (int i = 1; i < count; i++) out[length++] = digits[i];
    }
    length += (size_t)djs_format_exponent(out + length, exponent);
    out[length] = '\0';
    return length;
}

/* The shortest digit sequence that reads back as the same double. */
static size_t shortest_significant(double magnitude, char *digits, int *exponent_out) {
    char text[64];
    for (int count = 1; count <= 17; count++) {
        int exponent = 0;
        extract_significant(magnitude, count, &exponent, digits);
        format_significant_exponential(digits, count, exponent, text);
        double parsed = 0.0;
        if (djs_string_to_number(text, djs_strlen(text), &parsed) && parsed == magnitude) {
            while (count > 1 && digits[count - 1] == '0') count--;
            *exponent_out = exponent;
            return (size_t)count;
        }
    }
    int exponent = 0;
    extract_significant(magnitude, 17, &exponent, digits);
    *exponent_out = exponent;
    return 17;
}

/* Fixed point text with exactly `digits` decimals. */
static size_t format_fixed(double magnitude, int digits, char *out) {
    char fraction[128];
    double integer = num_trunc(magnitude);
    double rest = magnitude - integer;
    for (int i = 0; i < digits; i++) {
        rest *= 10.0;
        int digit = (int)rest;
        if (digit < 0) digit = 0;
        if (digit > 9) digit = 9;
        fraction[i] = (char)('0' + digit);
        rest -= (double)digit;
    }
    if (rest >= 0.5) {
        int index = digits - 1;
        while (index >= 0 && fraction[index] == '9') { fraction[index] = '0'; index--; }
        if (index >= 0) fraction[index]++;
        else integer += 1.0;
    }
    size_t length = 0;
    if (integer < 1.8446744073709552e19) {
        length = format_u64((uint64_t)integer, out);
    } else {
        /* ponytail: magnitudes beyond 2^64 keep decimal digits. */
        char text[64];
        djs_number_to_string(integer, text, sizeof(text));
        length = djs_strlen(text);
        for (size_t i = 0; i < length; i++) out[i] = text[i];
    }
    if (digits > 0) {
        out[length++] = '.';
        for (int i = 0; i < digits; i++) out[length++] = fraction[i];
    }
    out[length] = '\0';
    return length;
}

static djs_value_t number_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)this_value;
    if (arg_count == 0) return djs_number(0.0);
    if (djs_is_object(args[0])) {
        djs_value_t primitive = djs_to_primitive(rt, args[0], 0);
        return djs_to_number(rt, primitive);
    }
    return djs_to_number(rt, args[0]);
}

static djs_value_t number_to_string_fn(djs_runtime_t *rt, djs_value_t this_value,
                                        djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t receiver = djs_is_object(this_value)
        ? djs_to_primitive(rt, this_value, 0) : this_value;
    double number = djs_to_number(rt, receiver).as.number;
    int radix = 10;
    if (arg_count > 0 && !djs_is_undefined(args[0])) {
        double value = djs_to_number(rt, args[0]).as.number;
        if (value != value) {
            djs_throw_error(rt, "RangeError",
                            "toString() radix must be between 2 and 36");
        }
        radix = (int)value;
        if (radix < 2 || radix > 36) {
            djs_throw_error(rt, "RangeError",
                            "toString() radix must be between 2 and 36");
        }
    }
    if (number != number) return make_cstring(rt, "NaN");
    if (number == 1.0 / 0.0) return make_cstring(rt, "Infinity");
    if (number == -1.0 / 0.0) return make_cstring(rt, "-Infinity");
    if (radix == 10) {
        char text[64];
        djs_number_to_string(number, text, sizeof(text));
        return make_cstring(rt, text);
    }
    bool negative = number < 0.0;
    double magnitude = negative ? -number : number;
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    if (negative) sb_append_char(&builder, '-');
    double integer = num_trunc(magnitude);
    double fraction = magnitude - integer;
    char buffer[80];
    if (integer < 1.8446744073709552e19) {
        size_t length = format_radix_integer((uint64_t)integer, radix, buffer);
        sb_append_bytes(&builder, buffer, length);
    } else {
        /* ponytail: huge magnitudes keep decimal digits. */
        char text[64];
        djs_number_to_string(integer, text, sizeof(text));
        sb_append_cstr(&builder, text);
    }
    if (fraction > 0.0) {
        sb_append_char(&builder, '.');
        char digits[32];
        size_t count = 0;
        while (count + 1 < sizeof(digits) && fraction > 0.0) {
            fraction *= (double)radix;
            int digit = (int)fraction;
            if (digit >= radix) digit = radix - 1;
            if (digit < 0) digit = 0;
            digits[count++] = "0123456789abcdefghijklmnopqrstuvwxyz"[digit];
            fraction -= (double)digit;
        }
        sb_append_bytes(&builder, digits, count);
    }
    return make_cstring(rt, sb_finish(&builder));
}

static djs_value_t number_to_fixed_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    double number = djs_to_number(rt, this_value).as.number;
    int digits = 0;
    if (arg_count > 0 && !djs_is_undefined(args[0])) {
        double value = djs_to_number(rt, args[0]).as.number;
        if (value != value || value < 0.0 || value > 100.0 ||
            value != num_trunc(value)) {
            djs_throw_error(rt, "RangeError",
                            "toFixed() digits argument must be between 0 and 100");
        }
        digits = (int)value;
    }
    if (number != number) return make_cstring(rt, "NaN");
    if (number == 1.0 / 0.0) return make_cstring(rt, "Infinity");
    if (number == -1.0 / 0.0) return make_cstring(rt, "-Infinity");
    bool negative = number < 0.0;
    double magnitude = negative ? -number : number;
    if (magnitude >= 1e21) {
        char text[64];
        djs_number_to_string(number, text, sizeof(text));
        return make_cstring(rt, text);
    }
    char buffer[256];
    format_fixed(magnitude, digits, buffer);
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    if (negative) sb_append_char(&builder, '-');
    sb_append_cstr(&builder, buffer);
    return make_cstring(rt, sb_finish(&builder));
}

static djs_value_t number_to_precision_fn(djs_runtime_t *rt, djs_value_t this_value,
                                          djs_value_t *args, size_t arg_count) {
    double number = djs_to_number(rt, this_value).as.number;
    if (arg_count == 0 || djs_is_undefined(args[0])) {
        char text[64];
        djs_number_to_string(number, text, sizeof(text));
        return make_cstring(rt, text);
    }
    double value = djs_to_number(rt, args[0]).as.number;
    if (value != value || value < 1.0 || value > 100.0 || value != num_trunc(value)) {
        djs_throw_error(rt, "RangeError",
                        "toPrecision() argument must be between 1 and 100");
    }
    int precision = (int)value;
    if (number != number) return make_cstring(rt, "NaN");
    if (number == 1.0 / 0.0) return make_cstring(rt, "Infinity");
    if (number == -1.0 / 0.0) return make_cstring(rt, "-Infinity");
    bool negative = number < 0.0;
    double magnitude = negative ? -number : number;
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    if (negative) sb_append_char(&builder, '-');
    char digits[128];
    char buffer[160];
    if (magnitude == 0.0) {
        for (int i = 0; i < precision; i++) digits[i] = '0';
        format_significant_fixed(digits, precision, 0, buffer);
        sb_append_cstr(&builder, buffer);
        return make_cstring(rt, sb_finish(&builder));
    }
    int exponent = 0;
    extract_significant(magnitude, precision, &exponent, digits);
    if (exponent < -6 || exponent >= precision) {
        format_significant_exponential(digits, precision, exponent, buffer);
    } else {
        format_significant_fixed(digits, precision, exponent, buffer);
    }
    sb_append_cstr(&builder, buffer);
    return make_cstring(rt, sb_finish(&builder));
}

static djs_value_t number_to_exponential_fn(djs_runtime_t *rt, djs_value_t this_value,
                                            djs_value_t *args, size_t arg_count) {
    double number = djs_to_number(rt, this_value).as.number;
    int fraction_digits = -1;
    if (arg_count > 0 && !djs_is_undefined(args[0])) {
        double value = djs_to_number(rt, args[0]).as.number;
        if (value != value || value < 0.0 || value > 100.0 ||
            value != num_trunc(value)) {
            djs_throw_error(rt, "RangeError",
                            "toExponential() argument must be between 0 and 100");
        }
        fraction_digits = (int)value;
    }
    if (number != number) return make_cstring(rt, "NaN");
    if (number == 1.0 / 0.0) return make_cstring(rt, "Infinity");
    if (number == -1.0 / 0.0) return make_cstring(rt, "-Infinity");
    bool negative = number < 0.0;
    double magnitude = negative ? -number : number;
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    if (negative) sb_append_char(&builder, '-');
    char digits[128];
    char buffer[160];
    int exponent = 0;
    size_t count;
    if (fraction_digits < 0) {
        count = shortest_significant(magnitude, digits, &exponent);
    } else {
        count = extract_significant(magnitude, fraction_digits + 1, &exponent, digits);
    }
    format_significant_exponential(digits, (int)count, exponent, buffer);
    sb_append_cstr(&builder, buffer);
    return make_cstring(rt, sb_finish(&builder));
}

static djs_value_t number_parse_int_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t text = djs_to_string(rt, arg_at(args, arg_count, 0));
    const char *characters = text.as.string->bytes;
    size_t length = text.as.string->length;
    int radix = 10;
    if (arg_count > 1 && !djs_is_undefined(args[1])) {
        double value = djs_to_number(rt, args[1]).as.number;
        if (value != value) return djs_number(0.0 / 0.0);
        radix = (int)value;
        if (radix == 0) radix = 10;
        else if (radix < 2 || radix > 36) return djs_number(0.0 / 0.0);
    }
    size_t index = 0;
    while (index < length && builtins_space(characters[index])) index++;
    bool negative = false;
    if (index < length && (characters[index] == '+' || characters[index] == '-')) {
        negative = characters[index] == '-';
        index++;
    }
    if (radix == 10 && index + 1 < length && characters[index] == '0') {
        char marker = characters[index + 1];
        int prefix_radix = 0;
        if (marker == 'x' || marker == 'X') prefix_radix = 16;
        else if (marker == 'o' || marker == 'O') prefix_radix = 8;
        else if (marker == 'b' || marker == 'B') prefix_radix = 2;
        if (prefix_radix != 0) { radix = prefix_radix; index += 2; }
    }
    double value = 0.0;
    size_t digits = 0;
    for (; index < length; index++) {
        int digit = digit_value(characters[index]);
        if (digit < 0 || digit >= radix) break;
        value = value * (double)radix + (double)digit;
        digits++;
    }
    if (digits == 0) return djs_number(0.0 / 0.0);
    return djs_number(negative ? -value : value);
}

static djs_value_t number_parse_float_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t text = djs_to_string(rt, arg_at(args, arg_count, 0));
    const char *characters = text.as.string->bytes;
    size_t length = text.as.string->length;
    size_t index = 0;
    while (index < length && builtins_space(characters[index])) index++;
    size_t sign = index;
    if (index < length && (characters[index] == '+' || characters[index] == '-')) index++;
    if (length - index >= 8 && djs_memcmp_helper(characters + index, "Infinity", 8) == 0) {
        return djs_number(characters[sign] == '-' ? -1.0 / 0.0 : 1.0 / 0.0);
    }
    size_t digits = 0;
    while (index < length && builtins_digit(characters[index])) { index++; digits++; }
    if (index < length && characters[index] == '.') {
        index++;
        while (index < length && builtins_digit(characters[index])) { index++; digits++; }
    }
    if (digits == 0) return djs_number(0.0 / 0.0);
    if (index < length && (characters[index] == 'e' || characters[index] == 'E')) {
        size_t probe = index + 1;
        if (probe < length && (characters[probe] == '+' || characters[probe] == '-')) probe++;
        size_t exponent_digits = 0;
        while (probe < length && builtins_digit(characters[probe])) {
            probe++;
            exponent_digits++;
        }
        if (exponent_digits > 0) index = probe;
    }
    double value = 0.0;
    if (!djs_string_to_number(characters + sign, index - sign, &value)) {
        value = 0.0 / 0.0;
    }
    return djs_number(value);
}

static djs_value_t number_is_integer_fn(djs_runtime_t *rt, djs_value_t this_value,
                                        djs_value_t *args, size_t arg_count) {
    (void)rt; (void)this_value;
    djs_value_t value = arg_at(args, arg_count, 0);
    if (!djs_is_number(value)) return djs_bool(false);
    return djs_bool(djs_number_is_integer(value.as.number));
}

static djs_value_t number_is_finite_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)rt; (void)this_value;
    djs_value_t value = arg_at(args, arg_count, 0);
    if (!djs_is_number(value)) return djs_bool(false);
    double number = value.as.number;
    return djs_bool(number == number && number <= 1.7976931348623157e308 &&
                    number >= -1.7976931348623157e308);
}

static djs_value_t number_is_nan_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)rt; (void)this_value;
    djs_value_t value = arg_at(args, arg_count, 0);
    if (!djs_is_number(value)) return djs_bool(false);
    return djs_bool(value.as.number != value.as.number);
}

static djs_value_t number_is_safe_integer_fn(djs_runtime_t *rt, djs_value_t this_value,
                                             djs_value_t *args, size_t arg_count) {
    (void)rt; (void)this_value;
    djs_value_t value = arg_at(args, arg_count, 0);
    if (!djs_is_number(value)) return djs_bool(false);
    double number = value.as.number;
    return djs_bool(djs_number_is_integer(number) &&
                    num_abs(number) <= 9007199254740991.0);
}

/* ----------------------------------------------------------------- Boolean */

static djs_value_t boolean_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                          djs_value_t *args, size_t arg_count) {
    (void)rt; (void)this_value;
    return djs_bool(djs_to_boolean(arg_at(args, arg_count, 0)));
}

static djs_value_t boolean_to_string_fn(djs_runtime_t *rt, djs_value_t this_value,
                                        djs_value_t *args, size_t arg_count) {
    (void)rt; (void)args; (void)arg_count;
    return make_cstring(rt, djs_to_boolean(this_value) ? "true" : "false");
}

static djs_value_t boolean_value_of_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)rt; (void)args; (void)arg_count;
    return djs_bool(djs_to_boolean(this_value));
}

/* -------------------------------------------------------------------- Math */

static double math_arg(djs_runtime_t *rt, djs_value_t *args, size_t arg_count,
                       size_t index) {
    return djs_to_number(rt, arg_at(args, arg_count, index)).as.number;
}

static bool is_negative_zero(double value) {
    return value == 0.0 && 1.0 / value < 0.0;
}

static djs_value_t math_abs_fn(djs_runtime_t *rt, djs_value_t this_value,
                               djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_abs(math_arg(rt, args, arg_count, 0)));
}

static djs_value_t math_floor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_floor(math_arg(rt, args, arg_count, 0)));
}

static djs_value_t math_ceil_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_ceil(math_arg(rt, args, arg_count, 0)));
}

static djs_value_t math_round_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_round(math_arg(rt, args, arg_count, 0)));
}

static djs_value_t math_trunc_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_trunc(math_arg(rt, args, arg_count, 0)));
}

static djs_value_t math_sign_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)this_value;
    double value = math_arg(rt, args, arg_count, 0);
    if (value != value) return djs_number(value);
    if (value > 0.0) return djs_number(1.0);
    if (value < 0.0) return djs_number(-1.0);
    return djs_number(value);
}

static djs_value_t math_sqrt_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_sqrt(math_arg(rt, args, arg_count, 0)));
}

static djs_value_t math_cbrt_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_cbrt(math_arg(rt, args, arg_count, 0)));
}

static djs_value_t math_exp_fn(djs_runtime_t *rt, djs_value_t this_value,
                               djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_exp(math_arg(rt, args, arg_count, 0)));
}

static djs_value_t math_log_fn(djs_runtime_t *rt, djs_value_t this_value,
                               djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_ln(math_arg(rt, args, arg_count, 0)));
}

static djs_value_t math_log2_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_ln(math_arg(rt, args, arg_count, 0)) / DJS_LN2);
}

static djs_value_t math_log10_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_ln(math_arg(rt, args, arg_count, 0)) / DJS_LN10);
}

static djs_value_t math_pow_fn(djs_runtime_t *rt, djs_value_t this_value,
                               djs_value_t *args, size_t arg_count) {
    (void)this_value;
    double base = math_arg(rt, args, arg_count, 0);
    double exponent = math_arg(rt, args, arg_count, 1);
    return djs_number(num_pow(base, exponent));
}

static djs_value_t math_min_fn(djs_runtime_t *rt, djs_value_t this_value,
                               djs_value_t *args, size_t arg_count) {
    (void)this_value;
    double result = 1.0 / 0.0;
    for (size_t i = 0; i < arg_count; i++) {
        double value = math_arg(rt, args, arg_count, i);
        if (value != value) return djs_number(value);
        if (value < result) result = value;
        else if (value == result && value == 0.0 && is_negative_zero(value)) result = value;
    }
    return djs_number(result);
}

static djs_value_t math_max_fn(djs_runtime_t *rt, djs_value_t this_value,
                               djs_value_t *args, size_t arg_count) {
    (void)this_value;
    double result = -1.0 / 0.0;
    for (size_t i = 0; i < arg_count; i++) {
        double value = math_arg(rt, args, arg_count, i);
        if (value != value) return djs_number(value);
        if (value > result) result = value;
        else if (value == result && value == 0.0 && !is_negative_zero(value)) result = value;
    }
    return djs_number(result);
}

static djs_value_t math_random_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    rt->random_state = rt->random_state * 6364136223846793005ULL + 1442695040888963407ULL;
    uint64_t value = rt->random_state >> 11;
    return djs_number((double)value / 9007199254740992.0);
}

static djs_value_t math_sin_fn(djs_runtime_t *rt, djs_value_t this_value,
                               djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_sin(math_arg(rt, args, arg_count, 0)));
}

static djs_value_t math_cos_fn(djs_runtime_t *rt, djs_value_t this_value,
                               djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_cos(math_arg(rt, args, arg_count, 0)));
}

static djs_value_t math_tan_fn(djs_runtime_t *rt, djs_value_t this_value,
                               djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_tan(math_arg(rt, args, arg_count, 0)));
}

static djs_value_t math_asin_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)this_value;
    double value = math_arg(rt, args, arg_count, 0);
    if (value != value || value < -1.0 || value > 1.0) return djs_number(0.0 / 0.0);
    return djs_number(num_atan2(value, num_sqrt(1.0 - value * value)));
}

static djs_value_t math_acos_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)this_value;
    double value = math_arg(rt, args, arg_count, 0);
    if (value != value || value < -1.0 || value > 1.0) return djs_number(0.0 / 0.0);
    return djs_number(num_atan2(num_sqrt(1.0 - value * value), value));
}

static djs_value_t math_atan_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_atan(math_arg(rt, args, arg_count, 0)));
}

static djs_value_t math_atan2_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return djs_number(num_atan2(math_arg(rt, args, arg_count, 0),
                                math_arg(rt, args, arg_count, 1)));
}

static djs_value_t math_hypot_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    (void)this_value;
    double result = 0.0;
    for (size_t i = 0; i < arg_count; i++) {
        double value = math_arg(rt, args, arg_count, i);
        if (value != value) return djs_number(value);
        result = num_hypot(result, value);
    }
    return djs_number(result);
}

static djs_value_t math_sinh_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)this_value;
    double value = math_arg(rt, args, arg_count, 0);
    return djs_number((num_exp(value) - num_exp(-value)) / 2.0);
}

static djs_value_t math_cosh_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)this_value;
    double value = math_arg(rt, args, arg_count, 0);
    return djs_number((num_exp(value) + num_exp(-value)) / 2.0);
}

static djs_value_t math_tanh_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)this_value;
    double value = math_arg(rt, args, arg_count, 0);
    double exponential = num_exp(2.0 * value);
    if (exponential == 1.0 / 0.0) return djs_number(1.0);
    return djs_number((exponential - 1.0) / (exponential + 1.0));
}

static djs_value_t math_fround_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)this_value;
    double value = math_arg(rt, args, arg_count, 0);
    return djs_number((double)(float)value);
}

static djs_value_t math_imul_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)this_value;
    long long left = (long long)num_trunc(math_arg(rt, args, arg_count, 0));
    long long right = (long long)num_trunc(math_arg(rt, args, arg_count, 1));
    uint32_t a = (uint32_t)(uint64_t)left;
    uint32_t b = (uint32_t)(uint64_t)right;
    uint32_t product = a * b;
    return djs_number((double)(int32_t)product);
}

/* -------------------------------------------------------------------- JSON */

typedef struct {
    djs_runtime_t *rt;
    const char *text;
    size_t length;
    size_t position;
} json_reader_t;

static void json_fail(json_reader_t *reader, const char *message) {
    djs_throw_error(reader->rt, "SyntaxError", message);
}

static void json_skip_space(json_reader_t *reader) {
    while (reader->position < reader->length) {
        char c = reader->text[reader->position];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') reader->position++;
        else break;
    }
}

static bool json_read_hex(json_reader_t *reader, size_t count, uint32_t *out) {
    uint32_t value = 0;
    if (reader->position + count > reader->length) return false;
    for (size_t i = 0; i < count; i++) {
        int digit = digit_value(reader->text[reader->position + i]);
        if (digit < 0 || digit > 15) return false;
        value = value * 16 + (uint32_t)digit;
    }
    reader->position += count;
    *out = value;
    return true;
}

static djs_value_t json_parse_value(json_reader_t *reader);

static djs_value_t json_parse_string(json_reader_t *reader) {
    djs_string_builder_t builder;
    sb_init(&builder, reader->rt);
    while (reader->position < reader->length) {
        char c = reader->text[reader->position++];
        if (c == '"') return make_cstring(reader->rt, sb_finish(&builder));
        if (c != '\\') {
            sb_append_char(&builder, c);
            continue;
        }
        if (reader->position >= reader->length) break;
        char escape = reader->text[reader->position++];
        switch (escape) {
        case '"': sb_append_char(&builder, '"'); break;
        case '\\': sb_append_char(&builder, '\\'); break;
        case '/': sb_append_char(&builder, '/'); break;
        case 'b': sb_append_char(&builder, '\b'); break;
        case 'f': sb_append_char(&builder, '\f'); break;
        case 'n': sb_append_char(&builder, '\n'); break;
        case 'r': sb_append_char(&builder, '\r'); break;
        case 't': sb_append_char(&builder, '\t'); break;
        case 'u': {
            uint32_t code = 0;
            if (!json_read_hex(reader, 4, &code)) {
                json_fail(reader, "Bad Unicode escape in JSON");
            }
            if (code >= 0xD800u && code <= 0xDBFFu &&
                reader->position + 1 < reader->length &&
                reader->text[reader->position] == '\\' &&
                reader->text[reader->position + 1] == 'u') {
                size_t saved = reader->position;
                reader->position += 2;
                uint32_t low = 0;
                if (json_read_hex(reader, 4, &low) && low >= 0xDC00u && low <= 0xDFFFu) {
                    code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                } else {
                    reader->position = saved;
                }
            }
            char encoded[4];
            size_t encoded_length = utf8_encode(code, encoded);
            sb_append_bytes(&builder, encoded, encoded_length);
            break;
        }
        default:
            json_fail(reader, "Bad escape sequence in JSON");
        }
    }
    json_fail(reader, "Unterminated string in JSON");
    return DJS_UNDEFINED;
}

static djs_value_t json_parse_number(json_reader_t *reader) {
    size_t start = reader->position;
    if (reader->position < reader->length && reader->text[reader->position] == '-') {
        reader->position++;
    }
    size_t digits = 0;
    while (reader->position < reader->length &&
           builtins_digit(reader->text[reader->position])) {
        reader->position++;
        digits++;
    }
    if (digits == 0) json_fail(reader, "Unexpected token in JSON");
    if (reader->position < reader->length && reader->text[reader->position] == '.') {
        reader->position++;
        size_t fraction_digits = 0;
        while (reader->position < reader->length &&
               builtins_digit(reader->text[reader->position])) {
            reader->position++;
            fraction_digits++;
        }
        if (fraction_digits == 0) json_fail(reader, "Unexpected token in JSON");
    }
    if (reader->position < reader->length &&
        (reader->text[reader->position] == 'e' || reader->text[reader->position] == 'E')) {
        reader->position++;
        if (reader->position < reader->length &&
            (reader->text[reader->position] == '+' || reader->text[reader->position] == '-')) {
            reader->position++;
        }
        size_t exponent_digits = 0;
        while (reader->position < reader->length &&
               builtins_digit(reader->text[reader->position])) {
            reader->position++;
            exponent_digits++;
        }
        if (exponent_digits == 0) json_fail(reader, "Unexpected token in JSON");
    }
    double value = 0.0;
    if (!djs_string_to_number(reader->text + start, reader->position - start, &value)) {
        json_fail(reader, "Unexpected token in JSON");
    }
    return djs_number(value);
}

static bool json_match_word(json_reader_t *reader, const char *word) {
    size_t length = djs_strlen(word);
    if (reader->position + length > reader->length) return false;
    if (djs_memcmp_helper(reader->text + reader->position, word, length) != 0) return false;
    reader->position += length;
    return true;
}

static djs_value_t json_parse_array(json_reader_t *reader) {
    djs_value_t result = djs_make_array(reader->rt, NULL, 0);
    reader->position++; /* '[' */
    json_skip_space(reader);
    if (reader->position < reader->length && reader->text[reader->position] == ']') {
        reader->position++;
        return result;
    }
    for (;;) {
        djs_value_t element = json_parse_value(reader);
        djs_array_push(reader->rt, result.as.object, element);
        json_skip_space(reader);
        if (reader->position >= reader->length) {
            json_fail(reader, "Unexpected end of JSON input");
        }
        char c = reader->text[reader->position++];
        if (c == ']') return result;
        if (c != ',') json_fail(reader, "Unexpected token in JSON");
        json_skip_space(reader);
    }
}

static djs_value_t json_parse_object(json_reader_t *reader) {
    djs_value_t result = djs_make_object(reader->rt);
    reader->position++; /* '{' */
    json_skip_space(reader);
    if (reader->position < reader->length && reader->text[reader->position] == '}') {
        reader->position++;
        return result;
    }
    for (;;) {
        json_skip_space(reader);
        if (reader->position >= reader->length ||
            reader->text[reader->position] != '"') {
            json_fail(reader, "Expected property name in JSON");
        }
        reader->position++;
        djs_value_t key = json_parse_string(reader);
        json_skip_space(reader);
        if (reader->position >= reader->length || reader->text[reader->position] != ':') {
            json_fail(reader, "Expected ':' in JSON");
        }
        reader->position++;
        djs_value_t value = json_parse_value(reader);
        djs_set_prop(reader->rt, result, key, value);
        json_skip_space(reader);
        if (reader->position >= reader->length) {
            json_fail(reader, "Unexpected end of JSON input");
        }
        char c = reader->text[reader->position++];
        if (c == '}') return result;
        if (c != ',') json_fail(reader, "Unexpected token in JSON");
    }
}

static djs_value_t json_parse_value(json_reader_t *reader) {
    json_skip_space(reader);
    if (reader->position >= reader->length) {
        json_fail(reader, "Unexpected end of JSON input");
    }
    char c = reader->text[reader->position];
    switch (c) {
    case '{': return json_parse_object(reader);
    case '[': return json_parse_array(reader);
    case '"':
        reader->position++;
        return json_parse_string(reader);
    case 't':
        if (json_match_word(reader, "true")) return djs_bool(true);
        json_fail(reader, "Unexpected token in JSON");
        break;
    case 'f':
        if (json_match_word(reader, "false")) return djs_bool(false);
        json_fail(reader, "Unexpected token in JSON");
        break;
    case 'n':
        if (json_match_word(reader, "null")) return DJS_NULL;
        json_fail(reader, "Unexpected token in JSON");
        break;
    default:
        break;
    }
    return json_parse_number(reader);
}

static djs_value_t json_parse_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t text = djs_to_string(rt, arg_at(args, arg_count, 0));
    json_reader_t reader;
    reader.rt = rt;
    reader.text = text.as.string->bytes;
    reader.length = text.as.string->length;
    reader.position = 0;
    djs_value_t value = json_parse_value(&reader);
    json_skip_space(&reader);
    if (reader.position != reader.length) {
        json_fail(&reader, "Unexpected token in JSON");
    }
    return value;
}

static void json_write_string(djs_runtime_t *rt, djs_string_builder_t *builder,
                              const char *text, size_t length) {
    (void)rt;
    static const char hex[] = "0123456789abcdef";
    sb_append_char(builder, '"');
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        switch (c) {
        case '"': sb_append_cstr(builder, "\\\""); break;
        case '\\': sb_append_cstr(builder, "\\\\"); break;
        case '\b': sb_append_cstr(builder, "\\b"); break;
        case '\f': sb_append_cstr(builder, "\\f"); break;
        case '\n': sb_append_cstr(builder, "\\n"); break;
        case '\r': sb_append_cstr(builder, "\\r"); break;
        case '\t': sb_append_cstr(builder, "\\t"); break;
        default:
            if (c < 0x20u) {
                char escape[8];
                escape[0] = '\\';
                escape[1] = 'u';
                escape[2] = '0';
                escape[3] = '0';
                escape[4] = hex[(c >> 4) & 0x0Fu];
                escape[5] = hex[c & 0x0Fu];
                escape[6] = '\0';
                sb_append_cstr(builder, escape);
            } else {
                sb_append_char(builder, (char)c);
            }
        }
    }
    sb_append_char(builder, '"');
}

static void json_write_indent(djs_string_builder_t *builder, const char *gap,
                              size_t indent) {
    if (gap[0] == '\0') return;
    sb_append_char(builder, '\n');
    for (size_t i = 0; i < indent; i++) sb_append_cstr(builder, gap);
}

static bool json_write_value(djs_runtime_t *rt, djs_string_builder_t *builder,
                             djs_value_t value, djs_value_t replacer,
                             const char *gap, size_t indent, int depth) {
    if (depth > 256) {
        djs_throw_type_error(rt, "Converting circular structure to JSON");
    }
    if (djs_is_object(value)) {
        if (value.as.object->class == DJS_CLASS_FUNCTION) return false;
        djs_value_t to_json = djs_object_get(rt, value.as.object, "toJSON");
        if (djs_value_is_callable(to_json)) {
            djs_value_t arguments[1] = { value };
            value = djs_call(rt, to_json, value, arguments, 1);
        }
    }
    switch (value.tag) {
    case DJS_TAG_UNDEFINED:
        return false;
    case DJS_TAG_NULL:
        sb_append_cstr(builder, "null");
        return true;
    case DJS_TAG_BOOL:
        sb_append_cstr(builder, value.as.boolean ? "true" : "false");
        return true;
    case DJS_TAG_NUMBER: {
        double number = value.as.number;
        if (number != number || number == 1.0 / 0.0 || number == -1.0 / 0.0) {
            sb_append_cstr(builder, "null");
            return true;
        }
        char text[64];
        djs_number_to_string(number, text, sizeof(text));
        sb_append_cstr(builder, text);
        return true;
    }
    case DJS_TAG_STRING:
        json_write_string(rt, builder, value.as.string->bytes, value.as.string->length);
        return true;
    case DJS_TAG_OBJECT:
        break;
    }
    if (value.as.object->class == DJS_CLASS_ARRAY) {
        size_t length = djs_array_length(value.as.object);
        sb_append_char(builder, '[');
        for (size_t i = 0; i < length; i++) {
            if (i > 0) sb_append_char(builder, ',');
            json_write_indent(builder, gap, indent + djs_strlen(gap));
            djs_value_t element = djs_array_get(rt, value.as.object, i);
            if (djs_value_is_callable(replacer)) {
                char key[32];
                format_u64(i, key);
                djs_value_t arguments[2] = {
                    make_cstring(rt, key), element
                };
                element = djs_call(rt, replacer, value, arguments, 2);
            }
            djs_string_builder_t scratch;
            sb_init(&scratch, rt);
            if (!json_write_value(rt, &scratch, element, replacer, gap,
                                  indent + djs_strlen(gap), depth + 1)) {
                sb_append_cstr(builder, "null");
                continue;
            }
            sb_append_cstr(builder, sb_finish(&scratch));
        }
        if (length > 0) json_write_indent(builder, gap, indent);
        sb_append_char(builder, ']');
        return true;
    }
    char **keys = NULL;
    size_t count = djs_object_keys(rt, value.as.object, &keys);
    sb_append_char(builder, '{');
    size_t written = 0;
    for (size_t i = 0; i < count; i++) {
        djs_value_t element = djs_object_get(rt, value.as.object, keys[i]);
        if (djs_value_is_callable(replacer)) {
            djs_value_t arguments[2] = { make_cstring(rt, keys[i]), element };
            element = djs_call(rt, replacer, value, arguments, 2);
        }
        djs_string_builder_t scratch;
        sb_init(&scratch, rt);
        if (!json_write_value(rt, &scratch, element, replacer, gap,
                              indent + djs_strlen(gap), depth + 1)) {
            continue;
        }
        if (written > 0) sb_append_char(builder, ',');
        json_write_indent(builder, gap, indent + djs_strlen(gap));
        json_write_string(rt, builder, keys[i], djs_strlen(keys[i]));
        sb_append_char(builder, ':');
        if (gap[0] != '\0') sb_append_char(builder, ' ');
        sb_append_cstr(builder, sb_finish(&scratch));
        written++;
    }
    if (written > 0) json_write_indent(builder, gap, indent);
    sb_append_char(builder, '}');
    return true;
}

static djs_value_t json_stringify_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t value = arg_at(args, arg_count, 0);
    djs_value_t replacer = arg_at(args, arg_count, 1);
    if (!djs_value_is_callable(replacer)) replacer = DJS_UNDEFINED;
    djs_value_t space = arg_at(args, arg_count, 2);
    char gap[16];
    size_t gap_length = 0;
    if (djs_is_number(space)) {
        double count = space.as.number;
        if (count > 10.0) count = 10.0;
        if (count < 0.0 || count != count) count = 0.0;
        gap_length = (size_t)num_trunc(count);
    } else if (djs_is_string(space)) {
        gap_length = space.as.string->length;
        if (gap_length > 10) gap_length = 10;
        for (size_t i = 0; i < gap_length; i++) gap[i] = space.as.string->bytes[i];
    }
    gap[gap_length] = '\0';
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    if (!json_write_value(rt, &builder, value, replacer, gap, 0, 0)) return DJS_UNDEFINED;
    return make_cstring(rt, sb_finish(&builder));
}

/* ------------------------------------------------------------------ Errors */

static djs_value_t make_error_value(djs_runtime_t *rt, const char *kind,
                                    djs_value_t *args, size_t arg_count) {
    char text[512];
    djs_value_t message = arg_at(args, arg_count, 0);
    if (djs_is_undefined(message)) {
        text[0] = '\0';
    } else {
        djs_value_t converted = djs_to_string(rt, message);
        size_t length = converted.as.string->length;
        if (length >= sizeof(text)) length = sizeof(text) - 1;
        djs_memcpy(text, converted.as.string->bytes, length);
        text[length] = '\0';
    }
    return djs_make_error(rt, kind, text);
}

static djs_value_t error_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                        djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return make_error_value(rt, "Error", args, arg_count);
}

static djs_value_t type_error_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                             djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return make_error_value(rt, "TypeError", args, arg_count);
}

static djs_value_t range_error_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                              djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return make_error_value(rt, "RangeError", args, arg_count);
}

static djs_value_t syntax_error_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                               djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return make_error_value(rt, "SyntaxError", args, arg_count);
}

static djs_value_t reference_error_constructor_fn(djs_runtime_t *rt,
                                                  djs_value_t this_value,
                                                  djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return make_error_value(rt, "ReferenceError", args, arg_count);
}

static djs_value_t eval_error_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                             djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return make_error_value(rt, "EvalError", args, arg_count);
}

static djs_value_t uri_error_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                            djs_value_t *args, size_t arg_count) {
    (void)this_value;
    return make_error_value(rt, "URIError", args, arg_count);
}

static djs_value_t error_to_string_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    if (!djs_is_object(this_value)) return make_cstring(rt, "Error");
    djs_value_t name = djs_object_get(rt, this_value.as.object, "name");
    djs_value_t message = djs_object_get(rt, this_value.as.object, "message");
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    if (djs_is_string(name)) sb_append_cstr(&builder, name.as.string->bytes);
    else sb_append_cstr(&builder, "Error");
    if (djs_is_string(message) && message.as.string->length > 0) {
        sb_append_cstr(&builder, ": ");
        sb_append_cstr(&builder, message.as.string->bytes);
    }
    return make_cstring(rt, sb_finish(&builder));
}

/* -------------------------------------------------------------- Map and Set */

static djs_map_entry_t *map_find_entry(djs_object_t *object, djs_value_t key) {
    for (djs_map_entry_t *entry = object->map_entries; entry != NULL;
         entry = entry->next) {
        if (entry->deleted) continue;
        if (same_value_zero(entry->key, key)) return entry;
    }
    return NULL;
}

static void map_set_entry(djs_runtime_t *rt, djs_object_t *object, djs_value_t key,
                          djs_value_t value) {
    djs_map_entry_t *entry = map_find_entry(object, key);
    if (entry != NULL) {
        entry->value = value;
        return;
    }
    djs_map_entry_t *fresh = (djs_map_entry_t *)djs_arena_calloc(rt, 1, sizeof(*fresh));
    if (fresh == NULL) return;
    fresh->key = key;
    fresh->value = value;
    djs_map_entry_t **cursor = &object->map_entries;
    while (*cursor != NULL) cursor = &(*cursor)->next;
    *cursor = fresh;
    object->map_count++;
}

static djs_object_t *map_this(djs_runtime_t *rt, djs_value_t this_value) {
    if (!djs_is_object(this_value) ||
        (this_value.as.object->class != DJS_CLASS_MAP &&
         this_value.as.object->class != DJS_CLASS_SET)) {
        djs_throw_type_error(rt, "method called on incompatible receiver");
    }
    return this_value.as.object;
}

static djs_value_t map_get_fn(djs_runtime_t *rt, djs_value_t this_value,
                              djs_value_t *args, size_t arg_count) {
    djs_object_t *map = map_this(rt, this_value);
    djs_map_entry_t *entry = map_find_entry(map, arg_at(args, arg_count, 0));
    return entry == NULL ? DJS_UNDEFINED : entry->value;
}

static djs_value_t map_set_fn(djs_runtime_t *rt, djs_value_t this_value,
                              djs_value_t *args, size_t arg_count) {
    djs_object_t *map = map_this(rt, this_value);
    map_set_entry(rt, map, arg_at(args, arg_count, 0), arg_at(args, arg_count, 1));
    return this_value;
}

static djs_value_t map_has_fn(djs_runtime_t *rt, djs_value_t this_value,
                              djs_value_t *args, size_t arg_count) {
    djs_object_t *map = map_this(rt, this_value);
    return djs_bool(map_find_entry(map, arg_at(args, arg_count, 0)) != NULL);
}

static djs_value_t map_delete_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    djs_object_t *map = map_this(rt, this_value);
    djs_map_entry_t *entry = map_find_entry(map, arg_at(args, arg_count, 0));
    if (entry == NULL) return djs_bool(false);
    entry->deleted = true;
    entry->value = DJS_UNDEFINED;
    if (map->map_count > 0) map->map_count--;
    return djs_bool(true);
}

static djs_value_t map_clear_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *map = map_this(rt, this_value);
    for (djs_map_entry_t *entry = map->map_entries; entry != NULL; entry = entry->next) {
        entry->deleted = true;
        entry->value = DJS_UNDEFINED;
    }
    map->map_count = 0;
    return DJS_UNDEFINED;
}

static djs_value_t map_size_fn(djs_runtime_t *rt, djs_value_t this_value,
                               djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *map = map_this(rt, this_value);
    return djs_number((double)map->map_count);
}

/* donejs exposes `size` on the collection value itself and has no size
 * accessor on the prototype (internal/jit/collections.go), so bind the
 * accessor to every collection that gets created. */
static void define_collection_size(djs_runtime_t *rt, djs_object_t *collection) {
    djs_define_accessor(rt, collection, "size", map_size_fn, NULL);
}

static djs_value_t map_for_each_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    djs_object_t *map = map_this(rt, this_value);
    djs_value_t callback = arg_at(args, arg_count, 0);
    djs_value_t this_arg = arg_at(args, arg_count, 1);
    if (!djs_value_is_callable(callback)) {
        djs_throw_type_error(rt, "callback is not a function");
    }
    for (djs_map_entry_t *entry = map->map_entries; entry != NULL; entry = entry->next) {
        if (entry->deleted) continue;
        djs_value_t callback_args[3] = { entry->value, entry->key, this_value };
        djs_call(rt, callback, this_arg, callback_args, 3);
    }
    return DJS_UNDEFINED;
}

static djs_value_t map_iterator_next(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    djs_value_t items = closure_get(rt, "items");
    djs_value_t index_value = closure_get(rt, "index");
    if (items.tag != DJS_TAG_OBJECT || index_value.tag != DJS_TAG_NUMBER) {
        return iterator_result(rt, DJS_UNDEFINED, true);
    }
    size_t index = (size_t)index_value.as.number;
    if (index >= djs_array_length(items.as.object)) {
        return iterator_result(rt, DJS_UNDEFINED, true);
    }
    closure_set(rt, rt->frame->function->closure, "index",
                djs_number((double)(index + 1)));
    djs_value_t pair = djs_array_get(rt, items.as.object, index);
    if (pair.tag != DJS_TAG_OBJECT) return iterator_result(rt, DJS_UNDEFINED, true);
    djs_value_t mode = closure_get(rt, "mode");
    if (mode.tag == DJS_TAG_STRING) {
        if (djs_strcmp(mode.as.string->bytes, "keys") == 0) {
            return iterator_result(rt, indexed_get(rt, pair.as.object, 0), false);
        }
        if (djs_strcmp(mode.as.string->bytes, "values") == 0) {
            return iterator_result(rt, indexed_get(rt, pair.as.object, 1), false);
        }
    }
    return iterator_result(rt, pair, false);
}

/* ponytail: iterators walk a snapshot, so entries deleted mid-iteration stay
 * visible until the iterator is exhausted. */
static djs_value_t make_map_iterator(djs_runtime_t *rt, djs_object_t *map,
                                     const char *mode) {
    djs_value_t items = djs_make_array(rt, NULL, 0);
    for (djs_map_entry_t *entry = map->map_entries; entry != NULL; entry = entry->next) {
        if (entry->deleted) continue;
        djs_value_t pair[2] = { entry->key, entry->value };
        djs_array_push(rt, items.as.object, djs_make_array(rt, pair, 2));
    }
    djs_env_t *closure = make_closure(rt);
    closure_set(rt, closure, "items", items);
    closure_set(rt, closure, "index", djs_number(0.0));
    closure_set(rt, closure, "mode", make_cstring(rt, mode));
    return make_iterator(rt, map_iterator_next, "next", closure);
}

static djs_value_t map_keys_fn(djs_runtime_t *rt, djs_value_t this_value,
                               djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *map = map_this(rt, this_value);
    return make_map_iterator(rt, map, "keys");
}

static djs_value_t map_values_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *map = map_this(rt, this_value);
    return make_map_iterator(rt, map, "values");
}

static djs_value_t map_entries_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *map = map_this(rt, this_value);
    return make_map_iterator(rt, map, "entries");
}

static djs_value_t map_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_object_t *map = djs_object_new(rt, rt->map_prototype);
    if (map == NULL) return DJS_UNDEFINED;
    map->class = DJS_CLASS_MAP;
    define_collection_size(rt, map);
    djs_value_t source = arg_at(args, arg_count, 0);
    if (!djs_is_nullish(source)) {
        djs_value_t items = iterable_to_array(rt, source);
        for (size_t i = 0; i < djs_array_length(items.as.object); i++) {
            djs_value_t entry = djs_array_get(rt, items.as.object, i);
            djs_value_t key = DJS_UNDEFINED;
            djs_value_t value = DJS_UNDEFINED;
            if (djs_is_object(entry)) {
                key = indexed_get(rt, entry.as.object, 0);
                value = indexed_get(rt, entry.as.object, 1);
            }
            map_set_entry(rt, map, key, value);
        }
    }
    return djs_object_value(map);
}

static djs_value_t set_add_fn(djs_runtime_t *rt, djs_value_t this_value,
                              djs_value_t *args, size_t arg_count) {
    djs_object_t *set = map_this(rt, this_value);
    djs_value_t value = arg_at(args, arg_count, 0);
    map_set_entry(rt, set, value, value);
    return this_value;
}

static djs_value_t set_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_object_t *set = djs_object_new(rt, rt->set_prototype);
    if (set == NULL) return DJS_UNDEFINED;
    set->class = DJS_CLASS_SET;
    define_collection_size(rt, set);
    djs_value_t source = arg_at(args, arg_count, 0);
    if (!djs_is_nullish(source)) {
        djs_value_t items = iterable_to_array(rt, source);
        for (size_t i = 0; i < djs_array_length(items.as.object); i++) {
            djs_value_t value = djs_array_get(rt, items.as.object, i);
            map_set_entry(rt, set, value, value);
        }
    }
    return djs_object_value(set);
}

/* ---------------------------------------------------------------- Promises */

static djs_value_t promise_resolve_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t promise = closure_get(rt, "promise");
    if (promise.tag != DJS_TAG_OBJECT) return DJS_UNDEFINED;
    djs_promise_resolve(rt, promise, arg_at(args, arg_count, 0));
    return DJS_UNDEFINED;
}

static djs_value_t promise_reject_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t promise = closure_get(rt, "promise");
    if (promise.tag != DJS_TAG_OBJECT) return DJS_UNDEFINED;
    djs_promise_reject(rt, promise, arg_at(args, arg_count, 0));
    return DJS_UNDEFINED;
}

static djs_value_t promise_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                          djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t executor = arg_at(args, arg_count, 0);
    if (!djs_value_is_callable(executor)) {
        djs_throw_type_error(rt, "Promise resolver is not a function");
    }
    djs_value_t promise = djs_promise_new(rt);
    djs_env_t *closure = make_closure(rt);
    closure_set(rt, closure, "promise", promise);
    djs_object_t *resolve = function_with_closure(rt, promise_resolve_fn, "resolve", 1,
                                                   closure);
    djs_object_t *reject = function_with_closure(rt, promise_reject_fn, "reject", 1,
                                                  closure);
    if (resolve == NULL || reject == NULL) return promise;
    djs_value_t executor_args[2] = { djs_object_value(resolve), djs_object_value(reject) };
    djs_call(rt, executor, DJS_UNDEFINED, executor_args, 2);
    return promise;
}

static djs_value_t promise_resolve_static_fn(djs_runtime_t *rt, djs_value_t this_value,
                                             djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t value = arg_at(args, arg_count, 0);
    if (djs_is_object(value) && value.as.object->class == DJS_CLASS_PROMISE) {
        return value;
    }
    djs_value_t promise = djs_promise_new(rt);
    djs_promise_resolve(rt, promise, value);
    return promise;
}

static djs_value_t promise_reject_static_fn(djs_runtime_t *rt, djs_value_t this_value,
                                            djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t promise = djs_promise_new(rt);
    djs_promise_reject(rt, promise, arg_at(args, arg_count, 0));
    return promise;
}

static djs_value_t promise_then_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    if (!djs_is_object(this_value)) {
        djs_throw_type_error(rt, "Promise.prototype.then called on incompatible receiver");
    }
    djs_value_t on_ok = arg_at(args, arg_count, 0);
    djs_value_t on_err = arg_at(args, arg_count, 1);
    if (!djs_value_is_callable(on_ok)) on_ok = DJS_UNDEFINED;
    if (!djs_value_is_callable(on_err)) on_err = DJS_UNDEFINED;
    return djs_promise_then(rt, this_value, on_ok, on_err);
}

static djs_value_t promise_catch_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    if (!djs_is_object(this_value)) {
        djs_throw_type_error(rt, "Promise.prototype.catch called on incompatible receiver");
    }
    djs_value_t on_err = arg_at(args, arg_count, 0);
    if (!djs_value_is_callable(on_err)) on_err = DJS_UNDEFINED;
    return djs_promise_then(rt, this_value, DJS_UNDEFINED, on_err);
}

static djs_value_t promise_finally_on_ok(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t callback = closure_get(rt, "callback");
    if (djs_value_is_callable(callback)) djs_call(rt, callback, DJS_UNDEFINED, NULL, 0);
    return arg_at(args, arg_count, 0);
}

static djs_value_t promise_finally_on_err(djs_runtime_t *rt, djs_value_t this_value,
                                          djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t callback = closure_get(rt, "callback");
    if (djs_value_is_callable(callback)) djs_call(rt, callback, DJS_UNDEFINED, NULL, 0);
    djs_throw(rt, arg_at(args, arg_count, 0));
    return DJS_UNDEFINED;
}

static djs_value_t promise_finally_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    if (!djs_is_object(this_value)) {
        djs_throw_type_error(rt, "Promise.prototype.finally called on incompatible receiver");
    }
    djs_value_t callback = arg_at(args, arg_count, 0);
    if (!djs_value_is_callable(callback)) {
        return djs_promise_then(rt, this_value, DJS_UNDEFINED, DJS_UNDEFINED);
    }
    djs_env_t *closure = make_closure(rt);
    closure_set(rt, closure, "callback", callback);
    djs_object_t *on_ok = function_with_closure(rt, promise_finally_on_ok, "", 0, closure);
    djs_object_t *on_err = function_with_closure(rt, promise_finally_on_err, "", 0, closure);
    if (on_ok == NULL || on_err == NULL) {
        return djs_promise_then(rt, this_value, DJS_UNDEFINED, DJS_UNDEFINED);
    }
    return djs_promise_then(rt, this_value, djs_object_value(on_ok),
                             djs_object_value(on_err));
}

static double promise_state_number(djs_runtime_t *rt, djs_value_t state,
                                   const char *name) {
    if (state.tag != DJS_TAG_OBJECT) return 0.0;
    djs_value_t value = djs_object_get(rt, state.as.object, name);
    return value.tag == DJS_TAG_NUMBER ? value.as.number : 0.0;
}

static djs_value_t promise_state_value(djs_runtime_t *rt, djs_value_t state,
                                       const char *name) {
    if (state.tag != DJS_TAG_OBJECT) return DJS_UNDEFINED;
    return djs_object_get(rt, state.as.object, name);
}

static djs_value_t promise_all_on_ok(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t state = closure_get(rt, "state");
    djs_value_t index = closure_get(rt, "index");
    djs_value_t results = promise_state_value(rt, state, "results");
    if (results.tag == DJS_TAG_OBJECT && index.tag == DJS_TAG_NUMBER) {
        djs_array_set(rt, results.as.object, (size_t)index.as.number,
                      arg_at(args, arg_count, 0));
    }
    double remaining = promise_state_number(rt, state, "remaining") - 1.0;
    if (state.tag == DJS_TAG_OBJECT) {
        djs_object_set(rt, state.as.object, "remaining", djs_number(remaining));
    }
    if (remaining <= 0.0) {
        djs_promise_resolve(rt, promise_state_value(rt, state, "promise"), results);
    }
    return DJS_UNDEFINED;
}

static djs_value_t promise_all_on_err(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t state = closure_get(rt, "state");
    djs_promise_reject(rt, promise_state_value(rt, state, "promise"),
                       arg_at(args, arg_count, 0));
    return DJS_UNDEFINED;
}

static djs_value_t promise_race_on_ok(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t state = closure_get(rt, "state");
    djs_promise_resolve(rt, promise_state_value(rt, state, "promise"),
                        arg_at(args, arg_count, 0));
    return DJS_UNDEFINED;
}

static djs_value_t promise_race_on_err(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t state = closure_get(rt, "state");
    djs_promise_reject(rt, promise_state_value(rt, state, "promise"),
                       arg_at(args, arg_count, 0));
    return DJS_UNDEFINED;
}

/* Prepare the shared state of Promise.all / allSettled / any / race. */
static djs_value_t promise_combine_setup(djs_runtime_t *rt, djs_value_t *args,
                                         size_t arg_count) {
    djs_value_t items = iterable_to_array(rt, arg_at(args, arg_count, 0));
    size_t count = djs_array_length(items.as.object);
    djs_value_t promise = djs_promise_new(rt);
    djs_value_t results = djs_make_array(rt, NULL, 0);
    djs_value_t state = djs_make_object(rt);
    djs_object_set(rt, state.as.object, "remaining", djs_number((double)count));
    djs_object_set(rt, state.as.object, "count", djs_number((double)count));
    djs_object_set(rt, state.as.object, "results", results);
    djs_object_set(rt, state.as.object, "promise", promise);
    djs_object_set(rt, state.as.object, "items", items);
    return state;
}

/* Attach the reaction handlers to every item of a combine. */
static void promise_combine_attach(djs_runtime_t *rt, djs_value_t state,
                                   djs_native_fn on_ok, djs_native_fn on_err) {
    djs_value_t items = promise_state_value(rt, state, "items");
    djs_value_t count_value = promise_state_value(rt, state, "count");
    size_t count = count_value.tag == DJS_TAG_NUMBER ? (size_t)count_value.as.number : 0;
    for (size_t i = 0; i < count; i++) {
        djs_env_t *closure = make_closure(rt);
        closure_set(rt, closure, "state", state);
        closure_set(rt, closure, "index", djs_number((double)i));
        djs_object_t *resolved = function_with_closure(rt, on_ok, "", 0, closure);
        djs_object_t *rejected = function_with_closure(rt, on_err, "", 0, closure);
        if (resolved == NULL || rejected == NULL) continue;
        djs_value_t item = djs_array_get(rt, items.as.object, i);
        djs_value_t item_promise = item;
        if (!(djs_is_object(item) && item.as.object->class == DJS_CLASS_PROMISE)) {
            item_promise = djs_promise_new(rt);
            djs_promise_resolve(rt, item_promise, item);
        }
        djs_promise_then(rt, item_promise, djs_object_value(resolved),
                         djs_object_value(rejected));
    }
}

/* Resolve an empty combine with its (empty) result list. */
static void promise_resolve_empty(djs_runtime_t *rt, djs_value_t state) {
    djs_value_t count = promise_state_value(rt, state, "count");
    if (count.tag != DJS_TAG_NUMBER || count.as.number != 0.0) return;
    djs_promise_resolve(rt, promise_state_value(rt, state, "promise"),
                        promise_state_value(rt, state, "results"));
}

static djs_value_t promise_all_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t state = promise_combine_setup(rt, args, arg_count);
    promise_combine_attach(rt, state, promise_all_on_ok, promise_all_on_err);
    promise_resolve_empty(rt, state);
    return promise_state_value(rt, state, "promise");
}

static djs_value_t promise_race_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t state = promise_combine_setup(rt, args, arg_count);
    promise_combine_attach(rt, state, promise_race_on_ok, promise_race_on_err);
    return promise_state_value(rt, state, "promise");
}

/* ------------------------------------------------------------------- Dates */

#define DJS_DAY_MS 86400000.0

static void civil_from_days(int64_t days, int *year, int *month, int *day) {
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t dd = doy - (153 * mp + 2) / 5 + 1;
    int64_t mm = mp < 10 ? mp + 3 : mp - 9;
    *year = (int)(y + (mm <= 2 ? 1 : 0));
    *month = (int)mm;
    *day = (int)dd;
}

static int64_t days_from_civil(int year, int month, int day) {
    year -= month <= 2 ? 1 : 0;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int64_t yoe = year - era * 400;
    int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void date_parts(double milliseconds, int *year, int *month, int *day,
                       int *hour, int *minute, int *second, int *milli) {
    double days = num_floor(milliseconds / DJS_DAY_MS);
    double rest = milliseconds - days * DJS_DAY_MS;
    civil_from_days((int64_t)days, year, month, day);
    *hour = (int)(rest / 3600000.0);
    *minute = (int)(rest / 60000.0) % 60;
    *second = (int)(rest / 1000.0) % 60;
    *milli = (int)rest % 1000;
}

static int date_weekday(double milliseconds) {
    double days = num_floor(milliseconds / DJS_DAY_MS);
    int64_t weekday = ((int64_t)days + 4) % 7;
    if (weekday < 0) weekday += 7;
    return (int)weekday;
}

static bool read_digits(const char *text, size_t length, size_t *index, size_t count,
                        int *out) {
    if (*index + count > length) return false;
    int value = 0;
    for (size_t i = 0; i < count; i++) {
        if (!builtins_digit(text[*index + i])) return false;
        value = value * 10 + (text[*index + i] - '0');
    }
    *index += count;
    *out = value;
    return true;
}

static double date_parse(const char *text) {
    size_t length = djs_strlen(text);
    size_t index = 0;
    while (index < length && builtins_space(text[index])) index++;
    size_t end = length;
    while (end > index && builtins_space(text[end - 1])) end--;
    bool numeric = true;
    for (size_t i = index; i < end; i++) {
        char c = text[i];
        if (!builtins_digit(c) && c != '-' && c != '+' && c != '.' && c != 'e' && c != 'E') {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        double value = 0.0;
        if (djs_string_to_number(text + index, end - index, &value)) return value;
        return 0.0 / 0.0;
    }
    int year = 0;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int milli = 0;
    if (!read_digits(text, end, &index, 4, &year)) return 0.0 / 0.0;
    if (index < end && text[index] == '-') index++;
    if (!read_digits(text, end, &index, 2, &month)) return 0.0 / 0.0;
    if (index < end && text[index] == '-') index++;
    if (!read_digits(text, end, &index, 2, &day)) day = 1;
    if (index < end && (text[index] == 'T' || text[index] == 't' || text[index] == ' ')) {
        index++;
        if (!read_digits(text, end, &index, 2, &hour)) return 0.0 / 0.0;
        if (index < end && text[index] == ':') index++;
        if (!read_digits(text, end, &index, 2, &minute)) minute = 0;
        if (index < end && text[index] == ':') {
            index++;
            if (!read_digits(text, end, &index, 2, &second)) second = 0;
        }
        if (index < end && (text[index] == '.' || text[index] == ',')) {
            index++;
            size_t fraction_digits = 0;
            while (index < end && builtins_digit(text[index])) {
                if (fraction_digits < 3) milli = milli * 10 + (text[index] - '0');
                fraction_digits++;
                index++;
            }
            while (fraction_digits > 0 && fraction_digits < 3) {
                milli *= 10;
                fraction_digits++;
            }
        }
    }
    int64_t days = days_from_civil(year, month, day);
    return (double)days * DJS_DAY_MS + (double)hour * 3600000.0 +
           (double)minute * 60000.0 + (double)second * 1000.0 + (double)milli;
}

static djs_object_t *date_this(djs_runtime_t *rt, djs_value_t this_value) {
    if (!djs_is_object(this_value) || this_value.as.object->class != DJS_CLASS_DATE) {
        djs_throw_type_error(rt, "Date method called on incompatible receiver");
    }
    return this_value.as.object;
}

/* Years 0..99 mean 1900..1999 in the Date constructor and Date.UTC. */
static int date_year(int year) {
    return (year >= 0 && year <= 99) ? year + 1900 : year;
}

static djs_value_t date_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)this_value;
    double milliseconds;
    if (arg_count == 0) {
        milliseconds = djs_host_time_ms();
    } else if (arg_count == 1) {
        djs_value_t value = args[0];
        if (djs_is_object(value)) value = djs_to_primitive(rt, value, 0);
        if (djs_is_string(value)) milliseconds = date_parse(value.as.string->bytes);
        else milliseconds = djs_to_number(rt, value).as.number;
    } else {
        int year = date_year((int)djs_to_number(rt, args[0]).as.number);
        int month = (int)djs_to_number(rt, args[1]).as.number;
        int day = arg_count > 2 ? (int)djs_to_number(rt, args[2]).as.number : 1;
        int hour = arg_count > 3 ? (int)djs_to_number(rt, args[3]).as.number : 0;
        int minute = arg_count > 4 ? (int)djs_to_number(rt, args[4]).as.number : 0;
        int second = arg_count > 5 ? (int)djs_to_number(rt, args[5]).as.number : 0;
        int milli = arg_count > 6 ? (int)djs_to_number(rt, args[6]).as.number : 0;
        int64_t days = days_from_civil(year, month + 1, day);
        milliseconds = (double)days * DJS_DAY_MS + (double)hour * 3600000.0 +
                       (double)minute * 60000.0 + (double)second * 1000.0 + (double)milli;
    }
    djs_object_t *date = djs_object_new(rt, rt->date_prototype);
    if (date == NULL) return DJS_UNDEFINED;
    date->class = DJS_CLASS_DATE;
    date->date_value = milliseconds;
    return djs_object_value(date);
}

static djs_value_t date_now_fn(djs_runtime_t *rt, djs_value_t this_value,
                               djs_value_t *args, size_t arg_count) {
    (void)rt; (void)this_value; (void)args; (void)arg_count;
    return djs_number(djs_host_time_ms());
}

static djs_value_t date_parse_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t text = djs_to_string(rt, arg_at(args, arg_count, 0));
    return djs_number(date_parse(text.as.string->bytes));
}

static djs_value_t date_utc_fn(djs_runtime_t *rt, djs_value_t this_value,
                               djs_value_t *args, size_t arg_count) {
    (void)this_value;
    int year = date_year((int)djs_to_number(rt, arg_at(args, arg_count, 0)).as.number);
    int month = arg_count > 1 ? (int)djs_to_number(rt, args[1]).as.number : 0;
    int day = arg_count > 2 ? (int)djs_to_number(rt, args[2]).as.number : 1;
    int hour = arg_count > 3 ? (int)djs_to_number(rt, args[3]).as.number : 0;
    int minute = arg_count > 4 ? (int)djs_to_number(rt, args[4]).as.number : 0;
    int second = arg_count > 5 ? (int)djs_to_number(rt, args[5]).as.number : 0;
    int milli = arg_count > 6 ? (int)djs_to_number(rt, args[6]).as.number : 0;
    int64_t days = days_from_civil(year, month + 1, day);
    double milliseconds = (double)days * DJS_DAY_MS + (double)hour * 3600000.0 +
                          (double)minute * 60000.0 + (double)second * 1000.0 + (double)milli;
    return djs_number(milliseconds);
}

static djs_value_t date_get_time_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    return djs_number(date_this(rt, this_value)->date_value);
}

static djs_value_t date_get_full_year_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *date = date_this(rt, this_value);
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int milli = 0;
    date_parts(date->date_value, &year, &month, &day, &hour, &minute, &second, &milli);
    return djs_number((double)year);
}

static djs_value_t date_get_month_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *date = date_this(rt, this_value);
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int milli = 0;
    date_parts(date->date_value, &year, &month, &day, &hour, &minute, &second, &milli);
    return djs_number((double)(month - 1));
}

static djs_value_t date_get_date_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *date = date_this(rt, this_value);
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int milli = 0;
    date_parts(date->date_value, &year, &month, &day, &hour, &minute, &second, &milli);
    return djs_number((double)day);
}

static djs_value_t date_get_day_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    return djs_number((double)date_weekday(date_this(rt, this_value)->date_value));
}

static djs_value_t date_get_hours_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *date = date_this(rt, this_value);
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int milli = 0;
    date_parts(date->date_value, &year, &month, &day, &hour, &minute, &second, &milli);
    return djs_number((double)hour);
}

static djs_value_t date_get_minutes_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *date = date_this(rt, this_value);
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int milli = 0;
    date_parts(date->date_value, &year, &month, &day, &hour, &minute, &second, &milli);
    return djs_number((double)minute);
}

static djs_value_t date_get_seconds_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *date = date_this(rt, this_value);
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int milli = 0;
    date_parts(date->date_value, &year, &month, &day, &hour, &minute, &second, &milli);
    return djs_number((double)second);
}

static djs_value_t date_get_milliseconds_fn(djs_runtime_t *rt, djs_value_t this_value,
                                            djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *date = date_this(rt, this_value);
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int milli = 0;
    date_parts(date->date_value, &year, &month, &day, &hour, &minute, &second, &milli);
    return djs_number((double)milli);
}

static djs_value_t date_get_timezone_offset_fn(djs_runtime_t *rt, djs_value_t this_value,
                                               djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    date_this(rt, this_value);
    return djs_number(0.0);
}

static djs_value_t date_to_iso_string_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *date = date_this(rt, this_value);
    double milliseconds = date->date_value;
    if (milliseconds != milliseconds) return make_cstring(rt, "Invalid Date");
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int milli = 0;
    date_parts(milliseconds, &year, &month, &day, &hour, &minute, &second, &milli);
    char buffer[64];
    size_t position = 0;
    if (year >= 0 && year <= 9999) {
        buffer[position++] = (char)('0' + (year / 1000) % 10);
        buffer[position++] = (char)('0' + (year / 100) % 10);
        buffer[position++] = (char)('0' + (year / 10) % 10);
        buffer[position++] = (char)('0' + year % 10);
    } else {
        position += format_u64((uint64_t)(year < 0 ? -year : year), buffer + position);
    }
    buffer[position++] = '-';
    buffer[position++] = (char)('0' + (month / 10) % 10);
    buffer[position++] = (char)('0' + month % 10);
    buffer[position++] = '-';
    buffer[position++] = (char)('0' + (day / 10) % 10);
    buffer[position++] = (char)('0' + day % 10);
    buffer[position++] = 'T';
    buffer[position++] = (char)('0' + (hour / 10) % 10);
    buffer[position++] = (char)('0' + hour % 10);
    buffer[position++] = ':';
    buffer[position++] = (char)('0' + (minute / 10) % 10);
    buffer[position++] = (char)('0' + minute % 10);
    buffer[position++] = ':';
    buffer[position++] = (char)('0' + (second / 10) % 10);
    buffer[position++] = (char)('0' + second % 10);
    buffer[position++] = '.';
    buffer[position++] = (char)('0' + (milli / 100) % 10);
    buffer[position++] = (char)('0' + (milli / 10) % 10);
    buffer[position++] = (char)('0' + milli % 10);
    buffer[position++] = 'Z';
    buffer[position] = '\0';
    return make_cstring(rt, buffer);
}

static djs_value_t date_to_json_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    return date_to_iso_string_fn(rt, this_value, NULL, 0);
}

static djs_value_t date_to_string_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_object_t *date = date_this(rt, this_value);
    double milliseconds = date->date_value;
    if (milliseconds != milliseconds) return make_cstring(rt, "Invalid Date");
    static const char *weekdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int milli = 0;
    date_parts(milliseconds, &year, &month, &day, &hour, &minute, &second, &milli);
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    sb_append_cstr(&builder, weekdays[date_weekday(milliseconds)]);
    sb_append_cstr(&builder, " ");
    sb_append_cstr(&builder, months[month - 1]);
    sb_append_char(&builder, ' ');
    char buffer[32];
    size_t length = format_u64((uint64_t)(day < 0 ? -day : day), buffer);
    if (day < 10) {
        sb_append_char(&builder, '0');
    }
    sb_append_bytes(&builder, buffer, length);
    sb_append_char(&builder, ' ');
    length = format_u64((uint64_t)(year < 0 ? -year : year), buffer);
    sb_append_bytes(&builder, buffer, length);
    sb_append_char(&builder, ' ');
    length = format_u64((uint64_t)hour, buffer);
    if (hour < 10) sb_append_char(&builder, '0');
    sb_append_bytes(&builder, buffer, length);
    sb_append_char(&builder, ':');
    length = format_u64((uint64_t)minute, buffer);
    if (minute < 10) sb_append_char(&builder, '0');
    sb_append_bytes(&builder, buffer, length);
    sb_append_char(&builder, ':');
    length = format_u64((uint64_t)second, buffer);
    if (second < 10) sb_append_char(&builder, '0');
    sb_append_bytes(&builder, buffer, length);
    sb_append_cstr(&builder, " GMT+0000");
    return make_cstring(rt, sb_finish(&builder));
}

/* --------------------------------------------------------- Function.prototype */

static djs_value_t function_call_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    if (!djs_value_is_callable(this_value)) {
        djs_throw_type_error(rt, "Function.prototype.call called on incompatible receiver");
    }
    djs_value_t this_arg = arg_at(args, arg_count, 0);
    size_t count = arg_count > 1 ? arg_count - 1 : 0;
    return djs_call(rt, this_value, this_arg, count > 0 ? args + 1 : NULL, count);
}

static djs_value_t function_apply_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    if (!djs_value_is_callable(this_value)) {
        djs_throw_type_error(rt, "Function.prototype.apply called on incompatible receiver");
    }
    djs_value_t this_arg = arg_at(args, arg_count, 0);
    djs_value_t list = arg_at(args, arg_count, 1);
    size_t count = 0;
    djs_value_t *values = NULL;
    if (!djs_is_nullish(list)) {
        if (!djs_is_object(list)) {
            djs_throw_type_error(rt, "Function.prototype.apply argument list must be an array");
        }
        count = length_index(rt, list.as.object);
        if (count > 0) {
            values = (djs_value_t *)djs_arena_alloc(rt, count * sizeof(*values));
            if (values == NULL) return DJS_UNDEFINED;
            for (size_t i = 0; i < count; i++) {
                values[i] = indexed_get(rt, list.as.object, i);
            }
        }
    }
    return djs_call(rt, this_value, this_arg, values, count);
}

static djs_value_t function_bound_call(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t target = closure_get(rt, "target");
    djs_value_t bound_this = closure_get(rt, "bound_this");
    djs_value_t bound_args = closure_get(rt, "bound_args");
    size_t bound_count = bound_args.tag == DJS_TAG_OBJECT
        ? djs_array_length(bound_args.as.object) : 0;
    size_t total = bound_count + arg_count;
    djs_value_t *values = NULL;
    if (total > 0) {
        values = (djs_value_t *)djs_arena_alloc(rt, total * sizeof(*values));
        if (values == NULL) return DJS_UNDEFINED;
        for (size_t i = 0; i < bound_count; i++) {
            values[i] = djs_array_get(rt, bound_args.as.object, i);
        }
        for (size_t i = 0; i < arg_count; i++) values[bound_count + i] = args[i];
    }
    return djs_call(rt, target, bound_this, values, total);
}

static djs_value_t function_bind_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    if (!djs_value_is_callable(this_value)) {
        djs_throw_type_error(rt, "Function.prototype.bind called on incompatible receiver");
    }
    djs_value_t bound_args = djs_make_array(rt, NULL, 0);
    for (size_t i = 1; i < arg_count; i++) {
        djs_array_push(rt, bound_args.as.object, args[i]);
    }
    djs_env_t *closure = make_closure(rt);
    closure_set(rt, closure, "target", this_value);
    closure_set(rt, closure, "bound_this", arg_at(args, arg_count, 0));
    closure_set(rt, closure, "bound_args", bound_args);
    djs_object_t *bound = function_with_closure(rt, function_bound_call, "bound", 0,
                                                closure);
    if (bound == NULL) return DJS_UNDEFINED;
    return djs_object_value(bound);
}

static djs_value_t function_to_string_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    sb_append_cstr(&builder, "function ");
    if (djs_is_object(this_value)) {
        djs_value_t name = djs_object_get(rt, this_value.as.object, "name");
        if (djs_is_string(name)) sb_append_cstr(&builder, name.as.string->bytes);
    }
    sb_append_cstr(&builder, "() { [native code] }");
    return make_cstring(rt, sb_finish(&builder));
}

/* --------------------------------------------------- Object.prototype bits */

static djs_value_t object_has_own_property_fn(djs_runtime_t *rt, djs_value_t this_value,
                                              djs_value_t *args, size_t arg_count) {
    if (!djs_is_object(this_value)) {
        djs_throw_type_error(rt, "hasOwnProperty called on incompatible receiver");
    }
    djs_value_t key = arg_at(args, arg_count, 0);
    const char *name = djs_value_cstr(rt, key);
    if (this_value.as.object->class == DJS_CLASS_ARRAY) {
        size_t index;
        if (djs_canonical_index(name, &index)) {
            return djs_bool(index < djs_array_length(this_value.as.object));
        }
    }
    return djs_bool(djs_property_get(this_value.as.object, name) != NULL);
}

static djs_value_t object_to_string_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)rt; (void)args; (void)arg_count;
    if (djs_is_undefined(this_value)) return make_cstring(rt, "[object Undefined]");
    if (djs_is_null(this_value)) return make_cstring(rt, "[object Null]");
    if (djs_is_object(this_value)) {
        switch (this_value.as.object->class) {
        case DJS_CLASS_ARRAY: return make_cstring(rt, "[object Array]");
        case DJS_CLASS_FUNCTION: return make_cstring(rt, "[object Function]");
        case DJS_CLASS_ERROR: return make_cstring(rt, "[object Error]");
        case DJS_CLASS_DATE: return make_cstring(rt, "[object Date]");
        case DJS_CLASS_MAP: return make_cstring(rt, "[object Map]");
        case DJS_CLASS_SET: return make_cstring(rt, "[object Set]");
        case DJS_CLASS_PROMISE: return make_cstring(rt, "[object Promise]");
        default: break;
        }
        return make_cstring(rt, "[object Object]");
    }
    if (djs_is_number(this_value)) return make_cstring(rt, "[object Number]");
    if (djs_is_string(this_value)) return make_cstring(rt, "[object String]");
    if (djs_is_bool(this_value)) return make_cstring(rt, "[object Boolean]");
    return make_cstring(rt, "[object Undefined]");
}

static djs_value_t object_value_of_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    if (djs_is_object(this_value)) return this_value;
    return djs_to_primitive(rt, this_value, 0);
}

static djs_value_t object_is_prototype_of_fn(djs_runtime_t *rt, djs_value_t this_value,
                                             djs_value_t *args, size_t arg_count) {
    djs_value_t value = arg_at(args, arg_count, 0);
    if (!djs_is_object(this_value) || !djs_is_object(value)) return djs_bool(false);
    djs_value_t prototype = this_value.as.object->prototype;
    while (prototype.tag == DJS_TAG_OBJECT) {
        if (prototype.as.object == value.as.object) return djs_bool(true);
        prototype = prototype.as.object->prototype;
    }
    return djs_bool(false);
}

static djs_value_t object_property_is_enumerable_fn(djs_runtime_t *rt,
                                                     djs_value_t this_value,
                                                     djs_value_t *args, size_t arg_count) {
    if (!djs_is_object(this_value)) {
        djs_throw_type_error(rt, "propertyIsEnumerable called on incompatible receiver");
    }
    const char *name = djs_value_cstr(rt, arg_at(args, arg_count, 0));
    djs_property_t *property = djs_property_get(this_value.as.object, name);
    return djs_bool(property != NULL &&
                    (property->flags & DJS_PROP_ENUMERABLE) != 0);
}

/* ----------------------------------------------------------- iterator glue */

static djs_value_t array_iterator_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    if (!djs_is_object(this_value)) {
        djs_throw_type_error(rt, "iterator called on incompatible receiver");
    }
    return make_array_iterator(rt, this_value);
}

static djs_value_t string_iterator_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    return make_string_iterator(rt, djs_to_string(rt, this_value));
}

static djs_value_t map_iterator_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    return make_map_iterator(rt, map_this(rt, this_value), "entries");
}

static djs_value_t set_iterator_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    return make_map_iterator(rt, map_this(rt, this_value), "values");
}

/* ------------------------------------------------------------------ Globals */

static djs_value_t global_is_nan_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value;
    double number = djs_to_number(rt, arg_at(args, arg_count, 0)).as.number;
    return djs_bool(number != number);
}

static djs_value_t global_is_finite_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)this_value;
    double number = djs_to_number(rt, arg_at(args, arg_count, 0)).as.number;
    return djs_bool(number == number && number <= 1.7976931348623157e308 &&
                    number >= -1.7976931348623157e308);
}

static bool uri_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
           c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')';
}

static bool uri_reserved(unsigned char c) {
    return c == ';' || c == '/' || c == '?' || c == ':' || c == '@' || c == '&' ||
           c == '=' || c == '+' || c == '$' || c == ',' || c == '#';
}

static djs_value_t encode_uri_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t text = djs_to_string(rt, arg_at(args, arg_count, 0));
    static const char hex[] = "0123456789ABCDEF";
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    const char *bytes = text.as.string->bytes;
    size_t length = text.as.string->length;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (uri_unreserved(c) || uri_reserved(c)) {
            sb_append_char(&builder, (char)c);
            continue;
        }
        char escape[4];
        escape[0] = '%';
        escape[1] = hex[(c >> 4) & 0x0Fu];
        escape[2] = hex[c & 0x0Fu];
        escape[3] = '\0';
        sb_append_bytes(&builder, escape, 3);
    }
    return make_cstring(rt, sb_finish(&builder));
}

static djs_value_t encode_uri_component_fn(djs_runtime_t *rt, djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t text = djs_to_string(rt, arg_at(args, arg_count, 0));
    static const char hex[] = "0123456789ABCDEF";
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    const char *bytes = text.as.string->bytes;
    size_t length = text.as.string->length;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (uri_unreserved(c)) {
            sb_append_char(&builder, (char)c);
            continue;
        }
        char escape[4];
        escape[0] = '%';
        escape[1] = hex[(c >> 4) & 0x0Fu];
        escape[2] = hex[c & 0x0Fu];
        escape[3] = '\0';
        sb_append_bytes(&builder, escape, 3);
    }
    return make_cstring(rt, sb_finish(&builder));
}

static djs_value_t decode_uri_fn(djs_runtime_t *rt, djs_value_t this_value,
                                 djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t text = djs_to_string(rt, arg_at(args, arg_count, 0));
    const char *bytes = text.as.string->bytes;
    size_t length = text.as.string->length;
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] != '%') {
            sb_append_char(&builder, bytes[i]);
            continue;
        }
        if (i + 2 >= length) djs_throw_error(rt, "URIError", "URI malformed");
        int high = digit_value(bytes[i + 1]);
        int low = digit_value(bytes[i + 2]);
        if (high < 0 || high > 15 || low < 0 || low > 15) {
            djs_throw_error(rt, "URIError", "URI malformed");
        }
        unsigned char decoded = (unsigned char)((high << 4) | low);
        if (uri_reserved(decoded)) {
            sb_append_bytes(&builder, bytes + i, 3);
        } else {
            sb_append_char(&builder, (char)decoded);
        }
        i += 2;
    }
    return make_cstring(rt, sb_finish(&builder));
}

static djs_value_t decode_uri_component_fn(djs_runtime_t *rt, djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t text = djs_to_string(rt, arg_at(args, arg_count, 0));
    const char *bytes = text.as.string->bytes;
    size_t length = text.as.string->length;
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] != '%') {
            sb_append_char(&builder, bytes[i]);
            continue;
        }
        if (i + 2 >= length) djs_throw_error(rt, "URIError", "URI malformed");
        int high = digit_value(bytes[i + 1]);
        int low = digit_value(bytes[i + 2]);
        if (high < 0 || high > 15 || low < 0 || low > 15) {
            djs_throw_error(rt, "URIError", "URI malformed");
        }
        sb_append_char(&builder, (char)((high << 4) | low));
        i += 2;
    }
    return make_cstring(rt, sb_finish(&builder));
}

static djs_value_t queue_microtask_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t callback = arg_at(args, arg_count, 0);
    if (!djs_value_is_callable(callback)) {
        djs_throw_type_error(rt, "queueMicrotask callback is not a function");
    }
    djs_queue_microtask(rt, callback, NULL, 0);
    return DJS_UNDEFINED;
}

static djs_value_t symbol_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t description = arg_at(args, arg_count, 0);
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    sb_append_cstr(&builder, "Symbol(");
    if (!djs_is_undefined(description)) {
        djs_value_t text = djs_to_string(rt, description);
        sb_append_cstr(&builder, text.as.string->bytes);
    }
    sb_append_char(&builder, ')');
    return make_cstring(rt, sb_finish(&builder));
}

static djs_value_t function_constructor_fn(djs_runtime_t *rt, djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    sb_append_cstr(&builder, "(function anonymous(");
    size_t limit = arg_count > 0 ? arg_count - 1 : 0;
    for (size_t i = 0; i < limit; i++) {
        if (i > 0) sb_append_char(&builder, ',');
        sb_append_cstr(&builder, djs_value_cstr(rt, args[i]));
    }
    sb_append_cstr(&builder, "){");
    if (arg_count > 0) sb_append_cstr(&builder, djs_value_cstr(rt, args[arg_count - 1]));
    sb_append_cstr(&builder, "})");
    char *source = sb_finish(&builder);
    char *error = NULL;
    djs_value_t function = djs_eval_source(rt, source, "Function", &error);
    if (error != NULL) {
        djs_throw_error(rt, "SyntaxError", error);
    }
    return function;
}

static djs_value_t string_to_string_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    return djs_to_string(rt, this_value);
}

static djs_value_t number_value_of_fn(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    (void)args; (void)arg_count;
    return djs_to_number(rt, this_value);
}

/* ------------------------------------------------------------- installation */

/* ---------------------------------------------------------- value inspection */

/* Quote a string the way donejs' util.inspect does. */
static void inspect_quoted(djs_string_builder_t *builder, const char *text,
                           size_t length) {
    static const char hex[] = "0123456789abcdef";
    sb_append_char(builder, '\'');
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        switch (c) {
        case '\'': sb_append_cstr(builder, "\\'"); break;
        case '\\': sb_append_cstr(builder, "\\\\"); break;
        case '\n': sb_append_cstr(builder, "\\n"); break;
        case '\r': sb_append_cstr(builder, "\\r"); break;
        case '\t': sb_append_cstr(builder, "\\t"); break;
        case '\b': sb_append_cstr(builder, "\\b"); break;
        case '\f': sb_append_cstr(builder, "\\f"); break;
        case '\v': sb_append_cstr(builder, "\\v"); break;
        default:
            if (c < 0x20) {
                char escape[5];
                escape[0] = '\\';
                escape[1] = 'x';
                escape[2] = hex[(c >> 4) & 0xF];
                escape[3] = hex[c & 0xF];
                escape[4] = '\0';
                sb_append_cstr(builder, escape);
            } else {
                sb_append_char(builder, (char)c);
            }
        }
    }
    sb_append_char(builder, '\'');
}

/* Render a value the way donejs' util.inspect does. */
static void inspect_value(djs_runtime_t *rt, djs_string_builder_t *builder,
                          djs_value_t value, int depth) {
    switch (value.tag) {
    case DJS_TAG_UNDEFINED:
        sb_append_cstr(builder, "undefined");
        return;
    case DJS_TAG_NULL:
        sb_append_cstr(builder, "null");
        return;
    case DJS_TAG_BOOL:
        sb_append_cstr(builder, value.as.boolean ? "true" : "false");
        return;
    case DJS_TAG_NUMBER: {
        char text[64];
        djs_number_to_string(value.as.number, text, sizeof(text));
        sb_append_cstr(builder, text);
        return;
    }
    case DJS_TAG_STRING:
        inspect_quoted(builder, value.as.string->bytes, value.as.string->length);
        return;
    case DJS_TAG_OBJECT:
        break;
    }
    djs_object_t *object = value.as.object;
    if (object == NULL) {
        sb_append_cstr(builder, "null");
        return;
    }
    if (object->function != NULL) {
        sb_append_cstr(builder, "[Function");
        const char *name = "";
        if (object->function->name.tag == DJS_TAG_STRING) {
            name = object->function->name.as.string->bytes;
        }
        if (name[0] != '\0') {
            sb_append_cstr(builder, ": ");
            sb_append_cstr(builder, name);
        } else {
            sb_append_cstr(builder, " (anonymous)");
        }
        sb_append_char(builder, ']');
        return;
    }
    if (object->class == DJS_CLASS_ERROR) {
        djs_value_t name = djs_object_get(rt, object, "name");
        djs_value_t message = djs_object_get(rt, object, "message");
        sb_append_cstr(builder, name.tag == DJS_TAG_STRING ? name.as.string->bytes : "Error");
        if (message.tag == DJS_TAG_STRING && message.as.string->length > 0) {
            sb_append_cstr(builder, ": ");
            sb_append_cstr(builder, message.as.string->bytes);
        }
        return;
    }
    if (depth > 4) {
        sb_append_cstr(builder, "[Object]");
        return;
    }
    if (object->class == DJS_CLASS_ARRAY) {
        sb_append_cstr(builder, "[ ");
        size_t length = djs_array_length(object);
        for (size_t i = 0; i < length; i++) {
            if (i > 0) sb_append_cstr(builder, ", ");
            inspect_value(rt, builder, djs_array_get(rt, object, i), depth + 1);
        }
        sb_append_cstr(builder, " ]");
        return;
    }
    if (object->class == DJS_CLASS_DATE) {
        sb_append_cstr(builder, "[Date]");
        return;
    }
    if (object->class == DJS_CLASS_REGEXP) {
        sb_append_char(builder, '/');
        sb_append_cstr(builder, object->regexp_source ? object->regexp_source : "");
        sb_append_char(builder, '/');
        sb_append_cstr(builder, object->regexp_flags ? object->regexp_flags : "");
        return;
    }
    if (object->class == DJS_CLASS_PROMISE) {
        sb_append_cstr(builder, "[Promise]");
        return;
    }
    sb_append_cstr(builder, "{ ");
    bool first = true;
    for (djs_property_t *property = djs_property_first(object); property != NULL;
         property = property->next) {
        if ((property->flags & DJS_PROP_PRIVATE) != 0) continue;
        if (!first) sb_append_cstr(builder, ", ");
        first = false;
        const char *key = property->key.tag == DJS_TAG_STRING
                              ? property->key.as.string->bytes
                              : djs_value_cstr(rt, property->key);
        sb_append_cstr(builder, key);
        sb_append_cstr(builder, ": ");
        inspect_value(rt, builder, property->value, depth + 1);
    }
    sb_append_cstr(builder, " }");
}

/* ------------------------------------------------------------ console output */

/* Write one line to the runtime output. */
static void console_line(djs_runtime_t *rt, djs_string_builder_t *builder) {
    sb_append_char(builder, '\n');
    char *text = sb_finish(builder);
    if (text != NULL) djs_write(rt, text, djs_strlen(text));
}

/* Format console arguments the way donejs' util.format does: a leading
 * string is a format string, every other value is inspected. */
static void console_format(djs_runtime_t *rt, djs_string_builder_t *builder,
                           djs_value_t *args, size_t arg_count) {
    if (arg_count == 0) return;
    if (!djs_is_string(args[0]) || arg_count == 1) {
        for (size_t i = 0; i < arg_count; i++) {
            if (i > 0) sb_append_char(builder, ' ');
            if (djs_is_string(args[i])) {
                sb_append_cstr(builder, args[i].as.string->bytes);
            } else {
                inspect_value(rt, builder, args[i], 0);
            }
        }
        return;
    }
    const char *format = args[0].as.string->bytes;
    size_t length = args[0].as.string->length;
    size_t next = 1;
    for (size_t i = 0; i < length; i++) {
        char c = format[i];
        if (c != '%') {
            sb_append_char(builder, c);
            continue;
        }
        if (i + 1 >= length) {
            sb_append_char(builder, '%');
            break;
        }
        char spec = format[++i];
        if (spec == '%') {
            sb_append_char(builder, '%');
            continue;
        }
        if (next >= arg_count) {
            sb_append_cstr(builder, "%!");
            sb_append_char(builder, spec);
            sb_append_cstr(builder, "(undefined)");
            continue;
        }
        djs_value_t value = args[next++];
        switch (spec) {
        case 's':
            if (djs_is_string(value)) {
                sb_append_cstr(builder, value.as.string->bytes);
            } else {
                inspect_value(rt, builder, value, 0);
            }
            break;
        case 'd':
        case 'i': {
            double number = djs_to_number(rt, value).as.number;
            if (number != number) {
                sb_append_cstr(builder, "NaN");
                break;
            }
            if (number > 9.2233720368547758e18 || number < -9.2233720368547758e18) {
                sb_append_cstr(builder, djs_value_cstr(rt, djs_number(number)));
                break;
            }
            char text[64];
            djs_number_to_string((double)(long long)number, text, sizeof(text));
            sb_append_cstr(builder, text);
            break;
        }
        case 'f': {
            double number = djs_to_number(rt, value).as.number;
            sb_append_cstr(builder, djs_value_cstr(rt, djs_number(number)));
            break;
        }
        case 'j': {
            djs_value_t json_args[1] = { value };
            djs_value_t text = json_stringify_fn(rt, DJS_UNDEFINED, json_args, 1);
            if (text.tag == DJS_TAG_STRING) sb_append_cstr(builder, text.as.string->bytes);
            break;
        }
        case 'o':
        case 'O':
            inspect_value(rt, builder, value, 0);
            break;
        default:
            sb_append_char(builder, '%');
            sb_append_char(builder, spec);
            break;
        }
    }
    if (next < arg_count) {
        sb_append_char(builder, ' ');
        console_format(rt, builder, args + next, arg_count - next);
    }
}

static djs_value_t console_write_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    console_format(rt, &builder, args, arg_count);
    console_line(rt, &builder);
    return DJS_UNDEFINED;
}

/* Append one value to a console line: strings raw, everything else
 * inspected. */
static void console_append_value(djs_runtime_t *rt, djs_string_builder_t *builder,
                                 djs_value_t value) {
    if (djs_is_string(value)) {
        sb_append_cstr(builder, value.as.string->bytes);
    } else {
        inspect_value(rt, builder, value, 0);
    }
}

static djs_value_t console_dir_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    inspect_value(rt, &builder, arg_at(args, arg_count, 0), 0);
    console_line(rt, &builder);
    return DJS_UNDEFINED;
}

static djs_value_t console_trace_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    sb_append_cstr(&builder, "Trace:");
    for (size_t i = 0; i < arg_count; i++) {
        sb_append_char(&builder, ' ');
        console_append_value(rt, &builder, args[i]);
    }
    console_line(rt, &builder);
    return DJS_UNDEFINED;
}

static djs_value_t console_assert_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value;
    if (djs_to_boolean(arg_at(args, arg_count, 0))) return DJS_UNDEFINED;
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    sb_append_cstr(&builder, "Assertion failed:");
    for (size_t i = 1; i < arg_count; i++) {
        sb_append_char(&builder, ' ');
        console_append_value(rt, &builder, args[i]);
    }
    console_line(rt, &builder);
    return DJS_UNDEFINED;
}

static djs_value_t console_group_end_fn(djs_runtime_t *rt, djs_value_t this_value,
                                        djs_value_t *args, size_t arg_count) {
    (void)rt; (void)this_value; (void)args; (void)arg_count;
    return DJS_UNDEFINED;
}

/* The console object a stateful method works on. */
static djs_object_t *console_object(djs_runtime_t *rt) {
    djs_value_t console = closure_get(rt, "console");
    if (!djs_is_object(console)) return NULL;
    return console.as.object;
}

/* The label of a console timer or counter. */
static const char *console_label(djs_value_t *args, size_t arg_count) {
    djs_value_t label = arg_at(args, arg_count, 0);
    if (djs_is_string(label)) return label.as.string->bytes;
    return "default";
}

static djs_value_t console_time_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_object_t *console = console_object(rt);
    if (console == NULL) return DJS_UNDEFINED;
    djs_value_t timers = djs_object_get(rt, console, "_timers");
    if (!djs_is_object(timers)) return DJS_UNDEFINED;
    djs_object_set(rt, timers.as.object, console_label(args, arg_count),
                   djs_number(djs_host_time_ms()));
    return DJS_UNDEFINED;
}

static djs_value_t console_time_end_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_object_t *console = console_object(rt);
    if (console == NULL) return DJS_UNDEFINED;
    djs_value_t timers = djs_object_get(rt, console, "_timers");
    if (!djs_is_object(timers)) return DJS_UNDEFINED;
    const char *label = console_label(args, arg_count);
    djs_value_t start = djs_object_get(rt, timers.as.object, label);
    if (!djs_is_number(start)) return DJS_UNDEFINED;
    djs_property_delete(rt, timers.as.object, label);
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    sb_append_cstr(&builder, label);
    sb_append_cstr(&builder, ": ");
    sb_append_cstr(&builder,
                   djs_value_cstr(rt, djs_number(djs_host_time_ms() - start.as.number)));
    sb_append_cstr(&builder, "ms");
    console_line(rt, &builder);
    return DJS_UNDEFINED;
}

static djs_value_t console_time_log_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_object_t *console = console_object(rt);
    if (console == NULL) return DJS_UNDEFINED;
    djs_value_t timers = djs_object_get(rt, console, "_timers");
    if (!djs_is_object(timers)) return DJS_UNDEFINED;
    const char *label = console_label(args, arg_count);
    djs_value_t start = djs_object_get(rt, timers.as.object, label);
    if (!djs_is_number(start)) return DJS_UNDEFINED;
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    sb_append_cstr(&builder, label);
    sb_append_cstr(&builder, ": ");
    sb_append_cstr(&builder,
                   djs_value_cstr(rt, djs_number(djs_host_time_ms() - start.as.number)));
    sb_append_cstr(&builder, "ms");
    for (size_t i = 1; i < arg_count; i++) {
        sb_append_char(&builder, ' ');
        console_append_value(rt, &builder, args[i]);
    }
    console_line(rt, &builder);
    return DJS_UNDEFINED;
}

static djs_value_t console_count_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_object_t *console = console_object(rt);
    if (console == NULL) return DJS_UNDEFINED;
    djs_value_t counts = djs_object_get(rt, console, "_counts");
    if (!djs_is_object(counts)) return DJS_UNDEFINED;
    const char *label = console_label(args, arg_count);
    djs_value_t current = djs_object_get(rt, counts.as.object, label);
    double value = djs_is_number(current) ? current.as.number + 1.0 : 1.0;
    djs_object_set(rt, counts.as.object, label, djs_number(value));
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    sb_append_cstr(&builder, label);
    sb_append_cstr(&builder, ": ");
    sb_append_cstr(&builder, djs_value_cstr(rt, djs_number(value)));
    console_line(rt, &builder);
    return DJS_UNDEFINED;
}

static djs_value_t console_count_reset_fn(djs_runtime_t *rt, djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_object_t *console = console_object(rt);
    if (console == NULL) return DJS_UNDEFINED;
    djs_value_t counts = djs_object_get(rt, console, "_counts");
    if (!djs_is_object(counts)) return DJS_UNDEFINED;
    djs_property_delete(rt, counts.as.object, console_label(args, arg_count));
    return DJS_UNDEFINED;
}

/* Define a method that carries its own closure. */
static void define_closure_method(djs_runtime_t *rt, djs_object_t *object,
                                  const char *name, djs_native_fn fn, int arity,
                                  djs_env_t *closure) {
    djs_object_t *function = function_with_closure(rt, fn, name, arity, closure);
    if (function == NULL) return;
    djs_property_set(rt, object, name, djs_object_value(function), 0);
    djs_property_t *property = djs_property_get(object, name);
    if (property != NULL) {
        property->flags = DJS_PROP_WRITABLE | DJS_PROP_CONFIGURABLE;
    }
}

static void install_console(djs_runtime_t *rt) {
    djs_object_t *console = djs_object_new(rt, rt->object_prototype);
    if (console == NULL) return;
    djs_env_t *closure = make_closure(rt);
    closure_set(rt, closure, "console", djs_object_value(console));
    define_raw(rt, console, "_timers",
               djs_object_value(djs_object_new(rt, rt->object_prototype)),
               DJS_PROP_PRIVATE);
    define_raw(rt, console, "_counts",
               djs_object_value(djs_object_new(rt, rt->object_prototype)),
               DJS_PROP_PRIVATE);
    define_closure_method(rt, console, "log", console_write_fn, 0, closure);
    define_closure_method(rt, console, "info", console_write_fn, 0, closure);
    define_closure_method(rt, console, "debug", console_write_fn, 0, closure);
    define_closure_method(rt, console, "warn", console_write_fn, 0, closure);
    define_closure_method(rt, console, "error", console_write_fn, 0, closure);
    define_closure_method(rt, console, "group", console_write_fn, 0, closure);
    define_closure_method(rt, console, "groupCollapsed", console_write_fn, 0, closure);
    define_closure_method(rt, console, "groupEnd", console_group_end_fn, 0, closure);
    define_closure_method(rt, console, "dir", console_dir_fn, 1, closure);
    define_closure_method(rt, console, "trace", console_trace_fn, 0, closure);
    define_closure_method(rt, console, "assert", console_assert_fn, 2, closure);
    define_closure_method(rt, console, "time", console_time_fn, 0, closure);
    define_closure_method(rt, console, "timeEnd", console_time_end_fn, 0, closure);
    define_closure_method(rt, console, "timeLog", console_time_log_fn, 0, closure);
    define_closure_method(rt, console, "count", console_count_fn, 0, closure);
    define_closure_method(rt, console, "countReset", console_count_reset_fn, 0, closure);
    define_builtin_value(rt, rt->global, "console", djs_object_value(console));
}

/* ------------------------------------------------------------------- timers */

/* Copy values into arena memory: the caller's frame is gone by the time a
 * timer fires. */
static djs_value_t *copy_values(djs_runtime_t *rt, djs_value_t *values,
                                 size_t count) {
    if (count == 0) return NULL;
    djs_value_t *copy = (djs_value_t *)djs_arena_alloc(rt, count * sizeof(*copy));
    if (copy == NULL) return NULL;
    djs_memcpy(copy, values, count * sizeof(*copy));
    return copy;
}

static djs_value_t set_timeout_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)this_value;
    if (arg_count == 0) djs_throw_type_error(rt, "callback is required");
    if (!djs_value_is_callable(args[0])) {
        djs_throw_type_error(rt, "callback must be a function");
    }
    double delay = arg_count > 1 ? djs_to_number(rt, args[1]).as.number : 0.0;
    if (!(delay > 0.0)) delay = 0.0;
    size_t extra = arg_count > 2 ? arg_count - 2 : 0;
    uint64_t id = djs_timer_add(rt, args[0], delay, false,
                                copy_values(rt, args + 2, extra), extra);
    return djs_number((double)id);
}

static djs_value_t set_interval_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)this_value;
    if (arg_count == 0) djs_throw_type_error(rt, "callback is required");
    if (!djs_value_is_callable(args[0])) {
        djs_throw_type_error(rt, "callback must be a function");
    }
    double delay = arg_count > 1 ? djs_to_number(rt, args[1]).as.number : 1.0;
    if (!(delay >= 1.0)) delay = 1.0;
    size_t extra = arg_count > 2 ? arg_count - 2 : 0;
    uint64_t id = djs_timer_add(rt, args[0], delay, true,
                                copy_values(rt, args + 2, extra), extra);
    return djs_number((double)id);
}

static djs_value_t set_immediate_fn(djs_runtime_t *rt, djs_value_t this_value,
                                    djs_value_t *args, size_t arg_count) {
    (void)this_value;
    if (arg_count == 0) djs_throw_type_error(rt, "callback is required");
    if (!djs_value_is_callable(args[0])) {
        djs_throw_type_error(rt, "callback must be a function");
    }
    size_t extra = arg_count > 1 ? arg_count - 1 : 0;
    uint64_t id = djs_timer_add(rt, args[0], 0.0, false,
                                copy_values(rt, args + 1, extra), extra);
    return djs_number((double)id);
}

static djs_value_t clear_timer_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)this_value;
    if (arg_count == 0) return DJS_UNDEFINED;
    double id = djs_to_number(rt, args[0]).as.number;
    if (id < 0.0) return DJS_UNDEFINED;
    djs_timer_clear(rt, (uint64_t)id);
    return DJS_UNDEFINED;
}

/* -------------------------------------------------------------- constructors */

/* The name of a constructor, as a value on the constructor object. */
static void set_constructor_name(djs_runtime_t *rt, djs_object_t *constructor,
                                 const char *name) {
    if (constructor == NULL || constructor->function == NULL) return;
    define_raw(rt, constructor, "name", make_cstring(rt, name), DJS_PROP_CONFIGURABLE);
}

/* Give a constructor that runtime_setup() made its native implementation. */
static void finish_runtime_constructor(djs_runtime_t *rt, djs_object_t *constructor,
                                       const char *name, djs_native_fn native) {
    if (constructor == NULL || constructor->function == NULL) return;
    constructor->function->native = native;
    set_constructor_name(rt, constructor, name);
}

/* Create a constructor runtime_setup() did not make, and wire it to its
 * prototype. */
static djs_object_t *make_builtin_constructor(djs_runtime_t *rt, djs_native_fn native,
                                              const char *name, int arity,
                                              djs_object_t *prototype) {
    djs_object_t *constructor = djs_function_new(rt, native, name, arity);
    if (constructor == NULL) return NULL;
    set_constructor_name(rt, constructor, name);
    if (prototype != NULL) {
        define_raw(rt, constructor, "prototype", djs_object_value(prototype),
                   DJS_PROP_WRITABLE | DJS_PROP_CONFIGURABLE);
        define_raw(rt, prototype, "constructor", djs_object_value(constructor),
                   DJS_PROP_WRITABLE | DJS_PROP_CONFIGURABLE);
    }
    define_builtin_value(rt, rt->global, name, djs_object_value(constructor));
    return constructor;
}

/* The error constructors share Error.prototype; the constructor link on that
 * prototype belongs to runtime_setup(). */
static djs_object_t *make_shared_error_constructor(djs_runtime_t *rt,
                                                   djs_native_fn native,
                                                   const char *name, int arity) {
    djs_object_t *constructor = djs_function_new(rt, native, name, arity);
    if (constructor == NULL) return NULL;
    set_constructor_name(rt, constructor, name);
    define_raw(rt, constructor, "prototype", djs_object_value(rt->error_prototype),
               DJS_PROP_WRITABLE | DJS_PROP_CONFIGURABLE);
    define_builtin_value(rt, rt->global, name, djs_object_value(constructor));
    return constructor;
}

/* --------------------------------------------------------------------- Symbol */

static void install_symbol(djs_runtime_t *rt) {
    djs_object_t *symbol = djs_function_new(rt, symbol_constructor_fn, "Symbol", 1);
    if (symbol == NULL) return;
    set_constructor_name(rt, symbol, "Symbol");
    /* ponytail: Symbol.iterator is the interned "@@iterator" marker string;
     * a real DJS_TAG_SYMBOL value type is needed for unique symbols. */
    define_builtin_value(rt, symbol, "iterator", make_cstring(rt, DJS_ITERATOR_KEY));
    define_builtin_value(rt, symbol, "asyncIterator",
                          make_cstring(rt, "@@asyncIterator"));
    define_builtin_value(rt, symbol, "toStringTag", make_cstring(rt, "Symbol"));
    define_builtin_value(rt, rt->global, "Symbol", djs_object_value(symbol));
}

/* ---------------------------------------------------------------------- Math */

static void install_math(djs_runtime_t *rt) {
    djs_object_t *math = djs_object_new(rt, rt->object_prototype);
    if (math == NULL) return;
    define_builtin_value(rt, math, "PI", djs_number(DJS_PI));
    define_builtin_value(rt, math, "E", djs_number(DJS_E));
    define_builtin_value(rt, math, "LN2", djs_number(DJS_LN2));
    define_builtin_value(rt, math, "LN10", djs_number(DJS_LN10));
    define_builtin_value(rt, math, "LOG2E", djs_number(1.0 / DJS_LN2));
    define_builtin_value(rt, math, "LOG10E", djs_number(1.0 / DJS_LN10));
    define_builtin_value(rt, math, "SQRT2", djs_number(num_sqrt(2.0)));
    define_builtin_value(rt, math, "SQRT1_2", djs_number(num_sqrt(0.5)));
    define_builtin_method(rt, math, "abs", math_abs_fn, 1);
    define_builtin_method(rt, math, "floor", math_floor_fn, 1);
    define_builtin_method(rt, math, "ceil", math_ceil_fn, 1);
    define_builtin_method(rt, math, "round", math_round_fn, 1);
    define_builtin_method(rt, math, "trunc", math_trunc_fn, 1);
    define_builtin_method(rt, math, "sign", math_sign_fn, 1);
    define_builtin_method(rt, math, "sqrt", math_sqrt_fn, 1);
    define_builtin_method(rt, math, "cbrt", math_cbrt_fn, 1);
    define_builtin_method(rt, math, "exp", math_exp_fn, 1);
    define_builtin_method(rt, math, "log", math_log_fn, 1);
    define_builtin_method(rt, math, "log2", math_log2_fn, 1);
    define_builtin_method(rt, math, "log10", math_log10_fn, 1);
    define_builtin_method(rt, math, "pow", math_pow_fn, 2);
    define_builtin_method(rt, math, "min", math_min_fn, 2);
    define_builtin_method(rt, math, "max", math_max_fn, 2);
    define_builtin_method(rt, math, "random", math_random_fn, 0);
    define_builtin_method(rt, math, "sin", math_sin_fn, 1);
    define_builtin_method(rt, math, "cos", math_cos_fn, 1);
    define_builtin_method(rt, math, "tan", math_tan_fn, 1);
    define_builtin_method(rt, math, "asin", math_asin_fn, 1);
    define_builtin_method(rt, math, "acos", math_acos_fn, 1);
    define_builtin_method(rt, math, "atan", math_atan_fn, 1);
    define_builtin_method(rt, math, "atan2", math_atan2_fn, 2);
    define_builtin_method(rt, math, "hypot", math_hypot_fn, 2);
    define_builtin_method(rt, math, "sinh", math_sinh_fn, 1);
    define_builtin_method(rt, math, "cosh", math_cosh_fn, 1);
    define_builtin_method(rt, math, "tanh", math_tanh_fn, 1);
    define_builtin_method(rt, math, "fround", math_fround_fn, 1);
    define_builtin_method(rt, math, "imul", math_imul_fn, 2);
    define_builtin_value(rt, rt->global, "Math", djs_object_value(math));
}

/* ---------------------------------------------------------------------- JSON */

/* JSON is a namespace, not a function: calling it is a type error. */
static djs_value_t json_not_callable_fn(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    djs_throw_type_error(rt, "JSON is not a function");
    return DJS_UNDEFINED;
}

static void install_json(djs_runtime_t *rt) {
    djs_object_t *json = djs_function_new(rt, json_not_callable_fn, "JSON", 2);
    if (json == NULL) return;
    set_constructor_name(rt, json, "JSON");
    define_builtin_method(rt, json, "parse", json_parse_fn, 2);
    define_builtin_method(rt, json, "stringify", json_stringify_fn, 3);
    define_builtin_value(rt, rt->global, "JSON", djs_object_value(json));
}

/* -------------------------------------------------------------------- String */

static djs_value_t string_from_code_point_fn(djs_runtime_t *rt, djs_value_t this_value,
                                             djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_string_builder_t builder;
    sb_init(&builder, rt);
    for (size_t i = 0; i < arg_count; i++) {
        double number = djs_to_number(rt, args[i]).as.number;
        if (number != number || number < 0.0 || number > 0x10FFFF ||
            number != (double)(uint32_t)number) {
            djs_throw_type_error(rt, "invalid code point");
        }
        char encoded[4];
        size_t length = utf8_encode((uint32_t)number, encoded);
        sb_append_bytes(&builder, encoded, length);
    }
    return make_cstring(rt, sb_finish(&builder));
}

/* --------------------------------------------------------------- Map and Set */

static djs_value_t map_group_by_fn(djs_runtime_t *rt, djs_value_t this_value,
                                   djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t callback = arg_at(args, arg_count, 1);
    if (!djs_value_is_callable(callback)) {
        djs_throw_type_error(rt, "callback is not a function");
    }
    djs_value_t items = iterable_to_array(rt, arg_at(args, arg_count, 0));
    djs_object_t *map = djs_object_new(rt, rt->map_prototype);
    if (map == NULL) return DJS_UNDEFINED;
    map->class = DJS_CLASS_MAP;
    define_collection_size(rt, map);
    size_t length = djs_array_length(items.as.object);
    for (size_t i = 0; i < length; i++) {
        djs_value_t item = djs_array_get(rt, items.as.object, i);
        djs_value_t key = djs_to_primitive(rt, djs_call(rt, callback, DJS_UNDEFINED,
                                                       &item, 1), 0);
        djs_map_entry_t *entry = map_find_entry(map, key);
        if (entry == NULL) {
            map_set_entry(rt, map, key, djs_make_array(rt, NULL, 0));
            entry = map_find_entry(map, key);
        }
        if (entry != NULL && djs_is_object(entry->value)) {
            djs_array_push(rt, entry->value.as.object, item);
        }
    }
    return djs_object_value(map);
}

/* A new Set, ready to receive values. */
static djs_object_t *set_new(djs_runtime_t *rt) {
    djs_object_t *set = djs_object_new(rt, rt->set_prototype);
    if (set == NULL) return NULL;
    set->class = DJS_CLASS_SET;
    define_collection_size(rt, set);
    return set;
}

/* Does an array hold this value? */
static bool array_has_value(djs_runtime_t *rt, djs_value_t array, djs_value_t value) {
    size_t length = djs_array_length(array.as.object);
    for (size_t i = 0; i < length; i++) {
        if (same_value_zero(djs_array_get(rt, array.as.object, i), value)) return true;
    }
    return false;
}

static djs_value_t set_union_fn(djs_runtime_t *rt, djs_value_t this_value,
                                djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t left = iterable_to_array(rt, arg_at(args, arg_count, 0));
    djs_value_t right = iterable_to_array(rt, arg_at(args, arg_count, 1));
    djs_object_t *set = set_new(rt);
    if (set == NULL) return DJS_UNDEFINED;
    size_t left_length = djs_array_length(left.as.object);
    size_t right_length = djs_array_length(right.as.object);
    for (size_t i = 0; i < left_length; i++) {
        djs_value_t value = djs_array_get(rt, left.as.object, i);
        map_set_entry(rt, set, value, value);
    }
    for (size_t i = 0; i < right_length; i++) {
        djs_value_t value = djs_array_get(rt, right.as.object, i);
        map_set_entry(rt, set, value, value);
    }
    return djs_object_value(set);
}

static djs_value_t set_intersection_fn(djs_runtime_t *rt, djs_value_t this_value,
                                       djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t left = iterable_to_array(rt, arg_at(args, arg_count, 0));
    djs_value_t right = iterable_to_array(rt, arg_at(args, arg_count, 1));
    djs_object_t *set = set_new(rt);
    if (set == NULL) return DJS_UNDEFINED;
    size_t left_length = djs_array_length(left.as.object);
    for (size_t i = 0; i < left_length; i++) {
        djs_value_t value = djs_array_get(rt, left.as.object, i);
        if (!array_has_value(rt, right, value)) continue;
        map_set_entry(rt, set, value, value);
    }
    return djs_object_value(set);
}

static djs_value_t set_difference_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t left = iterable_to_array(rt, arg_at(args, arg_count, 0));
    djs_value_t right = iterable_to_array(rt, arg_at(args, arg_count, 1));
    djs_object_t *set = set_new(rt);
    if (set == NULL) return DJS_UNDEFINED;
    size_t left_length = djs_array_length(left.as.object);
    for (size_t i = 0; i < left_length; i++) {
        djs_value_t value = djs_array_get(rt, left.as.object, i);
        if (array_has_value(rt, right, value)) continue;
        map_set_entry(rt, set, value, value);
    }
    return djs_object_value(set);
}

static djs_value_t set_symmetric_difference_fn(djs_runtime_t *rt, djs_value_t this_value,
                                               djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t left = iterable_to_array(rt, arg_at(args, arg_count, 0));
    djs_value_t right = iterable_to_array(rt, arg_at(args, arg_count, 1));
    djs_object_t *set = set_new(rt);
    if (set == NULL) return DJS_UNDEFINED;
    size_t left_length = djs_array_length(left.as.object);
    size_t right_length = djs_array_length(right.as.object);
    for (size_t i = 0; i < left_length; i++) {
        djs_value_t value = djs_array_get(rt, left.as.object, i);
        if (array_has_value(rt, right, value)) continue;
        map_set_entry(rt, set, value, value);
    }
    for (size_t i = 0; i < right_length; i++) {
        djs_value_t value = djs_array_get(rt, right.as.object, i);
        if (array_has_value(rt, left, value)) continue;
        map_set_entry(rt, set, value, value);
    }
    return djs_object_value(set);
}

/* ------------------------------------------------------------------ Promises */

static djs_value_t promise_all_settled_on_ok(djs_runtime_t *rt, djs_value_t this_value,
                                             djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t state = closure_get(rt, "state");
    djs_value_t index = closure_get(rt, "index");
    djs_value_t results = promise_state_value(rt, state, "results");
    djs_value_t entry = djs_make_object(rt);
    if (entry.tag == DJS_TAG_OBJECT) {
        djs_object_set(rt, entry.as.object, "status", make_cstring(rt, "fulfilled"));
        djs_object_set(rt, entry.as.object, "value", arg_at(args, arg_count, 0));
    }
    if (results.tag == DJS_TAG_OBJECT && index.tag == DJS_TAG_NUMBER) {
        djs_array_set(rt, results.as.object, (size_t)index.as.number, entry);
    }
    double remaining = promise_state_number(rt, state, "remaining") - 1.0;
    if (state.tag == DJS_TAG_OBJECT) {
        djs_object_set(rt, state.as.object, "remaining", djs_number(remaining));
    }
    if (remaining <= 0.0) {
        djs_promise_resolve(rt, promise_state_value(rt, state, "promise"), results);
    }
    return DJS_UNDEFINED;
}

static djs_value_t promise_all_settled_on_err(djs_runtime_t *rt, djs_value_t this_value,
                                               djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t state = closure_get(rt, "state");
    djs_value_t index = closure_get(rt, "index");
    djs_value_t results = promise_state_value(rt, state, "results");
    djs_value_t entry = djs_make_object(rt);
    if (entry.tag == DJS_TAG_OBJECT) {
        djs_object_set(rt, entry.as.object, "status", make_cstring(rt, "rejected"));
        djs_object_set(rt, entry.as.object, "reason", arg_at(args, arg_count, 0));
    }
    if (results.tag == DJS_TAG_OBJECT && index.tag == DJS_TAG_NUMBER) {
        djs_array_set(rt, results.as.object, (size_t)index.as.number, entry);
    }
    double remaining = promise_state_number(rt, state, "remaining") - 1.0;
    if (state.tag == DJS_TAG_OBJECT) {
        djs_object_set(rt, state.as.object, "remaining", djs_number(remaining));
    }
    if (remaining <= 0.0) {
        djs_promise_resolve(rt, promise_state_value(rt, state, "promise"), results);
    }
    return DJS_UNDEFINED;
}

static djs_value_t promise_all_settled_fn(djs_runtime_t *rt, djs_value_t this_value,
                                           djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t state = promise_combine_setup(rt, args, arg_count);
    promise_combine_attach(rt, state, promise_all_settled_on_ok,
                           promise_all_settled_on_err);
    promise_resolve_empty(rt, state);
    return promise_state_value(rt, state, "promise");
}

static djs_value_t promise_any_on_ok(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t state = closure_get(rt, "state");
    if (promise_state_number(rt, state, "settled") != 0.0) return DJS_UNDEFINED;
    if (state.tag == DJS_TAG_OBJECT) {
        djs_object_set(rt, state.as.object, "settled", djs_number(1.0));
    }
    djs_promise_resolve(rt, promise_state_value(rt, state, "promise"),
                        arg_at(args, arg_count, 0));
    return DJS_UNDEFINED;
}

static djs_value_t promise_any_on_err(djs_runtime_t *rt, djs_value_t this_value,
                                      djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t state = closure_get(rt, "state");
    djs_value_t index = closure_get(rt, "index");
    djs_value_t reasons = promise_state_value(rt, state, "results");
    if (reasons.tag == DJS_TAG_OBJECT && index.tag == DJS_TAG_NUMBER) {
        djs_array_set(rt, reasons.as.object, (size_t)index.as.number,
                      arg_at(args, arg_count, 0));
    }
    double remaining = promise_state_number(rt, state, "remaining") - 1.0;
    if (state.tag == DJS_TAG_OBJECT) {
        djs_object_set(rt, state.as.object, "remaining", djs_number(remaining));
    }
    if (remaining <= 0.0) {
        djs_string_builder_t builder;
        sb_init(&builder, rt);
        sb_append_cstr(&builder, "All promises were rejected");
        djs_promise_reject(rt, promise_state_value(rt, state, "promise"),
                           djs_make_error(rt, "Error", sb_finish(&builder)));
    }
    return DJS_UNDEFINED;
}

static djs_value_t promise_any_fn(djs_runtime_t *rt, djs_value_t this_value,
                                  djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_value_t state = promise_combine_setup(rt, args, arg_count);
    promise_combine_attach(rt, state, promise_any_on_ok, promise_any_on_err);
    djs_value_t count = promise_state_value(rt, state, "count");
    if (count.tag == DJS_TAG_NUMBER && count.as.number == 0.0) {
        djs_promise_reject(rt, promise_state_value(rt, state, "promise"),
                           djs_make_error(rt, "Error", "All promises were rejected"));
    }
    return promise_state_value(rt, state, "promise");
}

/* --------------------------------------------------------------- entry points */

void djs_install_globals(djs_runtime_t *rt) {
    if (rt == NULL || rt->global == NULL) return;

    /* runtime_setup() owns the prototype chain and the constructor objects;
     * only the values that live on the global object itself are installed
     * here. */
    define_raw(rt, rt->global, "undefined", DJS_UNDEFINED, 0u);
    define_raw(rt, rt->global, "NaN", djs_number(0.0 / 0.0), 0u);
    define_raw(rt, rt->global, "Infinity", djs_number(1.0 / 0.0), 0u);
    define_builtin_value(rt, rt->global, "globalThis", djs_object_value(rt->global));

    define_builtin_method(rt, rt->global, "parseInt", number_parse_int_fn, 2);
    define_builtin_method(rt, rt->global, "parseFloat", number_parse_float_fn, 1);
    define_builtin_method(rt, rt->global, "isNaN", global_is_nan_fn, 1);
    define_builtin_method(rt, rt->global, "isFinite", global_is_finite_fn, 1);
    define_builtin_method(rt, rt->global, "encodeURIComponent",
                          encode_uri_component_fn, 1);
    define_builtin_method(rt, rt->global, "encodeURI", encode_uri_fn, 1);
    define_builtin_method(rt, rt->global, "decodeURIComponent",
                          decode_uri_component_fn, 1);
    define_builtin_method(rt, rt->global, "decodeURI", decode_uri_fn, 1);
    define_builtin_method(rt, rt->global, "queueMicrotask", queue_microtask_fn, 1);
    define_builtin_method(rt, rt->global, "setTimeout", set_timeout_fn, 2);
    define_builtin_method(rt, rt->global, "clearTimeout", clear_timer_fn, 1);
    define_builtin_method(rt, rt->global, "setInterval", set_interval_fn, 2);
    define_builtin_method(rt, rt->global, "clearInterval", clear_timer_fn, 1);
    define_builtin_method(rt, rt->global, "setImmediate", set_immediate_fn, 1);
    define_builtin_method(rt, rt->global, "clearImmediate", clear_timer_fn, 1);

    install_console(rt);
    install_symbol(rt);
}

void djs_install_builtins(djs_runtime_t *rt) {
    if (rt == NULL || rt->global == NULL) return;

    /* Object */
    if (rt->object_constructor != NULL) {
        finish_runtime_constructor(rt, rt->object_constructor, "Object",
                                   object_constructor_fn);
        define_builtin_value(rt, rt->global, "Object",
                              djs_object_value(rt->object_constructor));
        define_builtin_method(rt, rt->object_constructor, "keys", object_keys_fn, 1);
        define_builtin_method(rt, rt->object_constructor, "values", object_values_fn, 1);
        define_builtin_method(rt, rt->object_constructor, "entries", object_entries_fn, 1);
        define_builtin_method(rt, rt->object_constructor, "assign", object_assign_fn, 2);
        define_builtin_method(rt, rt->object_constructor, "freeze", object_freeze_fn, 1);
        define_builtin_method(rt, rt->object_constructor, "isFrozen",
                              object_is_frozen_fn, 1);
        define_builtin_method(rt, rt->object_constructor, "create", object_create_fn, 2);
        define_builtin_method(rt, rt->object_constructor, "fromEntries",
                              object_from_entries_fn, 1);
        define_builtin_method(rt, rt->object_constructor, "getPrototypeOf",
                              object_get_prototype_fn, 1);
        define_builtin_method(rt, rt->object_constructor, "setPrototypeOf",
                              object_set_prototype_fn, 2);
        define_builtin_method(rt, rt->object_constructor, "getOwnPropertyNames",
                              object_get_own_names_fn, 1);
        define_builtin_method(rt, rt->object_constructor, "defineProperty",
                              object_define_property_fn, 3);
        define_builtin_method(rt, rt->object_constructor, "getOwnPropertyDescriptor",
                              object_get_descriptor_fn, 2);
    }
    define_builtin_method(rt, rt->object_prototype, "hasOwnProperty",
                          object_has_own_property_fn, 1);
    define_builtin_method(rt, rt->object_prototype, "toString", object_to_string_fn, 0);
    define_builtin_method(rt, rt->object_prototype, "valueOf", object_value_of_fn, 0);
    define_builtin_method(rt, rt->object_prototype, "isPrototypeOf",
                          object_is_prototype_of_fn, 1);
    define_builtin_method(rt, rt->object_prototype, "propertyIsEnumerable",
                          object_property_is_enumerable_fn, 1);

    /* Array */
    if (rt->array_constructor != NULL) {
        finish_runtime_constructor(rt, rt->array_constructor, "Array",
                                   array_constructor_fn);
        define_builtin_value(rt, rt->global, "Array",
                              djs_object_value(rt->array_constructor));
        define_builtin_method(rt, rt->array_constructor, "isArray", array_is_array_fn, 1);
        define_builtin_method(rt, rt->array_constructor, "from", array_from_fn, 1);
        define_builtin_method(rt, rt->array_constructor, "of", array_of_fn, 0);
    }
    define_builtin_method(rt, rt->array_prototype, "push", array_push_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "pop", array_pop_fn, 0);
    define_builtin_method(rt, rt->array_prototype, "shift", array_shift_fn, 0);
    define_builtin_method(rt, rt->array_prototype, "unshift", array_unshift_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "slice", array_slice_fn, 2);
    define_builtin_method(rt, rt->array_prototype, "splice", array_splice_fn, 2);
    define_builtin_method(rt, rt->array_prototype, "concat", array_concat_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "join", array_join_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "indexOf", array_index_of_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "lastIndexOf", array_last_index_of_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "includes", array_includes_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "find", array_find_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "findIndex", array_find_index_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "filter", array_filter_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "map", array_map_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "forEach", array_for_each_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "reduce", array_reduce_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "reduceRight", array_reduce_right_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "some", array_some_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "every", array_every_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "sort", array_sort_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "reverse", array_reverse_fn, 0);
    define_builtin_method(rt, rt->array_prototype, "fill", array_fill_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "flat", array_flat_fn, 0);
    define_builtin_method(rt, rt->array_prototype, "flatMap", array_flat_map_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "at", array_at_fn, 1);
    define_builtin_method(rt, rt->array_prototype, "keys", array_keys_fn, 0);
    define_builtin_method(rt, rt->array_prototype, "values", array_values_fn, 0);
    define_builtin_method(rt, rt->array_prototype, "entries", array_entries_fn, 0);
    define_builtin_method(rt, rt->array_prototype, DJS_ITERATOR_KEY, array_iterator_fn, 0);

    /* String */
    djs_object_t *string_constructor = make_builtin_constructor(
        rt, string_constructor_fn, "String", 1, rt->string_prototype);
    if (string_constructor != NULL) {
        define_builtin_method(rt, string_constructor, "fromCharCode",
                              string_from_char_code_fn, 1);
        define_builtin_method(rt, string_constructor, "fromCodePoint",
                              string_from_code_point_fn, 1);
    }
    define_builtin_method(rt, rt->string_prototype, "charAt", string_char_at_fn, 1);
    define_builtin_method(rt, rt->string_prototype, "charCodeAt", string_char_code_at_fn, 1);
    define_builtin_method(rt, rt->string_prototype, "codePointAt", string_code_point_at_fn, 1);
    define_builtin_method(rt, rt->string_prototype, "indexOf", string_index_of_fn, 1);
    define_builtin_method(rt, rt->string_prototype, "lastIndexOf", string_last_index_of_fn, 1);
    define_builtin_method(rt, rt->string_prototype, "includes", string_includes_fn, 1);
    define_builtin_method(rt, rt->string_prototype, "startsWith", string_starts_with_fn, 1);
    define_builtin_method(rt, rt->string_prototype, "endsWith", string_ends_with_fn, 1);
    define_builtin_method(rt, rt->string_prototype, "slice", string_slice_fn, 2);
    define_builtin_method(rt, rt->string_prototype, "substring", string_substring_fn, 2);
    define_builtin_method(rt, rt->string_prototype, "substr", string_substr_fn, 2);
    define_builtin_method(rt, rt->string_prototype, "toUpperCase", string_to_upper_case_fn, 0);
    define_builtin_method(rt, rt->string_prototype, "toLowerCase", string_to_lower_case_fn, 0);
    define_builtin_method(rt, rt->string_prototype, "trim", string_trim_default_fn, 0);
    define_builtin_method(rt, rt->string_prototype, "trimStart", string_trim_start_fn, 0);
    define_builtin_method(rt, rt->string_prototype, "trimEnd", string_trim_end_fn, 0);
    define_builtin_method(rt, rt->string_prototype, "split", string_split_fn, 2);
    define_builtin_method(rt, rt->string_prototype, "repeat", string_repeat_fn, 1);
    define_builtin_method(rt, rt->string_prototype, "padStart", string_pad_start_fn, 1);
    define_builtin_method(rt, rt->string_prototype, "padEnd", string_pad_end_fn, 1);
    define_builtin_method(rt, rt->string_prototype, "concat", string_concat_fn, 1);
    define_builtin_method(rt, rt->string_prototype, "replace", string_replace_fn, 2);
    define_builtin_method(rt, rt->string_prototype, "replaceAll", string_replace_all_fn, 2);
    define_builtin_method(rt, rt->string_prototype, "search", string_search_fn, 1);
    define_builtin_method(rt, rt->string_prototype, "at", string_at_fn, 1);
    define_builtin_method(rt, rt->string_prototype, "toString", string_to_string_fn, 0);
    define_builtin_method(rt, rt->string_prototype, "valueOf", string_to_string_fn, 0);
    define_builtin_method(rt, rt->string_prototype, DJS_ITERATOR_KEY, string_iterator_fn, 0);

    /* Number */
    djs_object_t *number_constructor = make_builtin_constructor(
        rt, number_constructor_fn, "Number", 1, rt->number_prototype);
    if (number_constructor != NULL) {
        define_builtin_method(rt, number_constructor, "parseInt", number_parse_int_fn, 2);
        define_builtin_method(rt, number_constructor, "parseFloat", number_parse_float_fn, 1);
        define_builtin_method(rt, number_constructor, "isInteger", number_is_integer_fn, 1);
        define_builtin_method(rt, number_constructor, "isFinite", number_is_finite_fn, 1);
        define_builtin_method(rt, number_constructor, "isNaN", number_is_nan_fn, 1);
        define_builtin_method(rt, number_constructor, "isSafeInteger",
                              number_is_safe_integer_fn, 1);
        define_builtin_value(rt, number_constructor, "MAX_SAFE_INTEGER",
                              djs_number(9007199254740991.0));
        define_builtin_value(rt, number_constructor, "MIN_SAFE_INTEGER",
                              djs_number(-9007199254740991.0));
        define_builtin_value(rt, number_constructor, "MAX_VALUE",
                              djs_number(1.7976931348623157e308));
        define_builtin_value(rt, number_constructor, "MIN_VALUE", djs_number(5e-324));
        define_builtin_value(rt, number_constructor, "EPSILON",
                              djs_number(2.220446049250313e-16));
        define_builtin_value(rt, number_constructor, "POSITIVE_INFINITY",
                              djs_number(1.0 / 0.0));
        define_builtin_value(rt, number_constructor, "NEGATIVE_INFINITY",
                              djs_number(-1.0 / 0.0));
        define_builtin_value(rt, number_constructor, "NaN", djs_number(0.0 / 0.0));
    }
    define_builtin_method(rt, rt->number_prototype, "toString", number_to_string_fn, 1);
    define_builtin_method(rt, rt->number_prototype, "toFixed", number_to_fixed_fn, 1);
    define_builtin_method(rt, rt->number_prototype, "toPrecision", number_to_precision_fn, 1);
    define_builtin_method(rt, rt->number_prototype, "toExponential", number_to_exponential_fn, 1);
    define_builtin_method(rt, rt->number_prototype, "valueOf", number_value_of_fn, 0);

    /* Boolean */
    make_builtin_constructor(rt, boolean_constructor_fn, "Boolean", 1,
                             rt->boolean_prototype);
    define_builtin_method(rt, rt->boolean_prototype, "toString", boolean_to_string_fn, 0);
    define_builtin_method(rt, rt->boolean_prototype, "valueOf", boolean_value_of_fn, 0);

    /* Function */
    if (rt->function_constructor != NULL) {
        finish_runtime_constructor(rt, rt->function_constructor, "Function",
                                   function_constructor_fn);
        define_builtin_value(rt, rt->global, "Function",
                              djs_object_value(rt->function_constructor));
    }
    define_builtin_method(rt, rt->function_prototype, "call", function_call_fn, 1);
    define_builtin_method(rt, rt->function_prototype, "apply", function_apply_fn, 2);
    define_builtin_method(rt, rt->function_prototype, "bind", function_bind_fn, 1);
    define_builtin_method(rt, rt->function_prototype, "toString", function_to_string_fn, 0);

    /* Math */
    install_math(rt);

    /* JSON */
    install_json(rt);

    /* Errors */
    define_builtin_value(rt, rt->error_prototype, "name", make_cstring(rt, "Error"));
    define_builtin_value(rt, rt->error_prototype, "message", make_cstring(rt, ""));
    define_builtin_method(rt, rt->error_prototype, "toString", error_to_string_fn, 0);
    finish_runtime_constructor(rt, rt->error_constructor, "Error", error_constructor_fn);
    finish_runtime_constructor(rt, rt->type_error_constructor, "TypeError",
                               type_error_constructor_fn);
    finish_runtime_constructor(rt, rt->range_error_constructor, "RangeError",
                               range_error_constructor_fn);
    finish_runtime_constructor(rt, rt->syntax_error_constructor, "SyntaxError",
                               syntax_error_constructor_fn);
    finish_runtime_constructor(rt, rt->reference_error_constructor, "ReferenceError",
                               reference_error_constructor_fn);
    if (rt->error_constructor != NULL) {
        define_builtin_value(rt, rt->global, "Error",
                              djs_object_value(rt->error_constructor));
    }
    if (rt->type_error_constructor != NULL) {
        define_builtin_value(rt, rt->global, "TypeError",
                              djs_object_value(rt->type_error_constructor));
    }
    if (rt->range_error_constructor != NULL) {
        define_builtin_value(rt, rt->global, "RangeError",
                              djs_object_value(rt->range_error_constructor));
    }
    if (rt->syntax_error_constructor != NULL) {
        define_builtin_value(rt, rt->global, "SyntaxError",
                              djs_object_value(rt->syntax_error_constructor));
    }
    if (rt->reference_error_constructor != NULL) {
        define_builtin_value(rt, rt->global, "ReferenceError",
                              djs_object_value(rt->reference_error_constructor));
    }
    make_shared_error_constructor(rt, eval_error_constructor_fn, "EvalError", 1);
    make_shared_error_constructor(rt, uri_error_constructor_fn, "URIError", 1);

    /* Map */
    djs_object_t *map_constructor = make_builtin_constructor(
        rt, map_constructor_fn, "Map", 0, rt->map_prototype);
    if (map_constructor != NULL) {
        define_builtin_method(rt, map_constructor, "groupBy", map_group_by_fn, 2);
    }
    define_builtin_method(rt, rt->map_prototype, "get", map_get_fn, 1);
    define_builtin_method(rt, rt->map_prototype, "set", map_set_fn, 2);
    define_builtin_method(rt, rt->map_prototype, "has", map_has_fn, 1);
    define_builtin_method(rt, rt->map_prototype, "delete", map_delete_fn, 1);
    define_builtin_method(rt, rt->map_prototype, "clear", map_clear_fn, 0);
    define_builtin_method(rt, rt->map_prototype, "forEach", map_for_each_fn, 1);
    define_builtin_method(rt, rt->map_prototype, "keys", map_keys_fn, 0);
    define_builtin_method(rt, rt->map_prototype, "values", map_values_fn, 0);
    define_builtin_method(rt, rt->map_prototype, "entries", map_entries_fn, 0);
    define_builtin_method(rt, rt->map_prototype, DJS_ITERATOR_KEY, map_iterator_fn, 0);

    /* Set */
    djs_object_t *set_constructor = make_builtin_constructor(
        rt, set_constructor_fn, "Set", 0, rt->set_prototype);
    if (set_constructor != NULL) {
        define_builtin_method(rt, set_constructor, "union", set_union_fn, 2);
        define_builtin_method(rt, set_constructor, "intersection", set_intersection_fn, 2);
        define_builtin_method(rt, set_constructor, "difference", set_difference_fn, 2);
        define_builtin_method(rt, set_constructor, "symmetricDifference",
                              set_symmetric_difference_fn, 2);
    }
    define_builtin_method(rt, rt->set_prototype, "add", set_add_fn, 1);
    define_builtin_method(rt, rt->set_prototype, "has", map_has_fn, 1);
    define_builtin_method(rt, rt->set_prototype, "delete", map_delete_fn, 1);
    define_builtin_method(rt, rt->set_prototype, "clear", map_clear_fn, 0);
    define_builtin_method(rt, rt->set_prototype, "forEach", map_for_each_fn, 1);
    define_builtin_method(rt, rt->set_prototype, "keys", map_keys_fn, 0);
    define_builtin_method(rt, rt->set_prototype, "values", map_values_fn, 0);
    define_builtin_method(rt, rt->set_prototype, "entries", map_entries_fn, 0);
    define_builtin_method(rt, rt->set_prototype, DJS_ITERATOR_KEY, set_iterator_fn, 0);

    /* Promise */
    if (rt->promise_constructor != NULL) {
        finish_runtime_constructor(rt, rt->promise_constructor, "Promise",
                                   promise_constructor_fn);
        define_builtin_value(rt, rt->global, "Promise",
                              djs_object_value(rt->promise_constructor));
        define_builtin_method(rt, rt->promise_constructor, "resolve",
                              promise_resolve_static_fn, 1);
        define_builtin_method(rt, rt->promise_constructor, "reject",
                              promise_reject_static_fn, 1);
        define_builtin_method(rt, rt->promise_constructor, "all", promise_all_fn, 1);
        define_builtin_method(rt, rt->promise_constructor, "allSettled",
                              promise_all_settled_fn, 1);
        define_builtin_method(rt, rt->promise_constructor, "any", promise_any_fn, 1);
        define_builtin_method(rt, rt->promise_constructor, "race", promise_race_fn, 1);
    }
    define_builtin_method(rt, rt->promise_prototype, "then", promise_then_fn, 2);
    define_builtin_method(rt, rt->promise_prototype, "catch", promise_catch_fn, 1);
    define_builtin_method(rt, rt->promise_prototype, "finally", promise_finally_fn, 1);

    /* Date */
    djs_object_t *date_constructor = make_builtin_constructor(
        rt, date_constructor_fn, "Date", 7, rt->date_prototype);
    if (date_constructor != NULL) {
        define_builtin_method(rt, date_constructor, "now", date_now_fn, 0);
        define_builtin_method(rt, date_constructor, "parse", date_parse_fn, 1);
        define_builtin_method(rt, date_constructor, "UTC", date_utc_fn, 7);
    }
    define_builtin_method(rt, rt->date_prototype, "getTime", date_get_time_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "valueOf", date_get_time_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getFullYear", date_get_full_year_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getUTCFullYear", date_get_full_year_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getMonth", date_get_month_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getUTCMonth", date_get_month_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getDate", date_get_date_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getUTCDate", date_get_date_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getDay", date_get_day_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getUTCDay", date_get_day_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getHours", date_get_hours_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getUTCHours", date_get_hours_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getMinutes", date_get_minutes_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getUTCMinutes", date_get_minutes_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getSeconds", date_get_seconds_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getUTCSeconds", date_get_seconds_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getMilliseconds",
                          date_get_milliseconds_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getUTCMilliseconds",
                          date_get_milliseconds_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "getTimezoneOffset",
                          date_get_timezone_offset_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "toISOString", date_to_iso_string_fn, 0);
    define_builtin_method(rt, rt->date_prototype, "toJSON", date_to_json_fn, 1);
    define_builtin_method(rt, rt->date_prototype, "toString", date_to_string_fn, 0);
}
