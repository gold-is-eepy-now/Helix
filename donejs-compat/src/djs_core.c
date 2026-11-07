/* djs_core.c — values, strings, numbers, conversions and runtime lifetime.
 *
 * Memory: every allocation goes through the arena, which hands out blocks
 * from the host allocator (malloc on the build host, helix_alloc on Helix).
 * ponytail: there is no garbage collector, so a long-running script that
 * allocates in a loop grows the arena until the runtime is freed. Add a
 * mark-sweep collector with precise roots (the frame stack and the object
 * graph) when a runtime needs to outlive a single script.
 */

#include "djs_internal.h"

#if defined(DJS_FREESTANDING)
#include "../../userspace/libhelix.h"
#define DJS_HOST_ALLOC(size) helix_alloc(size)
#define DJS_HOST_FREE(ptr)   helix_free(ptr)
#else
#include <stdlib.h>
#define DJS_HOST_ALLOC(size) malloc(size)
#define DJS_HOST_FREE(ptr)   free(ptr)
#endif

/* ------------------------------------------------------------- utilities */

size_t djs_strlen(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') length++;
    return length;
}

void djs_memcpy(void *destination, const void *source, size_t count) {
    unsigned char *d = (unsigned char *)destination;
    const unsigned char *s = (const unsigned char *)source;
    while (count--) *d++ = *s++;
}

void djs_memset(void *destination, int value, size_t count) {
    unsigned char *d = (unsigned char *)destination;
    while (count--) *d++ = (unsigned char)value;
}

int djs_strcmp(const char *a, const char *b) {
    if (a == b) return 0;
    if (a == NULL) return -1;
    if (b == NULL) return 1;
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int djs_memcmp_helper(const void *a, const void *b, size_t count) {
    const unsigned char *left = (const unsigned char *)a;
    const unsigned char *right = (const unsigned char *)b;
    for (size_t i = 0; i < count; i++) {
        if (left[i] != right[i]) return (int)left[i] - (int)right[i];
    }
    return 0;
}

/* Write "e+21" / "e-7" and return the number of bytes written. */
int djs_format_exponent(char *out, int exponent) {
    int written = 0;
    out[written++] = 'e';
    out[written++] = exponent < 0 ? '-' : '+';
    int value = exponent < 0 ? -exponent : exponent;
    char reverse[16];
    int count = 0;
    if (value == 0) {
        reverse[count++] = '0';
    } else {
        while (value > 0 && count < 15) {
            reverse[count++] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    while (count > 0) out[written++] = reverse[--count];
    return written;
}

bool djs_str_equal(const char *a, const char *b) {
    return djs_strcmp(a, b) == 0;
}

/* ------------------------------------------------------------------ arena */

#define DJS_ARENA_BLOCK (64u * 1024u)

static void arena_add_block(djs_arena_t *arena, size_t size) {
    if (size < DJS_ARENA_BLOCK) size = DJS_ARENA_BLOCK;
    djs_arena_block_t *block = (djs_arena_block_t *)DJS_HOST_ALLOC(sizeof(*block) + size);
    if (block == NULL) return;
    block->next = NULL;
    block->used = 0;
    block->capacity = size;
    block->data = (unsigned char *)(block + 1);
    if (arena->current == NULL) {
        arena->first = block;
    } else {
        arena->current->next = block;
    }
    arena->current = block;
}

void *djs_arena_alloc(djs_runtime_t *rt, size_t size) {
    if (size == 0) size = 1;
    size = (size + 15u) & ~(size_t)15u;
    djs_arena_t *arena = &rt->arena;
    if (arena->current == NULL || arena->current->used + size > arena->current->capacity) {
        arena_add_block(arena, size);
        if (arena->current == NULL) return NULL;
    }
    void *result = arena->current->data + arena->current->used;
    arena->current->used += size;
    return result;
}

void *djs_arena_calloc(djs_runtime_t *rt, size_t count, size_t size) {
    if (count != 0 && size > (size_t)-1 / count) return NULL;
    size_t total = count * size;
    void *memory = djs_arena_alloc(rt, total);
    if (memory != NULL) djs_memset(memory, 0, total);
    return memory;
}

char *djs_arena_strdup(djs_runtime_t *rt, const char *text) {
    return djs_arena_strndup(rt, text, djs_strlen(text));
}

char *djs_arena_strndup(djs_runtime_t *rt, const char *text, size_t length) {
    char *copy = (char *)djs_arena_alloc(rt, length + 1);
    if (copy == NULL) return NULL;
    if (length > 0) djs_memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

void djs_arena_release(djs_runtime_t *rt) {
    djs_arena_block_t *block = rt->arena.first;
    while (block != NULL) {
        djs_arena_block_t *next = block->next;
        DJS_HOST_FREE(block);
        block = next;
    }
    rt->arena.first = NULL;
    rt->arena.current = NULL;
}

/* ---------------------------------------------------------------- strings */

static uint32_t hash_bytes(const char *bytes, size_t length) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        hash ^= (uint8_t)bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

djs_string_t *djs_string_new(djs_runtime_t *rt, const char *bytes, size_t length) {
    djs_string_t *string = (djs_string_t *)djs_arena_alloc(rt, sizeof(*string));
    if (string == NULL) return NULL;
    char *copy = (char *)djs_arena_alloc(rt, length + 1);
    if (copy == NULL) return NULL;
    if (length > 0) djs_memcpy(copy, bytes, length);
    copy[length] = '\0';
    string->length = length;
    string->hash = hash_bytes(copy, length);
    string->bytes = copy;
    return string;
}

djs_string_t *djs_string_new_cstr(djs_runtime_t *rt, const char *text) {
    return djs_string_new(rt, text, djs_strlen(text));
}

bool djs_string_equals(djs_string_t *a, djs_string_t *b) {
    if (a == b) return true;
    if (a == NULL || b == NULL) return false;
    if (a->hash != b->hash || a->length != b->length) return false;
    return djs_memcmp_helper(a->bytes, b->bytes, a->length) == 0;
}

/* ---------------------------------------------------------------- numbers */

/* The digit extraction uses long double (64-bit mantissa on x86-64) so that
 * 17 significant digits survive the scaling steps; the result is then
 * narrowed to the shortest decimal string that reads back as the same
 * double. */

static bool is_space_char(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v' || (unsigned char)c == 0xA0;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static long double pow10_long(long double base, int exponent) {
    long double result = 1.0L;
    if (exponent > 0) {
        for (int i = 0; i < exponent; i++) result *= base;
    } else {
        for (int i = 0; i < -exponent; i++) result /= base;
    }
    return result;
}

/* Scale `value` by 10^exponent using chunked exact powers of ten so the
 * accumulated error stays below the 18th significant digit. */
static long double scale_pow10(long double value, int exponent) {
    const long double chunk = 10000000000000000000.0L; /* 1e19, exact */
    while (exponent > 19) { value /= chunk; exponent -= 19; }
    while (exponent < -19) { value *= chunk; exponent += 19; }
    if (exponent > 0) value *= pow10_long(10.0L, exponent);
    else if (exponent < 0) value /= pow10_long(10.0L, -exponent);
    return value;
}

/* Extract up to 18 significant digits and the decimal exponent E2 such that
 * the value equals d1.d2... x 10^E2. Returns the number of digits written. */
static int extract_digits(double value, char *digits, int *exponent_out) {
    long double wide = (long double)value;
    int exponent = 0;

    /* Bring the value into [1, 10) with chunked scaling: each step divides
     * by an exactly representable power of ten, so the error stays far below
     * the 18th significant digit. */
    if (wide >= 10.0L) {
        while (wide >= 1.0e19L) { wide /= 1.0e19L; exponent += 19; }
        while (wide >= 10.0L) { wide /= 10.0L; exponent++; }
    } else {
        while (wide < 1.0e-19L) { wide *= 1.0e19L; exponent -= 19; }
        while (wide < 1.0L) { wide *= 10.0L; exponent--; }
    }

    int count = 0;
    for (int i = 0; i < 18; i++) {
        int digit = (int)wide;
        if (digit >= 10) {
            /* Rounding pushed the value up to the next power of ten. */
            digits[0] = '1';
            for (int j = 1; j < i; j++) digits[j] = '0';
            count = i;
            exponent++;
            break;
        }
        if (digit < 0) digit = 0;
        digits[count++] = (char)('0' + digit);
        wide = (wide - (long double)digit) * 10.0L;
    }
    digits[count] = '\0';
    *exponent_out = exponent;
    return count;
}

/* Round the digit string to `keep` digits. Returns the new length and sets
 * *carried when the rounding overflowed into the next power of ten. */
static int round_digits(const char *digits, int count, int keep, char *out,
                        bool *carried) {
    *carried = false;
    int length = keep;
    for (int i = 0; i < keep; i++) out[i] = digits[i];
    out[keep] = '\0';
    if (keep >= count) return length;

    if (digits[keep] < '5') {
        while (length > 1 && out[length - 1] == '0') {
            out[length - 1] = '\0';
            length--;
        }
        return length;
    }

    int index = keep - 1;
    while (index >= 0) {
        if (out[index] != '9') { out[index]++; break; }
        out[index] = '0';
        index--;
    }
    if (index < 0) {
        for (int i = length; i > 0; i--) out[i] = out[i - 1];
        out[0] = '1';
        length++;
        *carried = true;
    }
    while (length > 1 && out[length - 1] == '0') {
        out[length - 1] = '\0';
        length--;
    }
    return length;
}

/* Parse a plain decimal literal (digits, optional '.', optional exponent).
 * Used both for Number() and for the round-trip check below. */
static bool parse_decimal(const char *text, size_t length, double *out) {
    long double mantissa = 0.0L;
    int fraction_digits = 0;
    int explicit_exponent = 0;
    size_t index = 0;
    bool any = false;

    while (index < length && is_digit(text[index])) {
        mantissa = mantissa * 10.0L + (long double)(text[index] - '0');
        index++;
        any = true;
    }
    if (index < length && text[index] == '.') {
        index++;
        while (index < length && is_digit(text[index])) {
            mantissa = mantissa * 10.0L + (long double)(text[index] - '0');
            fraction_digits++;
            index++;
            any = true;
        }
    }
    if (!any) return false;
    if (index < length && (text[index] == 'e' || text[index] == 'E')) {
        index++;
        int sign = 1;
        if (index < length && (text[index] == '+' || text[index] == '-')) {
            if (text[index] == '-') sign = -1;
            index++;
        }
        if (index >= length || !is_digit(text[index])) return false;
        int value = 0;
        while (index < length && is_digit(text[index])) {
            if (value < 100000) value = value * 10 + (text[index] - '0');
            index++;
        }
        explicit_exponent = sign * value;
    }
    if (index != length) return false;

    long double scaled = scale_pow10(mantissa, explicit_exponent - fraction_digits);
    *out = (double)scaled;
    return true;
}

bool djs_string_to_number(const char *text, size_t length, double *out) {
    size_t start = 0;
    while (start < length && is_space_char(text[start])) start++;
    size_t end = length;
    while (end > start && is_space_char(text[end - 1])) end--;
    if (start == end) { *out = 0.0; return true; }

    const char *body = text + start;
    size_t body_length = end - start;

    /* Hexadecimal, octal and binary literals. */
    if (body_length > 2 && body[0] == '0') {
        int base = 0;
        if (body[1] == 'x' || body[1] == 'X') base = 16;
        else if (body[1] == 'o' || body[1] == 'O') base = 8;
        else if (body[1] == 'b' || body[1] == 'B') base = 2;
        if (base != 0) {
            double value = 0.0;
            for (size_t i = 2; i < body_length; i++) {
                char c = body[i];
                int digit;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                else return false;
                if (digit >= base) return false;
                value = value * (double)base + (double)digit;
            }
            *out = value;
            return true;
        }
    }

    /* Infinity. */
    size_t offset = 0;
    bool negative = false;
    if (body[0] == '+' || body[0] == '-') {
        negative = body[0] == '-';
        offset = 1;
    }
    if (body_length - offset == 8 && djs_memcmp_helper(body + offset, "Infinity", 8) == 0) {
        *out = negative ? -1.0 / 0.0 : 1.0 / 0.0;
        return true;
    }

    double value = 0.0;
    if (!parse_decimal(body + offset, body_length - offset, &value)) return false;
    *out = negative ? -value : value;
    return true;
}

bool djs_number_is_integer(double value) {
    if (value != value) return false;              /* NaN */
    if (value > 1.7976931348623157e308) return false;
    if (value < -1.7976931348623157e308) return false;
    if (value == 0.0) return true;
    double magnitude = value < 0.0 ? -value : value;
    if (magnitude >= 4503599627370496.0) return true; /* 2^52: every double is an int */
    double truncated = (double)(long long)value;
    return truncated == value;
}

/* Build the shortest decimal representation of `value`. */
void djs_number_to_string(double value, char *buffer, size_t buffer_size) {
    if (buffer_size == 0) return;
    buffer[0] = '\0';
    if (value != value) { djs_memcpy(buffer, "NaN", 4); return; }
    if (value == 1.0 / 0.0) { djs_memcpy(buffer, "Infinity", 9); return; }
    if (value == -1.0 / 0.0) { djs_memcpy(buffer, "-Infinity", 10); return; }
    if (value == 0.0) { djs_memcpy(buffer, "0", 2); return; }

    bool negative = value < 0.0;
    double magnitude = negative ? -value : value;

    char digits[32];
    int exponent = 0;
    int count = extract_digits(magnitude, digits, &exponent);

    /* Find the shortest prefix that reads back as the same double. */
    char best[32];
    int best_length = count;
    int best_exponent = exponent;
    djs_memcpy(best, digits, (size_t)count + 1);

    for (int keep = 1; keep <= count; keep++) {
        char scratch[32];
        bool carried = false;
        int length = round_digits(digits, count, keep, scratch, &carried);
        int adjusted = exponent + (carried ? 1 : 0);

        char candidate[64];
        int written = 0;
        candidate[written++] = scratch[0];
        if (length > 1) {
            candidate[written++] = '.';
            for (int i = 1; i < length; i++) candidate[written++] = scratch[i];
        }
        written += djs_format_exponent(candidate + written, adjusted);
        double parsed = 0.0;
        if (djs_string_to_number(candidate, (size_t)written, &parsed) && parsed == magnitude) {
            best_length = length;
            best_exponent = adjusted;
            djs_memcpy(best, scratch, (size_t)length + 1);
            break;
        }
    }

    /* best describes magnitude = 0.best x 10^(best_exponent + 1). */
    int k = best_length;
    int n = best_exponent + 1;

    size_t needed;
    if (k <= n && n <= 21) needed = (size_t)k + (size_t)(n - k);
    else if (0 < n && n <= 21) needed = (size_t)n + 1 + (size_t)(k - n);
    else if (-6 < n && n <= 0) needed = (size_t)(-n) + 2 + (size_t)k;
    else needed = (size_t)k + 1 + (size_t)(k > 1 ? k - 1 : 0) + 4;
    if (negative) needed++;
    if (needed + 1 > buffer_size) needed = buffer_size - 1;

    size_t position = 0;
    if (negative && position < needed) buffer[position++] = '-';
    if (k <= n && n <= 21) {
        for (int i = 0; i < k && position < needed; i++) buffer[position++] = best[i];
        for (int i = 0; i < n - k && position < needed; i++) buffer[position++] = '0';
    } else if (0 < n && n <= 21) {
        for (int i = 0; i < n && position < needed; i++) buffer[position++] = best[i];
        if (position < needed) buffer[position++] = '.';
        for (int i = n; i < k && position < needed; i++) buffer[position++] = best[i];
    } else if (-6 < n && n <= 0) {
        buffer[position++] = '0';
        if (position < needed) buffer[position++] = '.';
        for (int i = 0; i < -n && position < needed; i++) buffer[position++] = '0';
        for (int i = 0; i < k && position < needed; i++) buffer[position++] = best[i];
    } else {
        buffer[position++] = best[0];
        if (k > 1) {
            if (position < needed) buffer[position++] = '.';
            for (int i = 1; i < k && position < needed; i++) buffer[position++] = best[i];
        }
        if (position < needed) buffer[position++] = 'e';
        if (position < needed) buffer[position++] = (n - 1) >= 0 ? '+' : '-';
        char exponent_digits[16];
        int length = 0;
        int exponent_value = (n - 1) >= 0 ? (n - 1) : -(n - 1);
        if (exponent_value == 0) {
            exponent_digits[length++] = '0';
        } else {
            char reverse[16];
            int count_digits = 0;
            while (exponent_value > 0 && count_digits < 15) {
                reverse[count_digits++] = (char)('0' + (exponent_value % 10));
                exponent_value /= 10;
            }
            while (count_digits > 0) exponent_digits[length++] = reverse[--count_digits];
        }
        for (int i = 0; i < length && position < needed; i++) buffer[position++] = exponent_digits[i];
    }
    buffer[position] = '\0';
}

/* --------------------------------------------------------------- runtime */

/* Create the object graph the builtins are installed into: the prototypes,
 * the global object and the constructor objects. */
static void runtime_setup(djs_runtime_t *rt) {
    rt->object_prototype = djs_object_new(rt, NULL);
    rt->function_prototype = djs_object_new(rt, rt->object_prototype);
    rt->array_prototype = djs_object_new(rt, rt->object_prototype);
    rt->string_prototype = djs_object_new(rt, rt->object_prototype);
    rt->number_prototype = djs_object_new(rt, rt->object_prototype);
    rt->boolean_prototype = djs_object_new(rt, rt->object_prototype);
    rt->error_prototype = djs_object_new(rt, rt->object_prototype);
    rt->promise_prototype = djs_object_new(rt, rt->object_prototype);
    rt->map_prototype = djs_object_new(rt, rt->object_prototype);
    rt->set_prototype = djs_object_new(rt, rt->object_prototype);
    rt->date_prototype = djs_object_new(rt, rt->object_prototype);
    rt->regexp_prototype = djs_object_new(rt, rt->object_prototype);
    rt->global = djs_object_new(rt, rt->object_prototype);
    rt->global_env = djs_env_new(rt, NULL);
    if (rt->global_env != NULL) rt->global_env->function_scope = true;

    rt->object_constructor = djs_function_new(rt, NULL, "Object", 1);
    rt->array_constructor = djs_function_new(rt, NULL, "Array", 1);
    rt->function_constructor = djs_function_new(rt, NULL, "Function", 1);
    rt->promise_constructor = djs_function_new(rt, NULL, "Promise", 1);
    rt->error_constructor = djs_function_new(rt, NULL, "Error", 1);
    rt->type_error_constructor = djs_function_new(rt, NULL, "TypeError", 1);
    rt->range_error_constructor = djs_function_new(rt, NULL, "RangeError", 1);
    rt->syntax_error_constructor = djs_function_new(rt, NULL, "SyntaxError", 1);
    rt->reference_error_constructor = djs_function_new(rt, NULL, "ReferenceError", 1);

    /* Every constructor points at its prototype, and every prototype points
     * back at its constructor. */
    struct { djs_object_t *constructor; djs_object_t *prototype; } pairs[] = {
        { rt->object_constructor, rt->object_prototype },
        { rt->array_constructor, rt->array_prototype },
        { rt->function_constructor, rt->function_prototype },
        { rt->promise_constructor, rt->promise_prototype },
        { rt->error_constructor, rt->error_prototype },
        { rt->type_error_constructor, rt->error_prototype },
        { rt->range_error_constructor, rt->error_prototype },
        { rt->syntax_error_constructor, rt->error_prototype },
        { rt->reference_error_constructor, rt->error_prototype },
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        if (pairs[i].constructor == NULL) continue;
        djs_property_set(rt, pairs[i].constructor, "prototype",
                         djs_object_value(pairs[i].prototype), 0);
        if (pairs[i].prototype != NULL) {
            djs_property_set(rt, pairs[i].prototype, "constructor",
                             djs_object_value(pairs[i].constructor), 0);
        }
    }
    /* The error constructors inherit from Error.prototype. */
    djs_object_t *error_constructors[] = {
        rt->type_error_constructor, rt->range_error_constructor,
        rt->syntax_error_constructor, rt->reference_error_constructor
    };
    for (size_t i = 0; i < sizeof(error_constructors) / sizeof(error_constructors[0]); i++) {
        if (error_constructors[i] == NULL) continue;
        error_constructors[i]->prototype = djs_object_value(rt->error_prototype);
        djs_property_set(rt, error_constructors[i], "prototype",
                         djs_object_value(rt->error_prototype), 0);
    }
}

const char *djs_version(void) { return "0.1.0-helix"; }

djs_runtime_t *djs_runtime_new(const djs_options_t *options) {
    djs_runtime_t *rt = (djs_runtime_t *)DJS_HOST_ALLOC(sizeof(*rt));
    if (rt == NULL) return NULL;
    djs_memset(rt, 0, sizeof(*rt));
    rt->random_state = 0x2545F4914F6CDD1DULL;
    if (options != NULL) {
        rt->write_fn = options->write_fn;
        rt->write_user = options->write_user;
        rt->platform = options->platform ? djs_arena_strdup(rt, options->platform) : "helix";
        rt->arch = options->arch ? djs_arena_strdup(rt, options->arch) : "x64";
        rt->version = options->version ? djs_arena_strdup(rt, options->version) : djs_version();
        rt->exec_path = options->exec_path ? djs_arena_strdup(rt, options->exec_path) : "donejs";
        rt->cwd = options->cwd ? djs_arena_strdup(rt, options->cwd) : "/";
    } else {
        rt->platform = "helix";
        rt->arch = "x64";
        rt->version = djs_version();
        rt->exec_path = "donejs";
        rt->cwd = "/";
    }
    runtime_setup(rt);
    /* The root frame: every script runs inside a frame, so `this` is the
     * global object and a block can open a scope of its own. */
    djs_frame_t *root = (djs_frame_t *)djs_arena_calloc(rt, 1, sizeof(*root));
    if (root != NULL) {
        root->env = rt->global_env;
        root->this_value = djs_object_value(rt->global);
        root->new_target = DJS_UNDEFINED;
        root->super_class = DJS_UNDEFINED;
        root->home_object = NULL;
        root->pending_fields = NULL;
        root->function = NULL;
        root->arrow = false;
        root->previous = NULL;
        rt->frame = root;
    }
    djs_install_globals(rt);
    djs_install_builtins(rt);
    djs_install_modules(rt);
    return rt;
}

void djs_runtime_free(djs_runtime_t *rt) {
    if (rt == NULL) return;
    djs_arena_release(rt);
    DJS_HOST_FREE(rt);
}

void djs_write(djs_runtime_t *rt, const char *text, size_t length) {
    if (rt->write_fn == NULL) return;
    rt->write_fn(rt->write_user, text, length);
}

void djs_write_cstr(djs_runtime_t *rt, const char *text) {
    djs_write(rt, text, djs_strlen(text));
}
