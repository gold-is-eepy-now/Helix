/* djs_interp.c — tree-walking evaluator for the donejs JavaScript port.
 *
 * Statements run as completions (normal / break / continue / return) so that
 * loops, try and labelled control flow unwind with ordinary C returns. The
 * evaluator pushes djs_frame_t records for functions and class field
 * initialisers; generators and async functions park themselves in a coroutine
 * (djs_jmp.S) and resume through djs_context_switch.
 */

#include "djs_internal.h"

/* ------------------------------------------------------------------ forward */

static djs_value_t eval_node(djs_runtime_t *rt, djs_node_t *node);
static djs_completion_t exec_node(djs_runtime_t *rt, djs_node_t *node);
static djs_completion_t exec_statements(djs_runtime_t *rt, djs_node_t **list,
                                        size_t count);
static djs_value_t eval_super_call(djs_runtime_t *rt, djs_node_t *node);
static djs_value_t apply_binary(djs_runtime_t *rt, const char *op,
                                 djs_value_t left, djs_value_t right);
static double to_int32(double value);
static void declare_name(djs_runtime_t *rt, djs_env_t *env, const char *name,
                         djs_value_t value, bool mutable_binding);
static djs_value_t call_function(djs_runtime_t *rt, djs_function_t *fn,
                                 djs_value_t this_value, djs_value_t *args,
                                 size_t arg_count, djs_value_t new_target);
static void assign_to(djs_runtime_t *rt, djs_node_t *target, djs_value_t value);
static void destructure(djs_runtime_t *rt, djs_env_t *env, djs_node_t *pattern,
                        djs_value_t value, bool declare);
static djs_value_t member_key(djs_runtime_t *rt, djs_node_t *node);
static djs_value_t get_member(djs_runtime_t *rt, djs_value_t base, djs_value_t key);
static void initialize_fields(djs_runtime_t *rt, djs_node_t *class_node,
                              djs_object_t *instance);
static djs_value_t eval_class(djs_runtime_t *rt, djs_node_t *node, bool declaration);

/* ------------------------------------------------------------------ helpers */

static djs_env_t *scope_env(djs_runtime_t *rt) {
    if (rt->frame != NULL && rt->frame->env != NULL) return rt->frame->env;
    return rt->global_env;
}

/* The environment that `var` declarations land in. */
static djs_env_t *var_scope(djs_runtime_t *rt) {
    djs_env_t *env = scope_env(rt);
    while (env != NULL && !env->function_scope) env = env->parent;
    return env != NULL ? env : rt->global_env;
}

static djs_value_t current_this(djs_runtime_t *rt) {
    if (rt->frame != NULL) return rt->frame->this_value;
    return DJS_UNDEFINED;
}

static djs_object_t *current_home(djs_runtime_t *rt) {
    if (rt->frame != NULL) return rt->frame->home_object;
    return NULL;
}

/* The name of an identifier that is not bound anywhere: reading it is an
 * error, but `typeof missing` is not. */
static bool lookup_identifier(djs_runtime_t *rt, const char *name, djs_value_t *out) {
    djs_binding_t *binding = djs_env_lookup(scope_env(rt), name);
    if (binding != NULL) {
        *out = binding->value;
        return true;
    }
    if (rt->global != NULL) {
        djs_property_t *property = djs_property_get(rt->global, name);
        if (property != NULL) {
            *out = property->value;
            return true;
        }
    }
    *out = DJS_UNDEFINED;
    return false;
}

/* A numeric string index into a string value, or -1. */
static long string_index(const char *name) {
    if (name == NULL || name[0] == '\0') return -1;
    if (name[0] == '0' && name[1] != '\0') return -1;
    long value = 0;
    for (const char *p = name; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') return -1;
        value = value * 10 + (*p - '0');
        if (value > 0x7FFFFFFFL) return -1;
    }
    return value;
}

/* Read a property of any value: objects use the property list, primitives
 * use their prototype (strings also expose their length and characters). */
static djs_value_t get_member(djs_runtime_t *rt, djs_value_t base, djs_value_t key) {
    if (base.tag == DJS_TAG_OBJECT) return djs_get_prop(rt, base, key);
    if (base.tag == DJS_TAG_STRING) {
        if (key.tag == DJS_TAG_STRING) {
            if (djs_strcmp(key.as.string->bytes, "length") == 0) {
                return djs_number((double)base.as.string->length);
            }
            long index = string_index(key.as.string->bytes);
            if (index >= 0 && (size_t)index < base.as.string->length) {
                return djs_string_value(djs_string_new(rt, base.as.string->bytes + index, 1));
            }
        } else if (key.tag == DJS_TAG_NUMBER) {
            double number = key.as.number;
            if (number >= 0 && number == (double)(size_t)number &&
                (size_t)number < base.as.string->length) {
                return djs_string_value(djs_string_new(rt, base.as.string->bytes + (size_t)number, 1));
            }
        }
        return djs_object_get_value(rt, rt->string_prototype, key);
    }
    if (base.tag == DJS_TAG_NUMBER) {
        return djs_object_get_value(rt, rt->number_prototype, key);
    }
    if (base.tag == DJS_TAG_BOOL) {
        return djs_object_get_value(rt, rt->boolean_prototype, key);
    }
    return DJS_UNDEFINED;
}

/* The key of a member expression: computed keys are evaluated, plain names
 * are interned strings. */
static djs_value_t member_key(djs_runtime_t *rt, djs_node_t *node) {
    if (node->flags & DJS_NODE_COMPUTED) return eval_node(rt, node->b);
    return djs_string_value(djs_string_new_cstr(rt, node->b->text));
}

/* -------------------------------------------------------------- string build */

typedef struct {
    djs_runtime_t *rt;
    char *data;
    size_t length;
    size_t capacity;
} djs_text_t;

static void text_init(djs_text_t *text, djs_runtime_t *rt) {
    text->rt = rt;
    text->data = NULL;
    text->length = 0;
    text->capacity = 0;
}

static void text_reserve(djs_text_t *text, size_t extra) {
    if (text->length + extra + 1 <= text->capacity) return;
    size_t capacity = text->capacity == 0 ? 64 : text->capacity;
    while (capacity < text->length + extra + 1) capacity *= 2;
    char *data = (char *)djs_arena_alloc(text->rt, capacity);
    if (data == NULL) return;
    if (text->length > 0) djs_memcpy(data, text->data, text->length);
    text->data = data;
    text->capacity = capacity;
}

static void text_append(djs_text_t *text, const char *bytes, size_t length) {
    text_reserve(text, length);
    if (text->data == NULL) return;
    if (length > 0) djs_memcpy(text->data + text->length, bytes, length);
    text->length += length;
    text->data[text->length] = '\0';
}

static void text_append_cstr(djs_text_t *text, const char *bytes) {
    text_append(text, bytes, djs_strlen(bytes));
}

static djs_value_t text_value(djs_text_t *text) {
    if (text->data == NULL) return djs_string_value(djs_string_new_cstr(text->rt, ""));
    return djs_string_value(djs_string_new(text->rt, text->data, text->length));
}

/* ----------------------------------------------------------- function values */

/* Build the JS function object for a function node. */
static djs_object_t *make_function_object(djs_runtime_t *rt, djs_node_t *node,
                                          const char *name, djs_value_t home_object,
                                          djs_value_t super_class,
                                          djs_node_t *class_node) {
    djs_function_def_t *def = (djs_function_def_t *)node->aux;
    if (def == NULL) return NULL;
    const char *function_name = name;
    if (function_name == NULL || function_name[0] == '\0') function_name = def->name;
    djs_object_t *object = djs_function_new(rt, NULL, function_name ? function_name : "", 0);
    if (object == NULL) return NULL;
    djs_function_t *fn = object->function;
    fn->params = node;
    fn->body = def->body;
    fn->closure = scope_env(rt);
    fn->home_object = home_object;
    fn->super_class = super_class;
    fn->class_node = class_node;
    fn->flags = def->flags;
    if (def->expression_body) fn->flags |= DJS_FN_EXPR_BODY;
    if (fn->flags & DJS_FN_ARROW) {
        fn->captured_this = current_this(rt);
        fn->has_captured_this = true;
    }
    int arity = 0;
    for (size_t i = 0; i < node->list_count; i++) {
        if (node->list[i]->kind != DJS_N_REST) arity++;
    }
    fn->arity = arity;
    return object;
}

/* A native function that carries a context pointer in its function record. */
static djs_value_t make_native_function(djs_runtime_t *rt, void *aux,
                                        djs_native_fn native, const char *name,
                                        int arity) {
    djs_object_t *object = djs_function_new(rt, native, name, arity);
    if (object == NULL) return DJS_UNDEFINED;
    object->function->aux = aux;
    return djs_object_value(object);
}

/* ------------------------------------------------------------------- calls */

static void bind_params(djs_runtime_t *rt, djs_function_t *fn, djs_env_t *env,
                        djs_value_t *args, size_t arg_count) {
    size_t index = 0;
    for (size_t i = 0; i < fn->params->list_count; i++) {
        djs_node_t *param = fn->params->list[i];
        if (param->kind == DJS_N_REST) {
            djs_value_t rest = djs_make_array(rt, NULL, 0);
            for (size_t j = index; j < arg_count; j++) {
                djs_array_push(rt, rest.as.object, args[j]);
            }
            destructure(rt, env, param->a, rest, true);
            break;
        }
        djs_value_t value = index < arg_count ? args[index] : DJS_UNDEFINED;
        index++;
        if (param->b != NULL && djs_is_undefined(value)) {
            value = eval_node(rt, param->b);
        }
        destructure(rt, env, param->a, value, true);
    }
}

static djs_value_t invoke_function(djs_runtime_t *rt, djs_function_t *fn,
                                   djs_value_t this_value, djs_value_t *args,
                                   size_t arg_count, djs_value_t new_target,
                                   djs_node_t *pending_fields) {
    if (fn->native != NULL) {
        djs_frame_t frame;
        frame.env = fn->closure;
        frame.saved_env = NULL;
        frame.this_value = this_value;
        frame.new_target = new_target;
        frame.super_class = fn->super_class;
        frame.home_object = fn->home_object.tag == DJS_TAG_OBJECT
                                ? fn->home_object.as.object : NULL;
        frame.pending_fields = NULL;
        frame.function = fn;
        frame.arrow = false;
        frame.previous = rt->frame;
        rt->frame = &frame;
        djs_value_t result = fn->native(rt, this_value, args, arg_count);
        rt->frame = frame.previous;
        return result;
    }

    djs_env_t *env = djs_env_new(rt, fn->closure);
    if (env == NULL) return DJS_UNDEFINED;
    env->function_scope = true;

    djs_frame_t frame;
    frame.env = env;
    frame.saved_env = NULL;
    frame.this_value = (fn->flags & DJS_FN_ARROW) && fn->has_captured_this
                           ? fn->captured_this : this_value;
    frame.new_target = new_target;
    frame.super_class = fn->super_class;
    frame.home_object = fn->home_object.tag == DJS_TAG_OBJECT
                            ? fn->home_object.as.object : NULL;
    frame.pending_fields = pending_fields;
    frame.function = fn;
    frame.arrow = (fn->flags & DJS_FN_ARROW) != 0;
    frame.previous = rt->frame;
    rt->frame = &frame;

    /* the function binds its own name, so it can call itself */
    if (fn->owner != NULL && fn->name.tag == DJS_TAG_STRING &&
        fn->name.as.string->length > 0 && !frame.arrow) {
        djs_env_declare(rt, env, fn->name.as.string->bytes, djs_object_value(fn->owner), false);
    }

    bind_params(rt, fn, env, args, arg_count);

    /* A base class runs its field initialisers before the constructor body. */
    if (frame.pending_fields != NULL) {
        djs_node_t *fields = frame.pending_fields;
        frame.pending_fields = NULL;
        if (frame.this_value.tag == DJS_TAG_OBJECT) {
            initialize_fields(rt, fields, frame.this_value.as.object);
        }
    }

    djs_value_t result = DJS_UNDEFINED;
    if (fn->flags & DJS_FN_EXPR_BODY) {
        result = eval_node(rt, fn->body);
    } else {
        djs_completion_t completion = exec_node(rt, fn->body);
        if (completion == DJS_C_RETURN) result = rt->return_value;
    }
    rt->frame = frame.previous;
    return result;
}

djs_value_t djs_call(djs_runtime_t *rt, djs_value_t callee, djs_value_t this_value,
                     djs_value_t *args, size_t arg_count) {
    if (!djs_is_callable(callee)) djs_throw_type_error(rt, "value is not a function");
    djs_function_t *fn = callee.as.object->function;
    return call_function(rt, fn, this_value, args, arg_count, DJS_UNDEFINED);
}

djs_value_t djs_construct(djs_runtime_t *rt, djs_value_t callee, djs_value_t *args,
                          size_t arg_count) {
    if (!djs_is_callable(callee)) djs_throw_type_error(rt, "value is not a constructor");
    djs_function_t *fn = callee.as.object->function;

    djs_value_t prototype_value = djs_object_get(rt, callee.as.object, "prototype");
    djs_object_t *prototype = rt->object_prototype;
    if (prototype_value.tag == DJS_TAG_OBJECT) prototype = prototype_value.as.object;

    if (fn->native != NULL) {
        djs_object_t *instance = djs_object_new(rt, prototype);
        djs_value_t this_value = djs_object_value(instance);
        djs_frame_t frame;
        frame.env = fn->closure;
        frame.saved_env = NULL;
        frame.this_value = this_value;
        frame.new_target = callee;
        frame.super_class = fn->super_class;
        frame.home_object = NULL;
        frame.pending_fields = NULL;
        frame.function = fn;
        frame.arrow = false;
        frame.previous = rt->frame;
        rt->frame = &frame;
        djs_value_t result = fn->native(rt, this_value, args, arg_count);
        rt->frame = frame.previous;
        return result.tag == DJS_TAG_OBJECT ? result : this_value;
    }

    /* A class constructor builds a fresh instance. */
    djs_object_t *instance = djs_object_new(rt, prototype);
    djs_value_t this_value = djs_object_value(instance);
    djs_node_t *pending = NULL;
    if (fn->class_node != NULL && fn->super_class.tag != DJS_TAG_OBJECT) {
        /* A base class runs its field initialisers before the body. */
        pending = fn->class_node;
    }
    djs_value_t result = invoke_function(rt, fn, this_value, args, arg_count, callee, pending);
    return result.tag == DJS_TAG_OBJECT ? result : this_value;
}

djs_value_t djs_call_method(djs_runtime_t *rt, djs_object_t *object, const char *name,
                            djs_value_t *args, size_t arg_count) {
    djs_value_t base = djs_object_value(object);
    djs_value_t key = djs_string_value(djs_string_new_cstr(rt, name));
    djs_value_t callee = get_member(rt, base, key);
    if (!djs_is_callable(callee)) {
        char message[160];
        size_t length = 0;
        const char *prefix = "not a function: ";
        for (size_t i = 0; prefix[i] != '\0'; i++) message[length++] = prefix[i];
        for (size_t i = 0; name[i] != '\0' && length + 1 < sizeof(message); i++) {
            message[length++] = name[i];
        }
        message[length] = '\0';
        djs_throw_type_error(rt, message);
    }
    return djs_call(rt, callee, base, args, arg_count);
}

djs_value_t djs_call_method_value(djs_runtime_t *rt, djs_value_t base, const char *name,
                                  djs_value_t *args, size_t arg_count) {
    djs_value_t key = djs_string_value(djs_string_new_cstr(rt, name));
    djs_value_t callee = get_member(rt, base, key);
    if (!djs_is_callable(callee)) djs_throw_type_error(rt, "value is not a function");
    return djs_call(rt, callee, base, args, arg_count);
}

bool djs_is_callable(djs_value_t value) {
    return value.tag == DJS_TAG_OBJECT && value.as.object->function != NULL;
}

/* ---------------------------------------------------------------- patterns */

/* Bind a destructuring pattern. `declare` selects between `const a = ...`
 * (declare in env) and `[a] = ...` (assign to what is already there). */
static void destructure(djs_runtime_t *rt, djs_env_t *env, djs_node_t *pattern,
                        djs_value_t value, bool declare) {
    if (pattern == NULL) return;
    switch (pattern->kind) {
        case DJS_N_IDENT: {
            if (declare) {
                djs_env_declare(rt, env, pattern->text, value, true);
            } else {
                djs_env_assign(rt, env, pattern->text, value);
            }
            return;
        }
        case DJS_N_ASSIGN_PATTERN: {
            djs_value_t chosen = value;
            if (djs_is_undefined(value) || djs_is_null(value)) {
                chosen = eval_node(rt, pattern->b);
            }
            destructure(rt, env, pattern->a, chosen, declare);
            return;
        }
        case DJS_N_ARRAY_PATTERN: {
            for (size_t i = 0; i < pattern->list_count; i++) {
                djs_node_t *item = pattern->list[i];
                if (item->kind == DJS_N_EMPTY) continue;
                if (item->kind == DJS_N_REST) {
                    djs_value_t rest = djs_make_array(rt, NULL, 0);
                    size_t remaining = 0;
                    djs_object_t *array = NULL;
                    if (value.tag == DJS_TAG_OBJECT) {
                        array = value.as.object;
                        remaining = djs_array_length(array);
                    }
                    if (remaining > i) {
                        for (size_t index = i; index < remaining; index++) {
                            djs_array_push(rt, rest.as.object, djs_array_get(rt, array, index));
                        }
                    }
                    destructure(rt, env, item->a, rest, declare);
                    break;
                }
                djs_node_t *target = item->kind == DJS_N_PARAM ? item->a : item;
                djs_node_t *fallback = item->kind == DJS_N_PARAM ? item->b : NULL;
                djs_value_t element = DJS_UNDEFINED;
                if (value.tag == DJS_TAG_OBJECT) {
                    element = djs_array_get(rt, value.as.object, i);
                }
                if (fallback != NULL && djs_is_undefined(element)) {
                    element = eval_node(rt, fallback);
                }
                destructure(rt, env, target, element, declare);
            }
            return;
        }
        case DJS_N_OBJECT_PATTERN: {
            for (size_t i = 0; i < pattern->list_count; i++) {
                djs_node_t *property = pattern->list[i];
                if (property->flags & DJS_NODE_SPREAD) {
                    djs_value_t source = eval_node(rt, property->a);
                    if (source.tag == DJS_TAG_OBJECT) {
                        for (djs_property_t *entry = djs_property_first(source.as.object);
                             entry != NULL; entry = entry->next) {
                            if (entry->flags & DJS_PROP_PRIVATE) continue;
                            const char *name = entry->key.tag == DJS_TAG_STRING
                                                   ? entry->key.as.string->bytes : "";
                            djs_env_declare(rt, env, name, entry->value, true);
                        }
                    }
                    continue;
                }
                djs_value_t key;
                if (property->flags & DJS_NODE_COMPUTED) {
                    key = eval_node(rt, property->a);
                } else if (property->a != NULL && property->a->text != NULL) {
                    key = djs_string_value(djs_string_new_cstr(rt, property->a->text));
                } else {
                    continue;
                }
                djs_node_t *target = property->b;
                djs_node_t *fallback = NULL;
                if (target != NULL && target->kind == DJS_N_ASSIGN_PATTERN) {
                    fallback = target->b;
                    target = target->a;
                }
                djs_value_t property_value = DJS_UNDEFINED;
                if (value.tag == DJS_TAG_OBJECT) {
                    property_value = djs_get_prop(rt, value, key);
                }
                if (fallback != NULL && djs_is_undefined(property_value)) {
                    property_value = eval_node(rt, fallback);
                }
                destructure(rt, env, target, property_value, declare);
            }
            return;
        }
        case DJS_N_MEMBER: {
            if (declare) return;
            djs_value_t base = eval_node(rt, pattern->a);
            djs_value_t key = member_key(rt, pattern);
            djs_set_prop(rt, base, key, value);
            return;
        }
        case DJS_N_ARRAY:
        case DJS_N_OBJECT: {
            destructure(rt, env, pattern, value, declare);
            return;
        }
        default:
            return;
    }
}

/* ------------------------------------------------------------ coroutines */

#define DJS_CORO_STACK_SIZE (128 * 1024)

static void coroutine_entry(void *arg);

static djs_coro_t *coroutine_new(djs_runtime_t *rt, djs_function_t *fn,
                                 djs_value_t this_value, djs_value_t new_target,
                                 djs_value_t *args, size_t arg_count) {
    djs_coro_t *coro = (djs_coro_t *)djs_arena_calloc(rt, 1, sizeof(*coro));
    if (coro == NULL) return NULL;
    coro->rt = rt;
    coro->function = fn;
    coro->this_value = this_value;
    coro->new_target = new_target;
    coro->state = DJS_CORO_SUSPENDED;
    if (arg_count > 0) {
        coro->args = (djs_value_t *)djs_arena_alloc(rt, arg_count * sizeof(djs_value_t));
        if (coro->args != NULL) {
            djs_memcpy(coro->args, args, arg_count * sizeof(djs_value_t));
            coro->arg_count = arg_count;
        }
    }
    return coro;
}

static void coroutine_start(djs_coro_t *coro) {
    djs_runtime_t *rt = coro->rt;
    void *stack = djs_host_alloc(DJS_CORO_STACK_SIZE);
    if (stack == NULL) {
        coro->state = DJS_CORO_DONE;
        coro->has_error = true;
        coro->error = djs_make_error(rt, "RangeError", "out of memory in generator");
        return;
    }
    coro->stack = stack;
    coro->stack_size = DJS_CORO_STACK_SIZE;
    coro->started = true;
    coro->state = DJS_CORO_RUNNING;
    coro->host_frame = rt->frame;
    coro->host_catch = rt->catch_frame;
    coro->host_coro = rt->current_coro;
    rt->current_coro = coro;
    void *top = (char *)stack + DJS_CORO_STACK_SIZE;
    djs_coro_enter(&coro->caller, top, coroutine_entry, coro);
    rt->frame = coro->host_frame;
    rt->catch_frame = coro->host_catch;
    rt->current_coro = coro->host_coro;
}

static void coroutine_resume(djs_coro_t *coro) {
    djs_runtime_t *rt = coro->rt;
    coro->state = DJS_CORO_RUNNING;
    coro->host_frame = rt->frame;
    coro->host_catch = rt->catch_frame;
    coro->host_coro = rt->current_coro;
    rt->current_coro = coro;
    rt->frame = coro->own_frame;
    rt->catch_frame = coro->own_catch;
    djs_context_switch(&coro->caller, &coro->ctx);
    rt->frame = coro->host_frame;
    rt->catch_frame = coro->host_catch;
    rt->current_coro = coro->host_coro;
}

/* Switch out of the coroutine back to the host. */
static void coroutine_suspend(djs_coro_t *coro) {
    djs_runtime_t *rt = coro->rt;
    coro->own_frame = rt->frame;
    coro->own_catch = rt->catch_frame;
    rt->frame = coro->host_frame;
    rt->catch_frame = coro->host_catch;
    rt->current_coro = coro->host_coro;
    djs_context_switch(&coro->ctx, &coro->caller);
    rt->current_coro = coro;
    rt->frame = coro->own_frame;
    rt->catch_frame = coro->own_catch;
}

/* Resolve or reject the promise of a finished async coroutine. */
static void coroutine_settle(djs_runtime_t *rt, djs_coro_t *coro) {
    if (!coro->is_async) return;
    if (coro->has_error) {
        djs_value_t error = coro->error;
        coro->has_error = false;
        djs_promise_reject(rt, coro->promise, error);
    } else {
        djs_promise_resolve(rt, coro->promise, coro->result);
    }
}

/* Resume a coroutine from a host task (promise reaction or microtask). */
static djs_value_t coroutine_resume_task(djs_runtime_t *rt, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_coro_t *coro = (djs_coro_t *)rt->frame->function->aux;
    if (coro == NULL) return DJS_UNDEFINED;
    /* The rejection reaction passes a flag before the error. */
    if (arg_count > 1 && args[0].tag == DJS_TAG_BOOL && args[0].as.boolean) {
        coro->has_error = true;
        coro->error = args[1];
    } else {
        coro->has_error = false;
        coro->sent = arg_count > 0 ? args[0] : DJS_UNDEFINED;
    }
    coroutine_resume(coro);
    coroutine_settle(rt, coro);
    return DJS_UNDEFINED;
}

static void coroutine_entry(void *arg) {
    djs_coro_t *coro = (djs_coro_t *)arg;
    djs_runtime_t *rt = coro->rt;
    djs_function_t *fn = coro->function;

    if (coro->env == NULL) {
        coro->env = djs_env_new(rt, fn->closure);
        if (coro->env == NULL) coro->env = rt->global_env;
        else coro->env->function_scope = true;
    }

    djs_frame_t frame;
    frame.env = coro->env;
    frame.saved_env = NULL;
    frame.this_value = (fn->flags & DJS_FN_ARROW) && fn->has_captured_this
                           ? fn->captured_this : coro->this_value;
    frame.new_target = coro->new_target;
    frame.super_class = fn->super_class;
    frame.home_object = fn->home_object.tag == DJS_TAG_OBJECT
                            ? fn->home_object.as.object : NULL;
    frame.pending_fields = NULL;
    frame.function = fn;
    frame.arrow = (fn->flags & DJS_FN_ARROW) != 0;
    frame.previous = rt->frame;
    rt->frame = &frame;

    djs_catch_frame_t frame_catch;
    frame_catch.previous = NULL;
    frame_catch.error = DJS_UNDEFINED;
    rt->catch_frame = &frame_catch;

    volatile int jumped = djs_setjmp(&frame_catch.saved);
    if (jumped == 0) {
        bind_params(rt, fn, coro->env, coro->args, coro->arg_count);
        djs_value_t result = DJS_UNDEFINED;
        if (fn->flags & DJS_FN_EXPR_BODY) {
            result = eval_node(rt, fn->body);
        } else {
            djs_completion_t completion = exec_node(rt, fn->body);
            if (completion == DJS_C_RETURN) result = rt->return_value;
        }
        coro->result = result;
        coro->state = DJS_CORO_DONE;
    } else {
        coro->error = frame_catch.error;
        coro->has_error = true;
        coro->state = DJS_CORO_DONE;
    }
    rt->frame = frame.previous;
    rt->catch_frame = frame_catch.previous;
    rt->current_coro = coro->host_coro;
}

/* -------------------------------------------------------------- generators */

/* The next() native of a generator object. */
static djs_value_t generator_next_fn(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value;
    djs_coro_t *coro = (djs_coro_t *)rt->frame->function->aux;
    djs_value_t sent = arg_count > 0 ? args[0] : DJS_UNDEFINED;
    djs_value_t out = DJS_UNDEFINED;
    bool done = true;
    if (coro != NULL) {
        djs_generator_next(rt, djs_object_value(coro->generator_owner), sent, &out, &done);
    }
    djs_value_t result = djs_make_object(rt);
    djs_object_set(rt, result.as.object, "value", out);
    djs_object_set(rt, result.as.object, "done", djs_bool(done));
    return result;
}

djs_value_t djs_make_generator(djs_runtime_t *rt, djs_function_t *function,
                               djs_env_t *env, djs_value_t this_value) {
    return djs_make_generator_with_args(rt, function, env, this_value, NULL, 0);
}

djs_value_t djs_make_generator_with_args(djs_runtime_t *rt, djs_function_t *function,
                                         djs_env_t *env, djs_value_t this_value,
                                         djs_value_t *args, size_t arg_count) {
    (void)env;
    djs_object_t *object = djs_object_new(rt, rt->object_prototype);
    if (object == NULL) return DJS_UNDEFINED;
    object->class = DJS_CLASS_GENERATOR;
    djs_coro_t *coro = coroutine_new(rt, function, this_value, DJS_UNDEFINED, args, arg_count);
    if (coro == NULL) return DJS_UNDEFINED;
    coro->generator_owner = object;
    object->coroutine = coro;

    djs_value_t next = make_native_function(rt, coro, generator_next_fn, "next", 1);
    if (next.tag == DJS_TAG_OBJECT) {
        djs_property_set(rt, object, "next", next, 0);
    }
    return djs_object_value(object);
}

bool djs_generator_next(djs_runtime_t *rt, djs_value_t generator, djs_value_t sent,
                        djs_value_t *value_out, bool *done_out) {
    if (value_out != NULL) *value_out = DJS_UNDEFINED;
    if (done_out != NULL) *done_out = true;
    if (generator.tag != DJS_TAG_OBJECT) return false;
    djs_coro_t *coro = generator.as.object->coroutine;
    if (coro == NULL) return false;

    coro->sent = sent;
    if (!coro->started) {
        coroutine_start(coro);
    } else if (coro->state == DJS_CORO_RUNNING) {
        coroutine_resume(coro);
    } else {
        return false;
    }
    if (coro->state == DJS_CORO_DONE) {
        if (coro->stack != NULL) {
            djs_host_free(coro->stack);
            coro->stack = NULL;
        }
        if (coro->has_error) {
            djs_value_t error = coro->error;
            coro->has_error = false;
            djs_throw(rt, error);
        }
        if (value_out != NULL) *value_out = coro->result;
        if (done_out != NULL) *done_out = true;
        return false;
    }
    if (value_out != NULL) *value_out = coro->yielded;
    if (done_out != NULL) *done_out = false;
    return true;
}

/* ---------------------------------------------------------------- async */

/* Start an async function: run its body on a coroutine and return the promise
 * that settles when it finishes. */
static djs_value_t start_async(djs_runtime_t *rt, djs_function_t *fn,
                               djs_value_t this_value, djs_value_t *args,
                               size_t arg_count, djs_value_t new_target) {
    djs_value_t promise = djs_promise_new(rt);
    djs_coro_t *coro = coroutine_new(rt, fn, this_value, new_target, args, arg_count);
    if (coro == NULL) return promise;
    coro->is_async = true;
    coro->promise = promise;
    coroutine_start(coro);
    coroutine_settle(rt, coro);
    return promise;
}

/* Await a value inside an async coroutine. */
static djs_value_t coroutine_await(djs_runtime_t *rt, djs_value_t awaited) {
    djs_coro_t *coro = rt->current_coro;
    if (coro == NULL) return awaited;
    if (awaited.tag == DJS_TAG_OBJECT && awaited.as.object->class == DJS_CLASS_PROMISE) {
        djs_value_t on_ok = make_native_function(rt, coro, coroutine_resume_task, "onResolved", 1);
        djs_value_t on_err = make_native_function(rt, coro, coroutine_resume_task, "onRejected", 2);
        djs_promise_then(rt, awaited, on_ok, on_err);
    } else {
        djs_value_t task = make_native_function(rt, coro, coroutine_resume_task, "awaited", 1);
        djs_queue_microtask(rt, task, NULL, 0);
    }
    coroutine_suspend(coro);
    if (coro->has_error) {
        djs_value_t error = coro->error;
        coro->has_error = false;
        djs_throw(rt, error);
    }
    return coro->sent;
}

/* ------------------------------------------------------------------ calls */

static djs_value_t call_function(djs_runtime_t *rt, djs_function_t *fn,
                                 djs_value_t this_value, djs_value_t *args,
                                 size_t arg_count, djs_value_t new_target) {
    if (fn->flags & DJS_FN_GENERATOR) {
        return djs_make_generator_with_args(rt, fn, NULL, this_value, args, arg_count);
    }
    if (fn->flags & DJS_FN_ASYNC) {
        return start_async(rt, fn, this_value, args, arg_count, new_target);
    }
    return invoke_function(rt, fn, this_value, args, arg_count, new_target, NULL);
}

/* ------------------------------------------------------------------ nodes */

static djs_node_t *make_node(djs_runtime_t *rt, int kind) {
    djs_node_t *node = (djs_node_t *)djs_arena_calloc(rt, 1, sizeof(*node));
    if (node != NULL) node->kind = (uint8_t)kind;
    return node;
}

static void node_list_push(djs_runtime_t *rt, djs_node_t *node, djs_node_t *item) {
    if (node->list_count == node->list_capacity) {
        size_t capacity = node->list_capacity == 0 ? 4 : node->list_capacity * 2;
        djs_node_t **grown = (djs_node_t **)djs_arena_alloc(rt, capacity * sizeof(*grown));
        if (grown == NULL) return;
        if (node->list_count > 0) djs_memcpy(grown, node->list, node->list_count * sizeof(*grown));
        node->list = grown;
        node->list_capacity = capacity;
    }
    node->list[node->list_count++] = item;
}

/* Build a default constructor for a class: the parameter is a rest parameter
 * so the arguments reach `super(...args)`. Derived classes insert a super call. */
static djs_node_t *make_default_constructor(djs_runtime_t *rt, bool derived) {
    djs_node_t *fn = make_node(rt, DJS_N_FUNCTION);
    if (fn == NULL) return NULL;
    djs_function_def_t *def = (djs_function_def_t *)djs_arena_calloc(rt, 1, sizeof(*def));
    if (def == NULL) return NULL;
    fn->aux = def;
    def->flags = DJS_FN_CTOR;
    djs_node_t *rest = make_node(rt, DJS_N_REST);
    djs_node_t *args_ident = make_node(rt, DJS_N_IDENT);
    if (args_ident != NULL) args_ident->text = (char *)"args";
    if (rest != NULL) rest->a = args_ident;
    node_list_push(rt, fn, rest);
    djs_node_t *block = make_node(rt, DJS_N_BLOCK);
    if (derived) {
        djs_node_t *call = make_node(rt, DJS_N_CALL);
        call->a = make_node(rt, DJS_N_SUPER);
        djs_node_t *spread = make_node(rt, DJS_N_SPREAD);
        djs_node_t *args_ident2 = make_node(rt, DJS_N_IDENT);
        if (args_ident2 != NULL) args_ident2->text = (char *)"args";
        spread->a = args_ident2;
        node_list_push(rt, call, spread);
        djs_node_t *stmt = make_node(rt, DJS_N_EXPR_STMT);
        stmt->a = call;
        node_list_push(rt, block, stmt);
    }
    def->body = block;
    return fn;
}

/* Define a property, using an accessor pair when the node is a getter or a
 * setter. */
static void define_property(djs_runtime_t *rt, djs_object_t *object, djs_value_t key,
                             djs_value_t value, uint32_t flags, djs_value_t setter) {
    const char *name = key.tag == DJS_TAG_STRING ? key.as.string->bytes : "";
    djs_property_t *property = djs_property_get(object, name);
    if (property == NULL) {
        djs_property_set(rt, object, name, value, flags);
        property = djs_property_get(object, name);
    }
    if (property == NULL) return;
    property->value = value;
    property->value2 = setter;
    property->flags |= flags;
}

/* Run the instance field initialisers of a class body on `instance`. */
static void initialize_fields(djs_runtime_t *rt, djs_node_t *class_node, djs_object_t *instance) {
    for (size_t i = 0; i < class_node->list_count; i++) {
        djs_node_t *member = class_node->list[i];
        if ((member->flags & DJS_NODE_STATIC) != 0) continue;
        if ((member->flags & DJS_NODE_METHOD) != 0) continue;
        djs_value_t value = DJS_UNDEFINED;
        if (member->b != NULL) {
            djs_node_t *body = member->b;
            /* A field with an initialiser is stored as an assignment; only
             * the right hand side is the value of the field. */
            if (body->kind == DJS_N_ASSIGN) body = body->b;
            value = eval_node(rt, body);
        }
        if ((member->flags & DJS_NODE_COMPUTED) != 0) {
            djs_value_t key = eval_node(rt, member->a);
            djs_set_prop(rt, djs_object_value(instance), key, value);
            continue;
        }
        const char *name = member->a->text;
        if (name[0] == '#') {
            djs_property_set(rt, instance, name, value, DJS_PROP_PRIVATE);
        } else {
            djs_object_set(rt, instance, name, value);
        }
    }
}

/* --------------------------------------------------------------- expressions */

static djs_value_t eval_template(djs_runtime_t *rt, djs_node_t *node) {
    djs_text_t text;
    text_init(&text, rt);
    for (size_t i = 0; i < node->list_count; i++) {
        djs_node_t *part = node->list[i];
        if (part->kind == DJS_N_STRING) {
            text_append_cstr(&text, part->text ? part->text : "");
        } else {
            djs_value_t value = eval_node(rt, part);
            djs_value_t as_string = djs_to_string(rt, value);
            if (as_string.tag == DJS_TAG_STRING) {
                text_append(&text, as_string.as.string->bytes, as_string.as.string->length);
            }
        }
    }
    return text_value(&text);
}

static djs_value_t eval_array(djs_runtime_t *rt, djs_node_t *node) {
    djs_object_t *array = djs_array_new(rt);
    if (array == NULL) return DJS_UNDEFINED;
    for (size_t i = 0; i < node->list_count; i++) {
        djs_node_t *item = node->list[i];
        if (item->kind == DJS_N_EMPTY) {
            djs_array_push(rt, array, DJS_UNDEFINED);
        } else if (item->kind == DJS_N_SPREAD) {
            djs_value_t source = eval_node(rt, item->a);
            djs_value_t iterator = djs_get_iterator(rt, source);
            djs_value_t value;
            while (djs_iterator_next(rt, iterator, &value)) {
                djs_array_push(rt, array, value);
            }
        } else {
            djs_array_push(rt, array, eval_node(rt, item));
        }
    }
    return djs_object_value(array);
}

static djs_value_t eval_object(djs_runtime_t *rt, djs_node_t *node) {
    djs_object_t *object = djs_object_new(rt, rt->object_prototype);
    if (object == NULL) return DJS_UNDEFINED;
    for (size_t i = 0; i < node->list_count; i++) {
        djs_node_t *property = node->list[i];
        if ((property->flags & DJS_NODE_SPREAD) != 0) {
            djs_value_t source = eval_node(rt, property->a);
            if (source.tag == DJS_TAG_OBJECT) {
                for (djs_property_t *entry = djs_property_first(source.as.object);
                     entry != NULL; entry = entry->next) {
                    if ((entry->flags & DJS_PROP_PRIVATE) != 0) continue;
                    if (entry->flags & DJS_PROP_GETTER) {
                        djs_value_t getter = entry->value;
                        if (getter.tag == DJS_TAG_OBJECT && getter.as.object->function != NULL) {
                            djs_value_t fn = make_native_function(rt, getter.as.object->function->aux,
                                                                getter.as.object->function->native,
                                                                "get", 0);
                            define_property(rt, object, entry->key, fn, DJS_PROP_GETTER, DJS_UNDEFINED);
                        }
                        continue;
                    }
                    if (entry->flags & DJS_PROP_SETTER) {
                        djs_value_t setter = entry->value2;
                        if (setter.tag == DJS_TAG_OBJECT && setter.as.object->function != NULL) {
                            djs_value_t fn = make_native_function(rt, setter.as.object->function->aux,
                                                                setter.as.object->function->native,
                                                                "set", 0);
                            define_property(rt, object, entry->key, DJS_UNDEFINED, DJS_PROP_SETTER, fn);
                        }
                        continue;
                    }
                    const char *name = entry->key.tag == DJS_TAG_STRING
                                          ? entry->key.as.string->bytes : "";
                    djs_property_set(rt, object, name, entry->value, 0);
                }
            }
            continue;
        }
        djs_value_t key = (property->flags & DJS_NODE_COMPUTED) != 0
                              ? eval_node(rt, property->a)
                              : djs_string_value(djs_string_new_cstr(rt, property->a->text));
        if ((property->flags & DJS_NODE_GETTER) != 0) {
            djs_object_t *method = make_function_object(rt, property->b, NULL, DJS_UNDEFINED,
                                                       DJS_UNDEFINED, NULL);
            if (method != NULL) {
                djs_value_t fn = djs_object_value(method);
                define_property(rt, object, key, fn, DJS_PROP_GETTER, DJS_UNDEFINED);
            }
            continue;
        }
        if ((property->flags & DJS_NODE_SETTER) != 0) {
            djs_object_t *method = make_function_object(rt, property->b, NULL, DJS_UNDEFINED,
                                                       DJS_UNDEFINED, NULL);
            if (method != NULL) {
                djs_value_t fn = djs_object_value(method);
                define_property(rt, object, key, DJS_UNDEFINED, DJS_PROP_SETTER, fn);
            }
            continue;
        }
        djs_value_t value = DJS_UNDEFINED;
        if ((property->flags & DJS_NODE_METHOD) != 0) {
            djs_object_t *method = make_function_object(rt, property->b, property->a->text,
                                                        DJS_UNDEFINED, DJS_UNDEFINED, NULL);
            if (method != NULL) {
                djs_value_t fn = djs_object_value(method);
                fn.as.object->function->flags |= DJS_FN_METHOD;
                define_property(rt, object, key, fn, 0, DJS_UNDEFINED);
            }
            continue;
        }
        if (property->b->kind == DJS_N_ASSIGN_PATTERN) {
            value = eval_node(rt, property->b->a);
            if (djs_is_undefined(value)) value = eval_node(rt, property->b->b);
        } else {
            value = eval_node(rt, property->b);
        }
        djs_set_prop(rt, djs_object_value(object), key, value);
    }
    return djs_object_value(object);
}

static djs_value_t eval_function(djs_runtime_t *rt, djs_node_t *node) {
    djs_object_t *object = make_function_object(rt, node, NULL, DJS_UNDEFINED, DJS_UNDEFINED, NULL);
    if (object == NULL) return DJS_UNDEFINED;
    return djs_object_value(object);
}

static djs_value_t eval_class(djs_runtime_t *rt, djs_node_t *node, bool declaration) {
    djs_class_def_t *def = (djs_class_def_t *)node->aux;
    if (def == NULL) return DJS_UNDEFINED;
    djs_value_t super_class = DJS_UNDEFINED;
    djs_object_t *parent_prototype = rt->object_prototype;
    if (def->superclass != NULL) {
        super_class = eval_node(rt, def->superclass);
        if (!djs_is_callable(super_class)) {
            djs_throw_type_error(rt, "class extends value is not a constructor");
        }
        djs_value_t prototype = djs_object_get(rt, super_class.as.object, "prototype");
        if (prototype.tag == DJS_TAG_OBJECT) parent_prototype = prototype.as.object;
    }
    djs_object_t *prototype = djs_object_new(rt, parent_prototype);
    if (prototype == NULL) return DJS_UNDEFINED;
    djs_node_t *ctor_node = NULL;
    if (def->constructor != NULL && def->constructor->b != NULL) {
        ctor_node = def->constructor->b;
    } else {
        ctor_node = make_default_constructor(rt, def->superclass != NULL);
    }
    djs_object_t *constructor = make_function_object(rt, ctor_node, def->name,
                                                      djs_object_value(prototype), super_class, node);
    if (constructor == NULL) return DJS_UNDEFINED;
    constructor->function->flags |= DJS_FN_CTOR;
    djs_property_set(rt, constructor, "prototype", djs_object_value(prototype), 0);
    djs_property_set(rt, prototype, "constructor", djs_object_value(constructor), 0);
    /* The class body has a scope of its own: the class name is bound before
     * the members run, and the members close over it. */
    djs_env_t *saved_env = scope_env(rt);
    djs_env_t *class_env = djs_env_new(rt, saved_env);
    if (class_env == NULL) class_env = saved_env;
    rt->frame->env = class_env;
    if (def->name != NULL && def->name[0] != '\0') {
        djs_env_declare(rt, class_env, def->name, djs_object_value(constructor), false);
    }
    for (size_t i = 0; i < node->list_count; i++) {
        djs_node_t *member = node->list[i];
        bool is_static = (member->flags & DJS_NODE_STATIC) != 0;
        djs_object_t *target = is_static ? constructor : prototype;
        if ((member->flags & DJS_NODE_METHOD) != 0) {
            djs_value_t key = (member->flags & DJS_NODE_COMPUTED) != 0
                                  ? eval_node(rt, member->a)
                                  : djs_string_value(djs_string_new_cstr(rt, member->a->text));
            djs_object_t *method = make_function_object(rt, member->b, member->a->text,
                                                        djs_object_value(target), super_class, NULL);
            if (method != NULL) {
                djs_value_t fn = djs_object_value(method);
                fn.as.object->function->flags |= DJS_FN_METHOD;
                if ((member->flags & DJS_NODE_GETTER) != 0) {
                    define_property(rt, target, key, fn, DJS_PROP_GETTER, DJS_UNDEFINED);
                } else if ((member->flags & DJS_NODE_SETTER) != 0) {
                    define_property(rt, target, key, DJS_UNDEFINED, DJS_PROP_SETTER, fn);
                } else {
                    define_property(rt, target, key, fn, 0, DJS_UNDEFINED);
                }
            }
            continue;
        }
        if (!is_static) continue; /* instance fields run at construction time */
        djs_value_t value = DJS_UNDEFINED;
        if (member->b != NULL) {
            value = eval_node(rt, member->b);
        }
        djs_value_t key = (member->flags & DJS_NODE_COMPUTED) != 0
                              ? eval_node(rt, member->a)
                              : djs_string_value(djs_string_new_cstr(rt, member->a->text));
        define_property(rt, constructor, key, value, 0, DJS_UNDEFINED);
    }
    rt->frame->env = saved_env;
    if (declaration && def->name != NULL && def->name[0] != '\0') {
        declare_name(rt, saved_env, def->name, djs_object_value(constructor), false);
    }
    return djs_object_value(constructor);
}

/* ------------------------------------------------------------- member / call / new */

static djs_value_t eval_member(djs_runtime_t *rt, djs_node_t *node) {
    if (node->a->kind == DJS_N_SUPER) {
        djs_object_t *prototype = rt->object_prototype;
        djs_object_t *home = current_home(rt);
        if (home != NULL && home->prototype.tag == DJS_TAG_OBJECT) {
            prototype = home->prototype.as.object;
        } else {
            djs_value_t parent = rt->frame != NULL ? rt->frame->super_class : DJS_UNDEFINED;
            if (parent.tag == DJS_TAG_OBJECT) {
                djs_value_t candidate = djs_object_get(rt, parent.as.object, "prototype");
                if (candidate.tag == DJS_TAG_OBJECT) prototype = candidate.as.object;
            }
        }
        djs_value_t key = member_key(rt, node);
        return djs_object_get_value(rt, prototype, key);
    }
    djs_value_t base = eval_node(rt, node->a);
    if ((node->flags & DJS_NODE_OPTIONAL) != 0 && djs_is_nullish(base)) return DJS_UNDEFINED;
    djs_value_t key = member_key(rt, node);
    return get_member(rt, base, key);
}

static void collect_arguments(djs_runtime_t *rt, djs_node_t *node, djs_value_t **args,
                               size_t *count) {
    size_t cap = node->list_count < 4 ? 4 : node->list_count;
    djs_value_t *values = (djs_value_t *)djs_arena_alloc(rt, cap * sizeof(djs_value_t));
    size_t total = 0;
    for (size_t i = 0; i < node->list_count; i++) {
        djs_node_t *argument = node->list[i];
        djs_value_t value;
        if (argument->kind == DJS_N_SPREAD) {
            value = eval_node(rt, argument->a);
            if (value.tag == DJS_TAG_OBJECT) {
                djs_value_t iterator = djs_get_iterator(rt, value);
                djs_value_t item;
                while (djs_iterator_next(rt, iterator, &item)) {
                    values[total++] = item;
                }
            }
        } else {
            values[total++] = eval_node(rt, argument);
        }
    }
    *args = values;
    *count = total;
}

static djs_value_t eval_call(djs_runtime_t *rt, djs_node_t *node) {
    if (node->a->kind == DJS_N_SUPER) return eval_super_call(rt, node);
    djs_value_t this_value = DJS_UNDEFINED;
    djs_value_t callee;
    djs_node_t *callee_node = node->a;
    if (callee_node->kind == DJS_N_MEMBER) {
        djs_value_t base = eval_node(rt, callee_node->a);
        if ((callee_node->flags & DJS_NODE_OPTIONAL) != 0 && djs_is_nullish(base)) {
            return DJS_UNDEFINED;
        }
        this_value = base;
        callee = get_member(rt, base, member_key(rt, callee_node));
    } else {
        callee = eval_node(rt, callee_node);
    }
    if ((node->flags & DJS_NODE_OPTIONAL) != 0 && djs_is_nullish(callee)) return DJS_UNDEFINED;
    djs_value_t *args = NULL;
    size_t arg_count = 0;
    collect_arguments(rt, node, &args, &arg_count);
    if (!djs_is_callable(callee)) djs_throw_type_error(rt, "callee is not a function");
    djs_function_t *fn = callee.as.object->function;
    return call_function(rt, fn, this_value, args, arg_count, DJS_UNDEFINED);
}

static djs_value_t eval_new(djs_runtime_t *rt, djs_node_t *node) {
    djs_value_t callee = eval_node(rt, node->a);
    if (!djs_is_callable(callee)) djs_throw_type_error(rt, "value is not a constructor");
    djs_value_t *args = NULL;
    size_t arg_count = 0;
    collect_arguments(rt, node, &args, &arg_count);
    return djs_construct(rt, callee, args, arg_count);
}

/* Execute a `super(...args)` call: it runs the parent constructor on the
 * current instance and then initialises the fields of the current class. */
static djs_value_t eval_super_call(djs_runtime_t *rt, djs_node_t *node) {
    djs_frame_t *frame = rt->frame;
    if (frame == NULL) djs_throw_type_error(rt, "super outside a constructor");
    djs_value_t parent = frame->super_class;
    if (parent.tag != DJS_TAG_OBJECT || parent.as.object->function == NULL) {
        djs_throw_type_error(rt, "super constructor is not a constructor");
    }
    djs_value_t *args = NULL;
    size_t arg_count = 0;
    collect_arguments(rt, node, &args, &arg_count);
    djs_value_t this_value = frame->this_value;
    if (this_value.tag == DJS_TAG_OBJECT) {
        djs_value_t prototype = djs_object_get(rt, parent.as.object, "prototype");
        if (prototype.tag == DJS_TAG_OBJECT) this_value.as.object->prototype = prototype;
    }
    djs_function_t *parent_fn = parent.as.object->function;
    djs_value_t result = DJS_UNDEFINED;
    if (parent_fn->native != NULL) {
        result = djs_call(rt, parent, this_value, args, arg_count);
    } else {
        djs_node_t *pending = NULL;
        if (parent_fn->class_node != NULL && parent_fn->super_class.tag != DJS_TAG_OBJECT) {
            pending = parent_fn->class_node;
        }
        result = invoke_function(rt, parent_fn, this_value, args, arg_count, frame->new_target, pending);
    }
    if (frame->function != NULL && frame->function->class_node != NULL && this_value.tag == DJS_TAG_OBJECT) {
        initialize_fields(rt, frame->function->class_node, this_value.as.object);
    }
    return result;
}
/* ------------------------------------------------------------------ unary */

static djs_value_t eval_unary(djs_runtime_t *rt, djs_node_t *node) {
    djs_value_t value = eval_node(rt, node->a);
    if (djs_strcmp(node->text, "typeof") == 0) {
        return djs_string_value(djs_string_new_cstr(rt, djs_typeof(value)));
    }
    if (djs_strcmp(node->text, "void") == 0) return DJS_UNDEFINED;
    if (djs_strcmp(node->text, "delete") == 0) {
        if (node->a->kind == DJS_N_MEMBER) {
            djs_value_t base = eval_node(rt, node->a->a);
            djs_value_t key = member_key(rt, node->a);
            return djs_bool(djs_property_delete(rt, base.tag == DJS_TAG_OBJECT ? base.as.object : NULL,
                                                key.tag == DJS_TAG_STRING ? key.as.string->bytes : ""));
        }
        if (node->a->kind == DJS_N_IDENT) {
            djs_binding_t *binding = djs_env_lookup(scope_env(rt), node->a->text);
            if (binding != NULL) return djs_bool((binding->flags & 1u) == 0);
        }
        return djs_bool(true);
    }
    switch (node->text[0]) {
        case '!': return djs_bool(!djs_to_boolean(value));
        case '~': return djs_number(to_int32(djs_to_number(rt, value).as.number));
        case '+': return djs_to_number(rt, value);
        case '-': return djs_number(-djs_to_number(rt, value).as.number);
    }
    return DJS_UNDEFINED;
}

/* ----------------------------------------------------------------- numbers */

static double to_int32(double value) {
    if (value != value) return 0.0;
    if (value > 2147483647.0 || value < -2147483648.0) {
        /* Wrap the value the way the language does. */
        double wrapped = value - (double)(long long)(value / 4294967296.0) * 4294967296.0;
        if (wrapped > 2147483647.0) wrapped -= 4294967296.0;
        if (wrapped < -2147483648.0) wrapped += 4294967296.0;
        return wrapped;
    }
    return (double)(int32_t)value;
}

static uint32_t to_uint32(double value) {
    if (value != value) return 0u;
    if (value > 4294967295.0 || value < 0.0) {
        double wrapped = value - (double)(long long)(value / 4294967296.0) * 4294967296.0;
        if (wrapped < 0.0) wrapped += 4294967296.0;
        if (wrapped > 4294967295.0) wrapped -= 4294967296.0;
        return (uint32_t)wrapped;
    }
    return (uint32_t)(int64_t)value;
}

/* The remainder of a division, without a libm dependency. */
static double fmod_safe(double a, double b) {
    if (b == 0.0 || a != a || b != b) return 0.0 / 0.0;
    if (a == 1.0 / 0.0 || a == -1.0 / 0.0) return 0.0 / 0.0;
    double sign = 1.0;
    if (a < 0.0) { a = -a; sign = -1.0; }
    if (b < 0.0) b = -b;
    if (a < b) return sign * a;
    if (a <= 9007199254740992.0 && b <= 9007199254740992.0) {
        long long left = (long long)a;
        long long right = (long long)b;
        if (right == 0) return 0.0 / 0.0;
        return sign * (double)(left % right);
    }
    /* Large values: subtract the biggest power-of-two multiple that fits. */
    double step = b;
    while (step <= a * 0.5) step *= 2.0;
    double remainder = a;
    while (step >= b) {
        if (remainder >= step) remainder -= step;
        step *= 0.5;
    }
    return sign * remainder;
}

/* Compare two strings byte by byte; the order matches UTF-16 for ASCII. */
static int djs_string_compare(const char *a, const char *b) {
    size_t length_a = djs_strlen(a);
    size_t length_b = djs_strlen(b);
    size_t shared = length_a < length_b ? length_a : length_b;
    int result = djs_memcmp_helper(a, b, shared);
    if (result != 0) return result;
    if (length_a == length_b) return 0;
    return length_a < length_b ? -1 : 1;
}

/* ----------------------------------------------------------------- binary */

/* Apply a binary operator to two already evaluated operands. */
static djs_value_t apply_binary(djs_runtime_t *rt, const char *op,
                                 djs_value_t left, djs_value_t right) {
    if (djs_strcmp(op, "==") == 0) return djs_bool(djs_loose_equals(rt, left, right));
    if (djs_strcmp(op, "!=") == 0) return djs_bool(!djs_loose_equals(rt, left, right));
    if (djs_strcmp(op, "===") == 0) return djs_bool(djs_strict_equals(left, right));
    if (djs_strcmp(op, "!==") == 0) return djs_bool(!djs_strict_equals(left, right));

    if (djs_strcmp(op, "+") == 0) {
        djs_value_t left_primitive = djs_to_primitive(rt, left, 0);
        djs_value_t right_primitive = djs_to_primitive(rt, right, 0);
        if (left_primitive.tag == DJS_TAG_STRING || right_primitive.tag == DJS_TAG_STRING) {
            djs_text_t text;
            text_init(&text, rt);
            if (left_primitive.tag == DJS_TAG_STRING) {
                text_append(&text, left_primitive.as.string->bytes, left_primitive.as.string->length);
            }
            if (right_primitive.tag == DJS_TAG_STRING) {
                text_append(&text, right_primitive.as.string->bytes, right_primitive.as.string->length);
            }
            return text_value(&text);
        }
        double left_number = djs_to_number(rt, left_primitive).as.number;
        double right_number = djs_to_number(rt, right_primitive).as.number;
        return djs_number(left_number + right_number);
    }

    if (djs_strcmp(op, "instanceof") == 0) {
        if (right.tag != DJS_TAG_OBJECT || right.as.object->function == NULL) return djs_bool(false);
        if (left.tag != DJS_TAG_OBJECT) return djs_bool(false);
        djs_value_t prototype = djs_object_get(rt, right.as.object, "prototype");
        if (prototype.tag != DJS_TAG_OBJECT) return djs_bool(false);
        djs_object_t *cursor = left.as.object;
        while (cursor != NULL) {
            if (cursor == prototype.as.object) return djs_bool(true);
            if (cursor->prototype.tag != DJS_TAG_OBJECT) break;
            cursor = cursor->prototype.as.object;
        }
        return djs_bool(false);
    }

    if (djs_strcmp(op, "in") == 0) {
        if (right.tag != DJS_TAG_OBJECT) return djs_bool(false);
        djs_value_t key = djs_to_string(rt, left);
        if (key.tag != DJS_TAG_STRING) return djs_bool(false);
        for (djs_object_t *cursor = right.as.object; cursor != NULL; ) {
            if (djs_property_get(cursor, key.as.string->bytes) != NULL) return djs_bool(true);
            if (cursor->prototype.tag != DJS_TAG_OBJECT) break;
            cursor = cursor->prototype.as.object;
        }
        return djs_bool(false);
    }

    /* The relational operators compare strings when both sides are strings. */
    if (djs_strcmp(op, "<") == 0 || djs_strcmp(op, ">") == 0 ||
        djs_strcmp(op, "<=") == 0 || djs_strcmp(op, ">=") == 0) {
        djs_value_t left_primitive = djs_to_primitive(rt, left, 1);
        djs_value_t right_primitive = djs_to_primitive(rt, right, 1);
        if (left_primitive.tag == DJS_TAG_STRING && right_primitive.tag == DJS_TAG_STRING) {
            int order = djs_string_compare(left_primitive.as.string->bytes,
                                           right_primitive.as.string->bytes);
            if (djs_strcmp(op, "<") == 0) return djs_bool(order < 0);
            if (djs_strcmp(op, ">") == 0) return djs_bool(order > 0);
            if (djs_strcmp(op, "<=") == 0) return djs_bool(order <= 0);
            return djs_bool(order >= 0);
        }
        double left_number = djs_to_number(rt, left_primitive).as.number;
        double right_number = djs_to_number(rt, right_primitive).as.number;
        if (left_number != left_number || right_number != right_number) return djs_bool(false);
        if (djs_strcmp(op, "<") == 0) return djs_bool(left_number < right_number);
        if (djs_strcmp(op, ">") == 0) return djs_bool(left_number > right_number);
        if (djs_strcmp(op, "<=") == 0) return djs_bool(left_number <= right_number);
        return djs_bool(left_number >= right_number);
    }

    /* The bitwise operators work on 32-bit integers. */
    if (djs_strcmp(op, "<<") == 0) {
        djs_value_t left_value = djs_to_number(rt, left);
        djs_value_t right_value = djs_to_number(rt, right);
        int32_t left = (int32_t)to_int32(left_value.as.number);
        uint32_t shift = to_uint32(right_value.as.number) & 31u;
        return djs_number((double)(int32_t)((uint32_t)left << shift));
    }
    if (djs_strcmp(op, ">>") == 0) {
        djs_value_t left_value = djs_to_number(rt, left);
        djs_value_t right_value = djs_to_number(rt, right);
        int32_t left = (int32_t)to_int32(left_value.as.number);
        uint32_t shift = to_uint32(right_value.as.number) & 31u;
        return djs_number((double)(left >> shift));
    }
    if (djs_strcmp(op, ">>>") == 0) {
        djs_value_t left_value = djs_to_number(rt, left);
        djs_value_t right_value = djs_to_number(rt, right);
        uint32_t left = to_uint32(left_value.as.number);
        uint32_t shift = to_uint32(right_value.as.number) & 31u;
        return djs_number((double)(left >> shift));
    }
    if (djs_strcmp(op, "&") == 0) {
        djs_value_t left_value = djs_to_number(rt, left);
        djs_value_t right_value = djs_to_number(rt, right);
        int32_t left = (int32_t)to_int32(left_value.as.number);
        int32_t right = (int32_t)to_int32(right_value.as.number);
        return djs_number((double)(left & right));
    }
    if (djs_strcmp(op, "|") == 0) {
        djs_value_t left_value = djs_to_number(rt, left);
        djs_value_t right_value = djs_to_number(rt, right);
        int32_t left = (int32_t)to_int32(left_value.as.number);
        int32_t right = (int32_t)to_int32(right_value.as.number);
        return djs_number((double)(left | right));
    }
    if (djs_strcmp(op, "^") == 0) {
        djs_value_t left_value = djs_to_number(rt, left);
        djs_value_t right_value = djs_to_number(rt, right);
        int32_t left = (int32_t)to_int32(left_value.as.number);
        int32_t right = (int32_t)to_int32(right_value.as.number);
        return djs_number((double)(left ^ right));
    }

    double left_number = djs_to_number(rt, left).as.number;
    double right_number = djs_to_number(rt, right).as.number;
    if (djs_strcmp(op, "-") == 0) return djs_number(left_number - right_number);
    if (djs_strcmp(op, "*") == 0) return djs_number(left_number * right_number);
    if (djs_strcmp(op, "/") == 0) return djs_number(left_number / right_number);
    if (djs_strcmp(op, "%") == 0) return djs_number(fmod_safe(left_number, right_number));
    if (djs_strcmp(op, "**") == 0) return djs_number(djs_math_pow(left_number, right_number));
    return DJS_UNDEFINED;
}

static djs_value_t eval_binary(djs_runtime_t *rt, djs_node_t *node) {
    djs_value_t left = eval_node(rt, node->a);
    djs_value_t right = eval_node(rt, node->b);
    return apply_binary(rt, node->text, left, right);
}

/* --------------------------------------------------------------- logical */

static djs_value_t eval_logical(djs_runtime_t *rt, djs_node_t *node) {
    djs_value_t left = eval_node(rt, node->a);
    if (djs_strcmp(node->text, "&&") == 0) {
        return djs_to_boolean(left) ? eval_node(rt, node->b) : left;
    }
    if (djs_strcmp(node->text, "||") == 0) {
        return djs_to_boolean(left) ? left : eval_node(rt, node->b);
    }
    return djs_is_nullish(left) ? eval_node(rt, node->b) : left;
}

static djs_value_t eval_conditional(djs_runtime_t *rt, djs_node_t *node) {
    djs_value_t test = eval_node(rt, node->a);
    return djs_to_boolean(test) ? eval_node(rt, node->b) : eval_node(rt, node->c);
}

static djs_value_t eval_sequence(djs_runtime_t *rt, djs_node_t *node) {
    djs_value_t value = DJS_UNDEFINED;
    for (size_t i = 0; i < node->list_count; i++) {
        value = eval_node(rt, node->list[i]);
    }
    return value;
}

/* ------------------------------------------------------------ assignment */

/* Store a value in an assignment target. */
static void assign_to(djs_runtime_t *rt, djs_node_t *target, djs_value_t value) {
    if (target == NULL) return;
    switch (target->kind) {
        case DJS_N_IDENT:
            djs_env_assign(rt, scope_env(rt), target->text, value);
            return;
        case DJS_N_MEMBER: {
            djs_value_t base = eval_node(rt, target->a);
            djs_value_t key = member_key(rt, target);
            djs_set_prop(rt, base, key, value);
            return;
        }
        case DJS_N_ARRAY_PATTERN:
        case DJS_N_OBJECT_PATTERN:
        case DJS_N_ARRAY:
        case DJS_N_OBJECT:
            destructure(rt, scope_env(rt), target, value, false);
            return;
        default:
            djs_throw_error(rt, "SyntaxError", "invalid assignment target");
    }
}

static djs_value_t eval_assign(djs_runtime_t *rt, djs_node_t *node) {
    const char *op = node->text;
    if (djs_strcmp(op, "=") == 0) {
        djs_value_t value = eval_node(rt, node->b);
        assign_to(rt, node->a, value);
        return value;
    }
    if (djs_strcmp(op, "&&=") == 0 || djs_strcmp(op, "||=") == 0 ||
        djs_strcmp(op, "?\?=") == 0) {
        djs_value_t current = eval_node(rt, node->a);
        bool assign;
        if (djs_strcmp(op, "&&=") == 0) {
            assign = djs_to_boolean(current);
        } else if (djs_strcmp(op, "||=") == 0) {
            assign = !djs_to_boolean(current);
        } else {
            assign = djs_is_nullish(current);
        }
        if (!assign) return current;
        djs_value_t value = eval_node(rt, node->b);
        assign_to(rt, node->a, value);
        return value;
    }
    /* A compound assignment: the operator without the trailing '='. */
    djs_value_t current = eval_node(rt, node->a);
    djs_value_t right = eval_node(rt, node->b);
    char binary_op[4];
    size_t length = 0;
    while (op[length] != '\0' && op[length] != '=' && length + 1 < sizeof(binary_op)) {
        binary_op[length] = op[length];
        length++;
    }
    binary_op[length] = '\0';
    djs_value_t value = apply_binary(rt, binary_op, current, right);
    assign_to(rt, node->a, value);
    return value;
}

static djs_value_t eval_update(djs_runtime_t *rt, djs_node_t *node) {
    djs_value_t old_value = eval_node(rt, node->a);
    double number = djs_to_number(rt, old_value).as.number;
    double updated = node->text[0] == '+' ? number + 1.0 : number - 1.0;
    djs_value_t value = djs_number(updated);
    assign_to(rt, node->a, value);
    return (node->flags & DJS_NODE_PREFIX) != 0 ? value : old_value;
}

/* ------------------------------------------------------------ yield, await */

static djs_value_t eval_yield(djs_runtime_t *rt, djs_node_t *node) {
    djs_coro_t *coro = rt->current_coro;
    if (coro == NULL) djs_throw_error(rt, "SyntaxError", "yield outside a generator");
    djs_value_t value = node->a != NULL ? eval_node(rt, node->a) : DJS_UNDEFINED;
    if ((node->flags & DJS_NODE_DELEGATE) != 0) {
        djs_value_t iterator = djs_get_iterator(rt, value);
        djs_value_t item;
        while (djs_iterator_next(rt, iterator, &item)) {
            coro->yielded = item;
            coro->sent = DJS_UNDEFINED;
            coroutine_suspend(coro);
            if (coro->has_error) {
                djs_value_t error = coro->error;
                coro->has_error = false;
                djs_throw(rt, error);
            }
        }
        return coro->sent;
    }
    coro->yielded = value;
    coro->sent = DJS_UNDEFINED;
    coroutine_suspend(coro);
    if (coro->has_error) {
        djs_value_t error = coro->error;
        coro->has_error = false;
        djs_throw(rt, error);
    }
    return coro->sent;
}

static djs_value_t eval_await(djs_runtime_t *rt, djs_node_t *node) {
    djs_value_t value = eval_node(rt, node->a);
    return coroutine_await(rt, value);
}

/* -------------------------------------------------------- tagged template */

static djs_value_t eval_tagged(djs_runtime_t *rt, djs_node_t *node) {
    djs_value_t tag = eval_node(rt, node->a);
    if (!djs_is_callable(tag)) djs_throw_type_error(rt, "tag is not a function");
    djs_node_t *tmpl = node->b;
    size_t parts = tmpl != NULL ? tmpl->list_count : 0;
    djs_value_t *args = (djs_value_t *)djs_arena_alloc(rt, (parts + 1) * sizeof(djs_value_t));
    if (args == NULL) return DJS_UNDEFINED;
    djs_object_t *strings = djs_array_new(rt);
    size_t count = 1;
    args[0] = djs_object_value(strings);
    for (size_t i = 0; i < parts; i++) {
        djs_node_t *part = tmpl->list[i];
        if (part->kind == DJS_N_STRING) {
            djs_array_push(rt, strings, djs_string_value(djs_string_new_cstr(rt, part->text)));
            continue;
        }
        args[count++] = eval_node(rt, part);
    }
    return call_function(rt, tag.as.object->function, DJS_UNDEFINED, args, count, DJS_UNDEFINED);
}

/* ------------------------------------------------------------- dispatcher */

static djs_value_t eval_node(djs_runtime_t *rt, djs_node_t *node) {
    if (node == NULL) return DJS_UNDEFINED;
    switch (node->kind) {
        case DJS_N_NUMBER: return djs_number(node->number);
        case DJS_N_STRING:
            return djs_string_value(djs_string_new_cstr(rt, node->text ? node->text : ""));
        case DJS_N_BOOLEAN: return djs_bool(node->number != 0.0);
        case DJS_N_NULL: return DJS_NULL;
        case DJS_N_UNDEFINED: return DJS_UNDEFINED;
        case DJS_N_IDENT: {
            djs_value_t value;
            if (!lookup_identifier(rt, node->text, &value)) {
                djs_throw_error(rt, "ReferenceError", node->text);
            }
            return value;
        }
        case DJS_N_THIS: return current_this(rt);
        case DJS_N_NEWTARGET:
            return rt->frame != NULL ? rt->frame->new_target : DJS_UNDEFINED;
        case DJS_N_TEMPLATE: return eval_template(rt, node);
        case DJS_N_ARRAY: return eval_array(rt, node);
        case DJS_N_OBJECT: return eval_object(rt, node);
        case DJS_N_FUNCTION: return eval_function(rt, node);
        case DJS_N_CLASS: return eval_class(rt, node, false);
        case DJS_N_CALL: return eval_call(rt, node);
        case DJS_N_NEW: return eval_new(rt, node);
        case DJS_N_MEMBER: return eval_member(rt, node);
        case DJS_N_UNARY: return eval_unary(rt, node);
        case DJS_N_BINARY: return eval_binary(rt, node);
        case DJS_N_LOGICAL: return eval_logical(rt, node);
        case DJS_N_ASSIGN: return eval_assign(rt, node);
        case DJS_N_CONDITIONAL: return eval_conditional(rt, node);
        case DJS_N_UPDATE: return eval_update(rt, node);
        case DJS_N_SEQUENCE: return eval_sequence(rt, node);
        case DJS_N_YIELD: return eval_yield(rt, node);
        case DJS_N_AWAIT: return eval_await(rt, node);
        case DJS_N_TAGGED: return eval_tagged(rt, node);
        case DJS_N_SPREAD: return eval_node(rt, node->a);
        case DJS_N_REGEXP:
            djs_throw_error(rt, "SyntaxError", "regular expressions are not supported");
        case DJS_N_SUPER:
            djs_throw_type_error(rt, "super outside a constructor");
        default:
            return DJS_UNDEFINED;
    }
}
/* ------------------------------------------------------------- statements */

/* Declare a name in a scope; the global object gets the property as well. */
static void declare_name(djs_runtime_t *rt, djs_env_t *env, const char *name,
                         djs_value_t value, bool mutable_binding) {
    if (name == NULL) return;
    djs_env_declare(rt, env, name, value, mutable_binding);
    if (env == rt->global_env && rt->global != NULL) {
        djs_object_set(rt, rt->global, name, value);
    }
}

/* Function and var declarations are visible before the statement runs. */
static void hoist_statements(djs_runtime_t *rt, djs_node_t **list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        djs_node_t *statement = list[i];
        if (statement == NULL) continue;
        if (statement->kind == DJS_N_FUNCTION) {
            djs_function_def_t *def = (djs_function_def_t *)statement->aux;
            if (def == NULL || def->name == NULL) continue;
            djs_object_t *object = make_function_object(rt, statement, def->name,
                                                       DJS_UNDEFINED, DJS_UNDEFINED, NULL);
            if (object != NULL) {
                declare_name(rt, scope_env(rt), def->name, djs_object_value(object), true);
            }
            continue;
        }
        if (statement->kind == DJS_N_VAR &&
            (statement->flags & (DJS_NODE_LET | DJS_NODE_CONST)) == 0) {
            djs_env_t *env = var_scope(rt);
            for (size_t j = 0; j < statement->list_count; j++) {
                djs_node_t *declarator = statement->list[j];
                if (declarator->a != NULL && declarator->a->kind == DJS_N_IDENT) {
                    declare_name(rt, env, declarator->a->text, DJS_UNDEFINED, true);
                }
            }
        }
    }
}

static djs_completion_t exec_statements(djs_runtime_t *rt, djs_node_t **list,
                                        size_t count) {
    for (size_t i = 0; i < count; i++) {
        djs_completion_t completion = exec_node(rt, list[i]);
        if (completion != DJS_C_NORMAL) return completion;
    }
    return DJS_C_NORMAL;
}

/* The outcome of running a loop body. */
typedef enum {
    LOOP_NEXT = 0,      /* the body finished: run the next iteration */
    LOOP_STOP,          /* a break was consumed: leave the loop */
    LOOP_PROPAGATE      /* return, or a break for another label */
} loop_result_t;

static loop_result_t run_loop_body(djs_runtime_t *rt, djs_node_t *loop,
                                   djs_node_t *body, djs_completion_t *completion_out) {
    djs_completion_t completion = exec_node(rt, body);
    if (completion_out != NULL) *completion_out = completion;
    if (completion == DJS_C_BREAK) {
        if (rt->break_label == NULL) return LOOP_PROPAGATE;
        if (rt->break_label[0] == '\0') {
            rt->break_label = NULL;
            return LOOP_STOP;
        }
        if (loop != NULL && loop->text != NULL &&
            djs_strcmp(rt->break_label, loop->text) == 0) {
            rt->break_label = NULL;
            return LOOP_STOP;
        }
        return LOOP_PROPAGATE;
    }
    if (completion == DJS_C_CONTINUE) {
        if (rt->continue_label == NULL) return LOOP_PROPAGATE;
        if (rt->continue_label[0] == '\0') {
            rt->continue_label = NULL;
            return LOOP_NEXT;
        }
        if (loop != NULL && loop->text != NULL &&
            djs_strcmp(rt->continue_label, loop->text) == 0) {
            rt->continue_label = NULL;
            return LOOP_NEXT;
        }
        return LOOP_PROPAGATE;
    }
    if (completion == DJS_C_RETURN) return LOOP_PROPAGATE;
    return LOOP_NEXT;
}

/* The enumerable keys of an object, including the inherited ones. */
static size_t for_in_keys(djs_runtime_t *rt, djs_object_t *object, char ***out) {
    size_t capacity = 8;
    size_t count = 0;
    char **keys = (char **)djs_arena_alloc(rt, capacity * sizeof(char *));
    if (keys == NULL) { *out = NULL; return 0; }
    for (djs_object_t *current = object; current != NULL; ) {
        if (current->class == DJS_CLASS_ARRAY) {
            for (size_t i = 0; i < current->element_count; i++) {
                char buffer[32];
                djs_number_to_string((double)i, buffer, sizeof(buffer));
                bool seen = false;
                for (size_t j = 0; j < count; j++) {
                    if (djs_strcmp(keys[j], buffer) == 0) { seen = true; break; }
                }
                if (seen) continue;
                if (count == capacity) {
                    size_t grown_capacity = capacity * 2;
                    char **grown = (char **)djs_arena_alloc(rt, grown_capacity * sizeof(char *));
                    if (grown == NULL) break;
                    djs_memcpy(grown, keys, count * sizeof(char *));
                    keys = grown;
                    capacity = grown_capacity;
                }
                keys[count++] = djs_arena_strdup(rt, buffer);
            }
        }
        for (djs_property_t *property = djs_property_first(current); property != NULL;
             property = property->next) {
            if ((property->flags & DJS_PROP_ENUMERABLE) == 0) continue;
            if ((property->flags & DJS_PROP_PRIVATE) != 0) continue;
            if (property->key.tag != DJS_TAG_STRING) continue;
            const char *name = property->key.as.string->bytes;
            bool seen = false;
            for (size_t j = 0; j < count; j++) {
                if (djs_strcmp(keys[j], name) == 0) { seen = true; break; }
            }
            if (seen) continue;
            if (count == capacity) {
                size_t grown_capacity = capacity * 2;
                char **grown = (char **)djs_arena_alloc(rt, grown_capacity * sizeof(char *));
                if (grown == NULL) break;
                djs_memcpy(grown, keys, count * sizeof(char *));
                keys = grown;
                capacity = grown_capacity;
            }
            keys[count++] = djs_arena_strdup(rt, name);
        }
        if (current->prototype.tag != DJS_TAG_OBJECT) break;
        current = current->prototype.as.object;
    }
    *out = keys;
    return count;
}

/* The target of a for-in / for-of loop: the binding and how to store it. */
typedef struct {
    djs_node_t *pattern;
    djs_env_t *env;
    bool declare;
} loop_target_t;

static loop_target_t loop_target(djs_runtime_t *rt, djs_node_t *node) {
    loop_target_t target;
    target.pattern = node;
    target.env = NULL;
    target.declare = false;
    if (node == NULL) return target;
    if (node->kind == DJS_N_VAR) {
        target.declare = true;
        bool block_scoped = (node->flags & (DJS_NODE_LET | DJS_NODE_CONST)) != 0;
        target.env = block_scoped ? scope_env(rt) : var_scope(rt);
        target.pattern = node->list_count > 0 ? node->list[0]->a : NULL;
    }
    return target;
}

static void loop_target_store(djs_runtime_t *rt, loop_target_t *target,
                              djs_value_t value) {
    if (target->pattern == NULL) return;
    if (target->declare) {
        destructure(rt, target->env, target->pattern, value, true);
        return;
    }
    assign_to(rt, target->pattern, value);
}

static djs_completion_t exec_node(djs_runtime_t *rt, djs_node_t *node) {
    if (node == NULL) return DJS_C_NORMAL;
    switch (node->kind) {
        case DJS_N_PROGRAM: {
            hoist_statements(rt, node->list, node->list_count);
            return exec_statements(rt, node->list, node->list_count);
        }
        case DJS_N_BLOCK: {
            djs_env_t *saved = scope_env(rt);
            djs_env_t *block_env = djs_env_new(rt, saved);
            if (block_env == NULL) return exec_statements(rt, node->list, node->list_count);
            rt->frame->env = block_env;
            hoist_statements(rt, node->list, node->list_count);
            djs_completion_t completion = exec_statements(rt, node->list, node->list_count);
            rt->frame->env = saved;
            return completion;
        }
        case DJS_N_EMPTY:
            return DJS_C_NORMAL;
        case DJS_N_EXPR_STMT:
            rt->last_value = eval_node(rt, node->a);
            return DJS_C_NORMAL;
        case DJS_N_VAR: {
            bool block_scoped = (node->flags & (DJS_NODE_LET | DJS_NODE_CONST)) != 0;
            bool is_const = (node->flags & DJS_NODE_CONST) != 0;
            djs_env_t *env = block_scoped ? scope_env(rt) : var_scope(rt);
            for (size_t i = 0; i < node->list_count; i++) {
                djs_node_t *declarator = node->list[i];
                if (declarator->a == NULL) continue;
                if (declarator->a->kind == DJS_N_IDENT) {
                    declare_name(rt, env, declarator->a->text, DJS_UNDEFINED, !is_const);
                    if (declarator->b != NULL) {
                        djs_value_t value = eval_node(rt, declarator->b);
                        djs_binding_t *binding = djs_env_lookup(env, declarator->a->text);
                        if (binding != NULL) {
                            binding->value = value;
                            binding->initialized = true;
                        }
                        if (env == rt->global_env && rt->global != NULL) {
                            djs_object_set(rt, rt->global, declarator->a->text, value);
                        }
                    }
                    continue;
                }
                djs_value_t value = DJS_UNDEFINED;
                if (declarator->b != NULL) value = eval_node(rt, declarator->b);
                destructure(rt, env, declarator->a, value, true);
            }
            return DJS_C_NORMAL;
        }
        case DJS_N_FUNCTION: {
            djs_function_def_t *def = (djs_function_def_t *)node->aux;
            if (def == NULL) return DJS_C_NORMAL;
            djs_object_t *object = make_function_object(rt, node, def->name, DJS_UNDEFINED,
                                                       DJS_UNDEFINED, NULL);
            if (object != NULL && def->name != NULL) {
                declare_name(rt, scope_env(rt), def->name, djs_object_value(object), true);
            }
            return DJS_C_NORMAL;
        }
        case DJS_N_CLASS: {
            djs_class_def_t *def = (djs_class_def_t *)node->aux;
            djs_value_t value = eval_class(rt, node, true);
            if (def != NULL && def->name != NULL) {
                declare_name(rt, scope_env(rt), def->name, value, false);
            }
            return DJS_C_NORMAL;
        }
        case DJS_N_IF: {
            djs_value_t test = eval_node(rt, node->a);
            if (djs_to_boolean(test)) return exec_node(rt, node->b);
            if (node->c != NULL) return exec_node(rt, node->c);
            return DJS_C_NORMAL;
        }
        case DJS_N_WHILE: {
            while (true) {
                djs_value_t test = eval_node(rt, node->a);
                if (!djs_to_boolean(test)) break;
                djs_completion_t completion = DJS_C_NORMAL;
                loop_result_t result = run_loop_body(rt, node, node->b, &completion);
                if (result == LOOP_STOP) break;
                if (result == LOOP_PROPAGATE) return completion;
            }
            return DJS_C_NORMAL;
        }
        case DJS_N_DO_WHILE: {
            while (true) {
                djs_completion_t completion = DJS_C_NORMAL;
                loop_result_t result = run_loop_body(rt, node, node->a, &completion);
                if (result == LOOP_STOP) break;
                if (result == LOOP_PROPAGATE) return completion;
                djs_value_t test = eval_node(rt, node->b);
                if (!djs_to_boolean(test)) break;
            }
            return DJS_C_NORMAL;
        }
        case DJS_N_FOR: {
            djs_env_t *saved = scope_env(rt);
            djs_env_t *loop_env = djs_env_new(rt, saved);
            if (loop_env != NULL) rt->frame->env = loop_env;
            if (node->a != NULL) {
                if (node->a->kind == DJS_N_VAR) {
                    exec_node(rt, node->a);
                } else {
                    eval_node(rt, node->a);
                }
            }
            while (true) {
                if (node->b != NULL) {
                    djs_value_t test = eval_node(rt, node->b);
                    if (!djs_to_boolean(test)) break;
                }
                djs_completion_t completion = DJS_C_NORMAL;
                loop_result_t result = run_loop_body(rt, node, node->d, &completion);
                if (result == LOOP_STOP) break;
                if (result == LOOP_PROPAGATE) {
                    rt->frame->env = saved;
                    return completion;
                }
                if (node->c != NULL) eval_node(rt, node->c);
            }
            rt->frame->env = saved;
            return DJS_C_NORMAL;
        }
        case DJS_N_FOR_IN: {
            djs_value_t source = eval_node(rt, node->b);
            loop_target_t target = loop_target(rt, node->a);
            char **keys = NULL;
            size_t key_count = 0;
            if (source.tag == DJS_TAG_OBJECT) {
                key_count = for_in_keys(rt, source.as.object, &keys);
            } else if (source.tag == DJS_TAG_STRING) {
                key_count = source.as.string->length;
                keys = (char **)djs_arena_alloc(rt, (key_count + 1) * sizeof(char *));
                if (keys != NULL) {
                    for (size_t i = 0; i < key_count; i++) {
                        char buffer[32];
                        djs_number_to_string((double)i, buffer, sizeof(buffer));
                        keys[i] = djs_arena_strdup(rt, buffer);
                    }
                }
            }
            for (size_t i = 0; i < key_count; i++) {
                loop_target_store(rt, &target, djs_string_value(djs_string_new_cstr(rt, keys[i])));
                djs_completion_t completion = DJS_C_NORMAL;
                loop_result_t result = run_loop_body(rt, node, node->c, &completion);
                if (result == LOOP_STOP) break;
                if (result == LOOP_PROPAGATE) return completion;
            }
            return DJS_C_NORMAL;
        }
        case DJS_N_FOR_OF: {
            djs_value_t source = eval_node(rt, node->b);
            loop_target_t target = loop_target(rt, node->a);
            djs_value_t iterator = djs_get_iterator(rt, source);
            djs_value_t item;
            while (djs_iterator_next(rt, iterator, &item)) {
                loop_target_store(rt, &target, item);
                djs_completion_t completion = DJS_C_NORMAL;
                loop_result_t result = run_loop_body(rt, node, node->c, &completion);
                if (result == LOOP_STOP) break;
                if (result == LOOP_PROPAGATE) return completion;
            }
            return DJS_C_NORMAL;
        }
        case DJS_N_SWITCH: {
            djs_value_t discriminant = eval_node(rt, node->a);
            size_t start = node->list_count;
            size_t default_index = node->list_count;
            for (size_t i = 0; i < node->list_count; i++) {
                djs_node_t *entry = node->list[i];
                if (entry->a == NULL) {
                    default_index = i;
                    continue;
                }
                djs_value_t test = eval_node(rt, entry->a);
                if (djs_strict_equals(discriminant, test)) {
                    start = i;
                    break;
                }
            }
            if (start == node->list_count) start = default_index;
            for (size_t i = start; i < node->list_count; i++) {
                djs_node_t *entry = node->list[i];
                djs_completion_t completion = exec_statements(rt, entry->list, entry->list_count);
                if (completion == DJS_C_BREAK) {
                    if (rt->break_label != NULL && rt->break_label[0] == '\0') {
                        rt->break_label = NULL;
                        return DJS_C_NORMAL;
                    }
                    return completion;
                }
                if (completion != DJS_C_NORMAL) return completion;
            }
            return DJS_C_NORMAL;
        }
        case DJS_N_TRY: {
            djs_frame_t *saved_frame = rt->frame;
            djs_env_t *saved_env = scope_env(rt);
            djs_coro_t *saved_coro = rt->current_coro;
            djs_catch_frame_t frame;
            frame.previous = rt->catch_frame;
            frame.error = DJS_UNDEFINED;
            rt->catch_frame = &frame;
            volatile int jumped = djs_setjmp(&frame.saved);
            if (jumped == 0) {
                djs_completion_t completion = exec_node(rt, node->a);
                rt->catch_frame = frame.previous;
                if (node->d != NULL) {
                    djs_completion_t final = exec_node(rt, node->d);
                    if (final != DJS_C_NORMAL) return final;
                }
                return completion;
            }
            /* An exception was thrown: run the catch block. */
            rt->catch_frame = frame.previous;
            rt->frame = saved_frame;
            rt->current_coro = saved_coro;
            rt->frame->env = saved_env;
            djs_completion_t completion = DJS_C_NORMAL;
            if (node->c != NULL) {
                djs_env_t *catch_env = djs_env_new(rt, saved_env);
                if (catch_env != NULL) rt->frame->env = catch_env;
                if (node->b != NULL) {
                    destructure(rt, catch_env, node->b, frame.error, true);
                }
                completion = exec_node(rt, node->c);
                rt->frame->env = saved_env;
            }
            if (node->d != NULL) {
                djs_completion_t final = exec_node(rt, node->d);
                if (final != DJS_C_NORMAL) return final;
            }
            if (node->c == NULL) djs_throw(rt, frame.error);
            return completion;
        }
        case DJS_N_BREAK:
            rt->break_label = node->text != NULL ? node->text : "";
            return DJS_C_BREAK;
        case DJS_N_CONTINUE:
            rt->continue_label = node->text != NULL ? node->text : "";
            return DJS_C_CONTINUE;
        case DJS_N_RETURN:
            rt->return_value = node->a != NULL ? eval_node(rt, node->a) : DJS_UNDEFINED;
            return DJS_C_RETURN;
        case DJS_N_THROW:
            djs_throw(rt, eval_node(rt, node->a));
        case DJS_N_LABEL: {
            djs_node_t *inner = node->a;
            bool is_loop = inner != NULL &&
                           (inner->kind == DJS_N_FOR || inner->kind == DJS_N_WHILE ||
                            inner->kind == DJS_N_DO_WHILE || inner->kind == DJS_N_FOR_IN ||
                            inner->kind == DJS_N_FOR_OF);
            char *saved_text = is_loop ? inner->text : NULL;
            if (is_loop) inner->text = node->text;
            djs_completion_t completion = exec_node(rt, inner);
            if (is_loop) inner->text = saved_text;
            if (completion == DJS_C_BREAK && rt->break_label != NULL &&
                djs_strcmp(rt->break_label, node->text) == 0) {
                rt->break_label = NULL;
                return DJS_C_NORMAL;
            }
            if (completion == DJS_C_CONTINUE && rt->continue_label != NULL &&
                djs_strcmp(rt->continue_label, node->text) == 0) {
                rt->continue_label = NULL;
                return DJS_C_NORMAL;
            }
            return completion;
        }
        default:
            rt->last_value = eval_node(rt, node);
            return DJS_C_NORMAL;
    }
}

/* ------------------------------------------------------------- public API */

djs_value_t djs_eval_node(djs_runtime_t *rt, djs_node_t *node) {
    return eval_node(rt, node);
}

djs_completion_t djs_exec_node(djs_runtime_t *rt, djs_node_t *node) {
    return exec_node(rt, node);
}

djs_completion_t djs_exec_list(djs_runtime_t *rt, djs_node_t **list, size_t count) {
    return exec_statements(rt, list, count);
}

djs_value_t djs_run_program(djs_runtime_t *rt, djs_node_t *program) {
    if (program == NULL) return DJS_UNDEFINED;
    djs_completion_t completion = exec_node(rt, program);
    if (completion == DJS_C_RETURN) return rt->return_value;
    return rt->last_value;
}

djs_value_t djs_eval_source(djs_runtime_t *rt, const char *source, const char *filename,
                             char **error_out) {
    char *error = NULL;
    djs_node_t *program = djs_parse(rt, source, filename, &error);
    if (program == NULL) {
        if (error_out != NULL) *error_out = error;
        return DJS_UNDEFINED;
    }
    return djs_run_program(rt, program);
}
