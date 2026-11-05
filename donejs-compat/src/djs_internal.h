/* djs_internal.h — shared types for the donejs runtime port.
 *
 * The runtime is a tree-walking interpreter over a parsed AST, with an arena
 * allocator: every allocation belongs to the runtime and is released when it
 * is freed. There is no garbage collector (see the ponytail note in
 * djs_core.c); a script runs to completion and the runtime is torn down.
 */
#ifndef DJS_INTERNAL_H
#define DJS_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../include/donejs.h"

/* ------------------------------------------------------------------ values */

typedef enum {
    DJS_TAG_UNDEFINED = 0,
    DJS_TAG_NULL,
    DJS_TAG_BOOL,
    DJS_TAG_NUMBER,
    DJS_TAG_STRING,
    DJS_TAG_OBJECT
} djs_tag_t;

typedef struct djs_string djs_string_t;
typedef struct djs_object djs_object_t;
typedef struct djs_runtime djs_runtime_t;
typedef struct djs_env djs_env_t;
typedef struct djs_node djs_node_t;
typedef struct djs_function djs_function_t;
typedef struct djs_class_def djs_class_def_t;

typedef struct djs_value {
    uint8_t tag;
    union {
        bool boolean;
        double number;
        djs_string_t *string;
        djs_object_t *object;
    } as;
} djs_value_t;

#define DJS_UNDEFINED ((djs_value_t){ .tag = DJS_TAG_UNDEFINED })
#define DJS_NULL      ((djs_value_t){ .tag = DJS_TAG_NULL })

static inline djs_value_t djs_bool(bool b) {
    djs_value_t v; v.tag = DJS_TAG_BOOL; v.as.boolean = b; return v;
}
static inline djs_value_t djs_number(double n) {
    djs_value_t v; v.tag = DJS_TAG_NUMBER; v.as.number = n; return v;
}
static inline djs_value_t djs_string_value(djs_string_t *s) {
    djs_value_t v; v.tag = DJS_TAG_STRING; v.as.string = s; return v;
}
static inline djs_value_t djs_object_value(djs_object_t *o) {
    djs_value_t v; v.tag = DJS_TAG_OBJECT; v.as.object = o; return v;
}

static inline bool djs_is_undefined(djs_value_t v) { return v.tag == DJS_TAG_UNDEFINED; }
static inline bool djs_is_null(djs_value_t v)      { return v.tag == DJS_TAG_NULL; }
static inline bool djs_is_nullish(djs_value_t v)   { return v.tag == DJS_TAG_UNDEFINED || v.tag == DJS_TAG_NULL; }
static inline bool djs_is_bool(djs_value_t v)      { return v.tag == DJS_TAG_BOOL; }
static inline bool djs_is_number(djs_value_t v)    { return v.tag == DJS_TAG_NUMBER; }
static inline bool djs_is_string(djs_value_t v)    { return v.tag == DJS_TAG_STRING; }
static inline bool djs_is_object(djs_value_t v)    { return v.tag == DJS_TAG_OBJECT; }

/* ----------------------------------------------------------------- strings */

struct djs_string {
    size_t length;      /* bytes (UTF-8) */
    uint32_t hash;
    char *bytes;        /* NUL terminated */
};

/* ----------------------------------------------------------------- objects */

typedef enum {
    DJS_CLASS_OBJECT = 0,
    DJS_CLASS_ARRAY,
    DJS_CLASS_FUNCTION,
    DJS_CLASS_ERROR,
    DJS_CLASS_PROMISE,
    DJS_CLASS_MAP,
    DJS_CLASS_SET,
    DJS_CLASS_DATE,
    DJS_CLASS_REGEXP,
    DJS_CLASS_GENERATOR
} djs_class_t;

#define DJS_PROP_ENUMERABLE   1u
#define DJS_PROP_WRITABLE     2u
#define DJS_PROP_CONFIGURABLE 4u
#define DJS_PROP_GETTER       8u
#define DJS_PROP_SETTER      16u
#define DJS_PROP_PRIVATE     32u   /* #name fields: hidden from Object.keys */

/* Function flags. */
#define DJS_FN_ARROW      1u
#define DJS_FN_GENERATOR  2u
#define DJS_FN_ASYNC      4u
#define DJS_FN_METHOD     8u
#define DJS_FN_GETTER    16u
#define DJS_FN_SETTER    32u
#define DJS_FN_CTOR      64u
#define DJS_FN_EXPR_BODY 128u

typedef djs_value_t (*djs_native_fn)(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count);

typedef struct djs_function {
    djs_value_t name;
    int arity;
    djs_native_fn native;      /* builtin functions */
    djs_node_t *params;        /* DJS_N_FUNCTION node holding the parameters */
    djs_node_t *body;          /* DJS_N_BLOCK or expression body */
    djs_env_t *closure;        /* defining lexical environment */
    djs_value_t home_object;   /* object the function was defined in (super) */
    djs_value_t super_class;   /* parent constructor of a class method */
    djs_class_def_t *class_def;/* class a constructor belongs to */
    uint32_t flags;
    djs_object_t *owner;       /* the function object that owns this record */
} djs_function_t;

typedef struct djs_binding {
    const char *name;
    djs_value_t value;
    uint32_t flags;            /* 1 = mutable */
    bool initialized;
} djs_binding_t;

struct djs_env {
    djs_binding_t *bindings;
    size_t count;
    size_t capacity;
    djs_env_t *parent;
};

typedef enum {
    DJS_PROMISE_PENDING = 0,
    DJS_PROMISE_FULFILLED,
    DJS_PROMISE_REJECTED
} djs_promise_state_t;

typedef struct djs_map_entry {
    djs_value_t key;
    djs_value_t value;
    bool deleted;
    struct djs_map_entry *next;
} djs_map_entry_t;

typedef struct djs_property {
    djs_value_t key;
    djs_value_t value;
    uint32_t flags;
    struct djs_property *next;
} djs_property_t;

/* A queued callback: microtasks, promise reactions and timers. */
typedef struct djs_task {
    djs_value_t callback;
    djs_value_t *args;
    size_t arg_count;
    struct djs_task *next;
} djs_task_t;

struct djs_object {
    djs_class_t class;
    djs_value_t prototype;
    djs_property_t *first_property;
    djs_property_t *last_property;
    djs_value_t *elements;        /* array storage */
    size_t element_count;
    size_t element_capacity;
    djs_function_t *function;     /* DJS_CLASS_FUNCTION */
    djs_value_t error_message;    /* DJS_CLASS_ERROR */
    /* promise */
    int promise_state;
    djs_value_t promise_value;
    djs_task_t *promise_fulfilled;
    djs_task_t *promise_rejected;
    /* map / set */
    djs_map_entry_t *map_entries;
    size_t map_count;
    /* regexp */
    char *regexp_source;
    char *regexp_flags;
    /* date */
    double date_value;
    /* generator */
    struct djs_coro *coroutine;
    bool frozen;
    bool extensible;
};

/* -------------------------------------------------------------------- AST */

typedef enum {
    DJS_N_PROGRAM = 0,
    DJS_N_NUMBER,
    DJS_N_STRING,
    DJS_N_TEMPLATE,
    DJS_N_REGEXP,
    DJS_N_BOOLEAN,
    DJS_N_NULL,
    DJS_N_UNDEFINED,
    DJS_N_IDENT,
    DJS_N_THIS,
    DJS_N_SUPER,
    DJS_N_NEWTARGET,
    DJS_N_ARRAY,
    DJS_N_OBJECT,
    DJS_N_FUNCTION,
    DJS_N_CLASS,
    DJS_N_CALL,
    DJS_N_NEW,
    DJS_N_MEMBER,
    DJS_N_UNARY,
    DJS_N_BINARY,
    DJS_N_LOGICAL,
    DJS_N_ASSIGN,
    DJS_N_CONDITIONAL,
    DJS_N_UPDATE,
    DJS_N_SEQUENCE,
    DJS_N_SPREAD,
    DJS_N_YIELD,
    DJS_N_AWAIT,
    DJS_N_TAGGED,
    DJS_N_BLOCK,
    DJS_N_VAR,
    DJS_N_EXPR_STMT,
    DJS_N_IF,
    DJS_N_FOR,
    DJS_N_FOR_IN,
    DJS_N_FOR_OF,
    DJS_N_WHILE,
    DJS_N_DO_WHILE,
    DJS_N_SWITCH,
    DJS_N_CASE,
    DJS_N_BREAK,
    DJS_N_CONTINUE,
    DJS_N_RETURN,
    DJS_N_THROW,
    DJS_N_TRY,
    DJS_N_EMPTY,
    DJS_N_LABEL,
    DJS_N_DECLARATOR,
    DJS_N_PROPERTY,
    DJS_N_PARAM,
    DJS_N_ARRAY_PATTERN,
    DJS_N_OBJECT_PATTERN,
    DJS_N_ASSIGN_PATTERN,
    DJS_N_REST
} djs_node_kind_t;

/* Node flags. */
#define DJS_NODE_COMPUTED  1u   /* property key / member index is computed */
#define DJS_NODE_STATIC    2u   /* class member is static */
#define DJS_NODE_SHORTHAND 4u   /* object literal shorthand */
#define DJS_NODE_SPREAD    8u   /* object literal / array element spread */
#define DJS_NODE_PREFIX   16u   /* ++/-- prefix form */
#define DJS_NODE_LET      32u   /* var declaration is let */
#define DJS_NODE_CONST    64u   /* var declaration is const */
#define DJS_NODE_OPTIONAL 128u  /* ?. member access */
#define DJS_NODE_DELEGATE 256u  /* yield* */
#define DJS_NODE_GETTER   512u  /* object/class accessor */
#define DJS_NODE_SETTER  1024u
#define DJS_NODE_METHOD  2048u  /* concise method (no `function` keyword) */

typedef struct djs_function_def {
    char *name;
    djs_node_t *body;
    uint32_t flags;
    bool expression_body;
} djs_function_def_t;

struct djs_class_def {
    char *name;
    djs_node_t *superclass;
    djs_node_t *constructor;    /* explicit constructor member, or NULL */
};

struct djs_node {
    uint8_t kind;
    uint32_t flags;
    int line;
    double number;
    char *text;                 /* identifier, string value, operator */
    char *text2;                /* regexp flags */
    djs_node_t *a;
    djs_node_t *b;
    djs_node_t *c;
    djs_node_t *d;
    djs_node_t **list;          /* args, elements, statements, params */
    size_t list_count;
    size_t list_capacity;
    void *aux;                  /* djs_function_def_t / djs_class_def_t */
};

/* ------------------------------------------------------------------- arena */

typedef struct djs_arena_block {
    struct djs_arena_block *next;
    size_t used;
    size_t capacity;
    unsigned char *data;
} djs_arena_block_t;

typedef struct {
    djs_arena_block_t *first;
    djs_arena_block_t *current;
} djs_arena_t;

/* ---------------------------------------------------------------- runtime */

/* Own setjmp/longjmp plus a context switch: the freestanding build cannot use
 * libc's setjmp, and generators need to move between stacks. */
typedef struct {
    uint64_t rbx, rbp, r12, r13, r14, r15, rsp, rip;
} djs_jmp_buf_t;

int djs_setjmp(djs_jmp_buf_t *buf);
__attribute__((noreturn)) void djs_longjmp(djs_jmp_buf_t *buf, int value);
void djs_context_switch(djs_jmp_buf_t *save_to, djs_jmp_buf_t *from);
void djs_coro_enter(djs_jmp_buf_t *save_to, void *stack_top,
                    void (*entry)(void *), void *arg);

typedef enum {
    DJS_CORO_RUNNING = 0,
    DJS_CORO_SUSPENDED,
    DJS_CORO_DONE
} djs_coro_state_t;

typedef struct djs_coro {
    djs_runtime_t *rt;
    void *stack;
    size_t stack_size;
    djs_jmp_buf_t ctx;        /* the coroutine's own saved context */
    djs_jmp_buf_t caller;     /* where to return to */
    djs_value_t sent;         /* value sent into the coroutine */
    djs_value_t yielded;      /* value yielded out of the coroutine */
    djs_value_t error;        /* pending exception thrown into the coroutine */
    djs_value_t result;       /* final result of the coroutine body */
    bool has_error;
    bool started;
    djs_coro_state_t state;
} djs_coro_t;

typedef struct djs_catch_frame {
    djs_jmp_buf_t saved;
    struct djs_catch_frame *previous;
    djs_value_t error;
} djs_catch_frame_t;

typedef struct djs_frame {
    djs_env_t *env;
    djs_value_t this_value;
    djs_value_t new_target;
    djs_value_t super_class;
    djs_object_t *home_object;
    djs_function_t *function;
    bool arrow;
    struct djs_frame *previous;
} djs_frame_t;

typedef struct djs_timer {
    double due_ms;
    double interval_ms;
    bool repeating;
    djs_value_t callback;
    uint64_t id;
    struct djs_timer *next;
} djs_timer_t;

struct djs_runtime {
    djs_arena_t arena;
    djs_object_t *global;
    djs_env_t *global_env;
    djs_frame_t *frame;

    djs_object_t *object_prototype;
    djs_object_t *function_prototype;
    djs_object_t *array_prototype;
    djs_object_t *string_prototype;
    djs_object_t *number_prototype;
    djs_object_t *boolean_prototype;
    djs_object_t *error_prototype;
    djs_object_t *promise_prototype;
    djs_object_t *map_prototype;
    djs_object_t *set_prototype;
    djs_object_t *date_prototype;
    djs_object_t *regexp_prototype;

    djs_object_t *object_constructor;
    djs_object_t *array_constructor;
    djs_object_t *function_constructor;
    djs_object_t *promise_constructor;
    djs_object_t *error_constructor;
    djs_object_t *type_error_constructor;
    djs_object_t *range_error_constructor;
    djs_object_t *syntax_error_constructor;
    djs_object_t *reference_error_constructor;

    djs_catch_frame_t *catch_frame;
    djs_task_t *microtasks;         /* queueMicrotask + promise reactions */
    djs_task_t *microtask_tail;
    djs_task_t *ticks;               /* process.nextTick */
    djs_task_t *tick_tail;
    djs_timer_t *timers;
    uint64_t next_timer_id;

    djs_write_fn write_fn;
    void *write_user;
    const char *platform;
    const char *arch;
    const char *version;
    const char *exec_path;
    const char *cwd;

    djs_object_t *module_registry;   /* name -> module exports */
    djs_object_t *require_object;
    bool allow_exec;
    double start_time_ms;
    uint64_t random_state;
    int exit_status;
    bool exiting;
    bool has_exit_code;
};

/* --------------------------------------------------------------- utilities */

/* Host allocator, so the interpreter can allocate coroutine stacks. */
void *djs_host_alloc(size_t size);
void djs_host_free(void *ptr);

/* Arena. */
void *djs_arena_alloc(djs_runtime_t *rt, size_t size);
void *djs_arena_calloc(djs_runtime_t *rt, size_t count, size_t size);
char *djs_arena_strdup(djs_runtime_t *rt, const char *text);
char *djs_arena_strndup(djs_runtime_t *rt, const char *text, size_t length);
void djs_arena_release(djs_runtime_t *rt);

/* Strings. */
djs_string_t *djs_string_new(djs_runtime_t *rt, const char *bytes, size_t length);
djs_string_t *djs_string_new_cstr(djs_runtime_t *rt, const char *text);
bool djs_string_equals(djs_string_t *a, djs_string_t *b);
bool djs_string_equals_cstr(djs_string_t *a, const char *text);

/* Numbers. */
void djs_number_to_string(double value, char *buffer, size_t buffer_size);
bool djs_string_to_number(const char *text, size_t length, double *out);
bool djs_number_is_integer(double value);
int djs_format_exponent(char *out, int exponent);

/* Values. */
djs_value_t djs_to_string(djs_runtime_t *rt, djs_value_t value);
djs_value_t djs_to_number(djs_runtime_t *rt, djs_value_t value);
bool djs_to_boolean(djs_value_t value);
djs_value_t djs_to_primitive(djs_runtime_t *rt, djs_value_t value, int hint);
bool djs_strict_equals(djs_value_t a, djs_value_t b);
bool djs_loose_equals(djs_runtime_t *rt, djs_value_t a, djs_value_t b);
const char *djs_typeof(djs_value_t value);

/* Objects. */
djs_object_t *djs_object_new(djs_runtime_t *rt, djs_object_t *prototype);
djs_object_t *djs_array_new(djs_runtime_t *rt);
djs_object_t *djs_function_new(djs_runtime_t *rt, djs_native_fn native,
                                const char *name, int arity);
djs_property_t *djs_property_get(djs_object_t *object, const char *name);
void djs_property_set(djs_runtime_t *rt, djs_object_t *object, const char *name,
                      djs_value_t value, uint32_t flags);
bool djs_property_delete(djs_runtime_t *rt, djs_object_t *object, const char *name);
djs_value_t djs_object_get(djs_runtime_t *rt, djs_object_t *object, const char *name);
djs_value_t djs_object_get_value(djs_runtime_t *rt, djs_object_t *object, djs_value_t key);
void djs_object_set(djs_runtime_t *rt, djs_object_t *object, const char *name,
                    djs_value_t value);
bool djs_object_has(djs_object_t *object, const char *name);
djs_value_t djs_array_get(djs_runtime_t *rt, djs_object_t *array, size_t index);
void djs_array_set(djs_runtime_t *rt, djs_object_t *array, size_t index, djs_value_t value);
void djs_array_push(djs_runtime_t *rt, djs_object_t *array, djs_value_t value);
size_t djs_array_length(djs_object_t *array);
void djs_array_set_length(djs_runtime_t *rt, djs_object_t *array, size_t length);
void djs_define_method(djs_runtime_t *rt, djs_object_t *object, const char *name,
                       djs_native_fn fn, int arity);
void djs_define_value(djs_runtime_t *rt, djs_object_t *object, const char *name,
                      djs_value_t value);
djs_object_t *djs_object_create(djs_runtime_t *rt, djs_object_t *prototype);
djs_value_t djs_make_array(djs_runtime_t *rt, djs_value_t *items, size_t count);
djs_value_t djs_make_object(djs_runtime_t *rt);

/* Environments. */
djs_env_t *djs_env_new(djs_runtime_t *rt, djs_env_t *parent);
djs_binding_t *djs_env_lookup(djs_env_t *env, const char *name);
djs_binding_t *djs_env_declare(djs_runtime_t *rt, djs_env_t *env, const char *name,
                               djs_value_t value, bool mutable_binding);
void djs_env_assign(djs_runtime_t *rt, djs_env_t *env, const char *name, djs_value_t value);

/* Errors and exceptions. */
__attribute__((noreturn)) void djs_throw(djs_runtime_t *rt, djs_value_t error);
__attribute__((noreturn)) void djs_throw_error(djs_runtime_t *rt, const char *kind,
                                               const char *message);
__attribute__((noreturn)) void djs_throw_type_error(djs_runtime_t *rt, const char *message);
djs_value_t djs_make_error(djs_runtime_t *rt, const char *kind, const char *message);
void djs_format_error(djs_runtime_t *rt, djs_value_t error, char *buffer, size_t size);

/* Output. */
void djs_write(djs_runtime_t *rt, const char *text, size_t length);
void djs_write_cstr(djs_runtime_t *rt, const char *text);

/* Calls. */
djs_value_t djs_call(djs_runtime_t *rt, djs_value_t callee, djs_value_t this_value,
                     djs_value_t *args, size_t arg_count);
djs_value_t djs_call_method(djs_runtime_t *rt, djs_object_t *object, const char *name,
                             djs_value_t *args, size_t arg_count);
djs_value_t djs_construct(djs_runtime_t *rt, djs_value_t callee, djs_value_t *args,
                          size_t arg_count);
bool djs_is_callable(djs_value_t value);

/* ------------------------------------------------------------------ lexer */

typedef enum {
    DJS_TOK_EOF = 0,
    DJS_TOK_NUMBER,
    DJS_TOK_STRING,
    DJS_TOK_IDENTIFIER,
    DJS_TOK_PUNCT,
    DJS_TOK_REGEXP,
    DJS_TOK_TEMPLATE
} djs_token_type_t;

typedef struct djs_token {
    int type;
    double number;
    char *text;                 /* string literal value or identifier */
    char *text2;                /* regexp flags */
    void *extra;                /* template parts */
    int position;
    int length;
    bool newline_before;        /* a line terminator preceded this token */
} djs_token_t;

/* One part of a template literal: a cooked string chunk or the raw source of
 * a substitution. */
typedef struct djs_template_part {
    bool is_expression;
    char *text;                 /* cooked text / expression source */
    char *raw;                  /* raw text (string parts only) */
} djs_template_part_t;

/* The parts of one template literal token. */
typedef struct djs_template {
    djs_template_part_t *parts;
    size_t count;
} djs_template_t;

typedef struct djs_lexer {
    djs_runtime_t *rt;
    const char *source;
    size_t length;
    size_t position;
    int line;
    bool regex_allowed;      /* the previous token allows a regexp literal */
    bool saw_newline;        /* a line terminator was skipped */
} djs_lexer_t;

void djs_lexer_init(djs_lexer_t *lexer, djs_runtime_t *rt, const char *source);
int djs_lexer_next(djs_lexer_t *lexer, djs_token_t *token, char **error_out);

/* ------------------------------------------------------------------ parser */

djs_node_t *djs_parse(djs_runtime_t *rt, const char *source, const char *filename,
                      char **error_out);

/* --------------------------------------------------------------- interpreter */

typedef enum {
    DJS_C_NORMAL = 0,
    DJS_C_BREAK,
    DJS_C_CONTINUE,
    DJS_C_RETURN
} djs_completion_t;

djs_value_t djs_eval_node(djs_runtime_t *rt, djs_node_t *node);
djs_completion_t djs_exec_node(djs_runtime_t *rt, djs_node_t *node);
djs_completion_t djs_exec_list(djs_runtime_t *rt, djs_node_t **list, size_t count);
djs_value_t djs_run_program(djs_runtime_t *rt, djs_node_t *program);
djs_value_t djs_eval_source(djs_runtime_t *rt, const char *source, const char *filename,
                            char **error_out);

/* Value helpers used across the implementation files. */
djs_value_t djs_get_prop(djs_runtime_t *rt, djs_value_t base, djs_value_t key);
void djs_set_prop(djs_runtime_t *rt, djs_value_t base, djs_value_t key, djs_value_t value);
djs_value_t djs_call_method_value(djs_runtime_t *rt, djs_value_t base, const char *name,
                                   djs_value_t *args, size_t arg_count);
bool djs_value_equals(djs_runtime_t *rt, djs_value_t a, djs_value_t b);

/* Tasks and timers. */
void djs_queue_microtask(djs_runtime_t *rt, djs_value_t callback, djs_value_t *args,
                          size_t arg_count);
void djs_queue_tick(djs_runtime_t *rt, djs_value_t callback, djs_value_t *args,
                     size_t arg_count);
djs_value_t djs_promise_new(djs_runtime_t *rt);
void djs_promise_resolve(djs_runtime_t *rt, djs_value_t promise, djs_value_t value);
void djs_promise_reject(djs_runtime_t *rt, djs_value_t promise, djs_value_t error);

/* Builtins. */
void djs_install_globals(djs_runtime_t *rt);
void djs_install_builtins(djs_runtime_t *rt);
void djs_install_modules(djs_runtime_t *rt);
djs_value_t djs_require(djs_runtime_t *rt, const char *name);

/* Helpers shared by the implementation files. */
size_t djs_strlen(const char *text);
void djs_memcpy(void *destination, const void *source, size_t count);
void djs_memset(void *destination, int value, size_t count);
int djs_memcmp_helper(const void *a, const void *b, size_t count);
int djs_strcmp(const char *a, const char *b);
bool djs_str_equal(const char *a, const char *b);

#endif /* DJS_INTERNAL_H */
