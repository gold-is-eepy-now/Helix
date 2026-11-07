/* djs_runtime.c — runtime support, iteration, promises, tasks, timers,
 * host integration and public API entry points for the donejs port.
 */

#define _POSIX_C_SOURCE 200809L
#include "djs_internal.h"

#if defined(DJS_FREESTANDING)
#include "../../userspace/libhelix.h"
#define DJS_HOST_ALLOC(size) helix_alloc(size)
#define DJS_HOST_FREE(ptr)   helix_free(ptr)
#else
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#define DJS_HOST_ALLOC(size) malloc(size)
#define DJS_HOST_FREE(ptr)   free(ptr)
#endif

/* ------------------------------------------------------------------ host alloc */

void *djs_host_alloc(size_t size) {
    return DJS_HOST_ALLOC(size);
}

void djs_host_free(void *ptr) {
    if (ptr != NULL) DJS_HOST_FREE(ptr);
}

/* ------------------------------------------------------------- time & random */

double djs_host_time_ms(void) {
#if defined(DJS_FREESTANDING)
    /* ponytail: no clock syscall in Helix; rdtsc fallback at 2 GHz.
     * Upgrade to native time syscall when kernel exports one. */
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t tsc = ((uint64_t)hi << 32) | (uint64_t)lo;
    return (double)(tsc / 2000000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

uint64_t djs_host_random(void) {
#if defined(DJS_FREESTANDING)
    static uint64_t seed = 0x853c49e6748fea9bULL;
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return seed;
#else
    static uint64_t seed = 0;
    if (seed == 0) seed = (uint64_t)time(NULL) ^ 0x5deece66dULL;
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return seed;
#endif
}

/* ------------------------------------------------------- value convenience */

djs_value_t djs_bool_value(bool value) {
    return djs_bool(value);
}

djs_value_t djs_number_value(double value) {
    return djs_number(value);
}

djs_value_t djs_string_value_cstr(djs_runtime_t *rt, const char *text) {
    return djs_string_value(djs_string_new_cstr(rt, text ? text : ""));
}

bool djs_string_equals_cstr(djs_string_t *a, const char *text) {
    if (a == NULL || text == NULL) return false;
    size_t length = djs_strlen(text);
    if (a->length != length) return false;
    return djs_memcmp_helper(a->bytes, text, length) == 0;
}

const char *djs_value_cstr(djs_runtime_t *rt, djs_value_t value) {
    djs_value_t str = djs_to_string(rt, value);
    if (str.tag == DJS_TAG_STRING && str.as.string != NULL) {
        return str.as.string->bytes;
    }
    return "";
}

bool djs_value_as_cstr(djs_runtime_t *rt, djs_value_t value, const char **out) {
    (void)rt;
    if (value.tag == DJS_TAG_STRING && value.as.string != NULL) {
        *out = value.as.string->bytes;
        return true;
    }
    return false;
}

bool djs_value_is_callable(djs_value_t value) {
    return djs_is_callable(value);
}

bool djs_value_is_array(djs_value_t value) {
    return value.tag == DJS_TAG_OBJECT && value.as.object != NULL &&
           value.as.object->class == DJS_CLASS_ARRAY;
}

bool djs_value_is_function(djs_value_t value) {
    return value.tag == DJS_TAG_OBJECT && value.as.object != NULL &&
           value.as.object->class == DJS_CLASS_FUNCTION;
}

/* ------------------------------------------------------------- properties */

djs_property_t *djs_property_first(djs_object_t *object) {
    return object ? object->first_property : NULL;
}

void djs_property_define(djs_runtime_t *rt, djs_object_t *object, const char *name,
                         djs_value_t value, uint32_t flags) {
    djs_property_set(rt, object, name, value, flags);
}

void djs_define_accessor(djs_runtime_t *rt, djs_object_t *object, const char *name,
                         djs_native_fn getter, djs_native_fn setter) {
    if (object == NULL || name == NULL) return;
    djs_property_t *property = djs_property_get(object, name);
    if (property == NULL) {
        property = (djs_property_t *)djs_arena_calloc(rt, 1, sizeof(*property));
        if (property == NULL) return;
        property->key = djs_string_value(djs_string_new_cstr(rt, name));
        property->flags = DJS_PROP_ENUMERABLE | DJS_PROP_CONFIGURABLE;
        if (object->last_property == NULL) {
            object->first_property = property;
        } else {
            object->last_property->next = property;
        }
        object->last_property = property;
    }
    if (getter != NULL) {
        djs_object_t *get_fn = djs_function_new(rt, getter, name, 0);
        property->value = djs_object_value(get_fn);
        property->flags |= DJS_PROP_GETTER;
    }
    if (setter != NULL) {
        djs_object_t *set_fn = djs_function_new(rt, setter, name, 1);
        property->value2 = djs_object_value(set_fn);
        property->flags |= DJS_PROP_SETTER;
    }
}

size_t djs_object_keys(djs_runtime_t *rt, djs_object_t *object, char ***out) {
    if (object == NULL) {
        if (out) *out = NULL;
        return 0;
    }
    size_t count = 0;
    for (djs_property_t *p = object->first_property; p != NULL; p = p->next) {
        if ((p->flags & DJS_PROP_ENUMERABLE) && p->key.tag == DJS_TAG_STRING) count++;
    }
    char **keys = (char **)djs_arena_alloc(rt, (count + 1) * sizeof(char *));
    if (keys == NULL) {
        if (out) *out = NULL;
        return 0;
    }
    size_t index = 0;
    for (djs_property_t *p = object->first_property; p != NULL; p = p->next) {
        if ((p->flags & DJS_PROP_ENUMERABLE) && p->key.tag == DJS_TAG_STRING) {
            keys[index++] = p->key.as.string->bytes;
        }
    }
    keys[index] = NULL;
    if (out) *out = keys;
    return count;
}

/* -------------------------------------------------------------- iteration */

djs_value_t djs_get_iterator(djs_runtime_t *rt, djs_value_t iterable) {
    if (iterable.tag != DJS_TAG_OBJECT || iterable.as.object == NULL) {
        djs_throw_type_error(rt, "value is not iterable");
    }
    djs_value_t key = djs_string_value(djs_string_new_cstr(rt, DJS_ITERATOR_KEY));
    djs_value_t method = djs_get_prop(rt, iterable, key);
    if (!djs_value_is_callable(method)) {
        djs_throw_type_error(rt, "value is not iterable");
    }
    return djs_call(rt, method, iterable, NULL, 0);
}

bool djs_iterator_next(djs_runtime_t *rt, djs_value_t iterator, djs_value_t *value_out) {
    if (iterator.tag != DJS_TAG_OBJECT || iterator.as.object == NULL) return false;
    djs_value_t next_method = djs_object_get(rt, iterator.as.object, "next");
    if (!djs_value_is_callable(next_method)) return false;
    djs_value_t step = djs_call(rt, next_method, iterator, NULL, 0);
    if (step.tag != DJS_TAG_OBJECT || step.as.object == NULL) return false;
    djs_value_t done = djs_object_get(rt, step.as.object, "done");
    if (djs_to_boolean(done)) return false;
    if (value_out != NULL) {
        *value_out = djs_object_get(rt, step.as.object, "value");
    }
    return true;
}

/* --------------------------------------------------------------- promises */

djs_value_t djs_promise_new(djs_runtime_t *rt) {
    djs_object_t *promise = djs_object_new(rt, rt->promise_prototype);
    if (promise == NULL) return DJS_UNDEFINED;
    promise->class = DJS_CLASS_PROMISE;
    promise->promise_state = DJS_PROMISE_PENDING;
    promise->promise_value = DJS_UNDEFINED;
    promise->promise_reactions = NULL;
    return djs_object_value(promise);
}

static void trigger_reactions(djs_runtime_t *rt, djs_object_t *promise);

void djs_promise_resolve(djs_runtime_t *rt, djs_value_t promise_val, djs_value_t value) {
    if (promise_val.tag != DJS_TAG_OBJECT || promise_val.as.object == NULL) return;
    djs_object_t *promise = promise_val.as.object;
    if (promise->class != DJS_CLASS_PROMISE) return;
    if (promise->promise_state != DJS_PROMISE_PENDING) return;
    if (djs_strict_equals(promise_val, value)) {
        djs_promise_reject(rt, promise_val,
                           djs_make_error(rt, "TypeError", "Chaining cycle detected for promise"));
        return;
    }
    if (value.tag == DJS_TAG_OBJECT && value.as.object != NULL &&
        value.as.object->class == DJS_CLASS_PROMISE) {
        djs_promise_then(rt, value,
                         djs_object_value(djs_function_new(rt, NULL, "resolve", 1)),
                         djs_object_value(djs_function_new(rt, NULL, "reject", 1)));
        /* Direct adoption: queue reaction when resolved */
        promise->promise_state = DJS_PROMISE_FULFILLED;
        promise->promise_value = value.as.object->promise_value;
        trigger_reactions(rt, promise);
        return;
    }
    promise->promise_state = DJS_PROMISE_FULFILLED;
    promise->promise_value = value;
    trigger_reactions(rt, promise);
}

void djs_promise_reject(djs_runtime_t *rt, djs_value_t promise_val, djs_value_t error) {
    if (promise_val.tag != DJS_TAG_OBJECT || promise_val.as.object == NULL) return;
    djs_object_t *promise = promise_val.as.object;
    if (promise->class != DJS_CLASS_PROMISE) return;
    if (promise->promise_state != DJS_PROMISE_PENDING) return;
    promise->promise_state = DJS_PROMISE_REJECTED;
    promise->promise_value = error;
    trigger_reactions(rt, promise);
}

static djs_value_t run_reaction_task(djs_runtime_t *rt, djs_value_t this_value,
                                     djs_value_t *args, size_t arg_count) {
    (void)this_value; (void)args; (void)arg_count;
    djs_task_t *task = (djs_task_t *)rt->frame->function->aux;
    if (task == NULL || task->aux == NULL) return DJS_UNDEFINED;
    djs_reaction_t *r = (djs_reaction_t *)task->aux;
    djs_value_t handler = task->callback;
    djs_value_t input = task->arg_count > 0 ? task->args[0] : DJS_UNDEFINED;
    bool is_err = r->rejection;

    if (!djs_value_is_callable(handler)) {
        if (is_err) {
            djs_promise_reject(rt, r->derived, input);
        } else {
            djs_promise_resolve(rt, r->derived, input);
        }
        return DJS_UNDEFINED;
    }

    djs_catch_frame_t catch_frame;
    catch_frame.previous = rt->catch_frame;
    catch_frame.error = DJS_UNDEFINED;
    rt->catch_frame = &catch_frame;
    if (djs_setjmp(&catch_frame.saved) == 0) {
        djs_value_t res = djs_call(rt, handler, DJS_UNDEFINED, &input, 1);
        rt->catch_frame = catch_frame.previous;
        djs_promise_resolve(rt, r->derived, res);
    } else {
        rt->catch_frame = catch_frame.previous;
        djs_promise_reject(rt, r->derived, catch_frame.error);
    }
    return DJS_UNDEFINED;
}

static void queue_reaction(djs_runtime_t *rt, djs_reaction_t *r, int state, djs_value_t val) {
    if ((state == DJS_PROMISE_FULFILLED && r->rejection) ||
        (state == DJS_PROMISE_REJECTED && !r->rejection)) {
        return;
    }
    djs_value_t handler = r->handler;
    bool is_err = r->rejection;
    djs_value_t *args = (djs_value_t *)djs_arena_alloc(rt, 2 * sizeof(djs_value_t));
    if (args != NULL) {
        args[0] = val;
        args[1] = djs_bool(is_err);
    }
    djs_task_t *task = (djs_task_t *)djs_arena_calloc(rt, 1, sizeof(*task));
    if (task == NULL) return;
    task->callback = handler;
    task->args = args;
    task->arg_count = 2;
    task->aux = r;

    djs_object_t *fn_obj = djs_function_new(rt, run_reaction_task, "reaction", 0);
    if (fn_obj != NULL && fn_obj->function != NULL) {
        fn_obj->function->aux = task;
    }
    djs_queue_promise_job(rt, djs_object_value(fn_obj), args, 2);
}

static void trigger_reactions(djs_runtime_t *rt, djs_object_t *promise) {
    for (djs_reaction_t *r = promise->promise_reactions; r != NULL; r = r->next) {
        queue_reaction(rt, r, promise->promise_state, promise->promise_value);
    }
    promise->promise_reactions = NULL;
}

djs_value_t djs_promise_then(djs_runtime_t *rt, djs_value_t promise_val,
                             djs_value_t on_ok, djs_value_t on_err) {
    if (promise_val.tag != DJS_TAG_OBJECT || promise_val.as.object == NULL) {
        djs_throw_type_error(rt, "Promise.prototype.then called on non-promise");
    }
    djs_object_t *promise = promise_val.as.object;
    djs_value_t derived = djs_promise_new(rt);

    djs_reaction_t *r_ok = (djs_reaction_t *)djs_arena_calloc(rt, 1, sizeof(*r_ok));
    djs_reaction_t *r_err = (djs_reaction_t *)djs_arena_calloc(rt, 1, sizeof(*r_err));
    if (r_ok == NULL || r_err == NULL) return derived;

    r_ok->handler = on_ok;
    r_ok->derived = derived;
    r_ok->rejection = false;

    r_err->handler = on_err;
    r_err->derived = derived;
    r_err->rejection = true;

    r_ok->next = r_err;

    if (promise->promise_state == DJS_PROMISE_PENDING) {
        r_err->next = promise->promise_reactions;
        promise->promise_reactions = r_ok;
    } else {
        queue_reaction(rt, r_ok, promise->promise_state, promise->promise_value);
        queue_reaction(rt, r_err, promise->promise_state, promise->promise_value);
    }
    return derived;
}

/* ------------------------------------------------------------- task queues */

static void task_enqueue(djs_runtime_t *rt, djs_task_t **head, djs_task_t **tail,
                         djs_value_t callback, djs_value_t *args, size_t arg_count) {
    djs_task_t *task = (djs_task_t *)djs_arena_calloc(rt, 1, sizeof(*task));
    if (task == NULL) return;
    task->callback = callback;
    if (arg_count > 0 && args != NULL) {
        task->args = (djs_value_t *)djs_arena_alloc(rt, arg_count * sizeof(djs_value_t));
        if (task->args != NULL) {
            djs_memcpy(task->args, args, arg_count * sizeof(djs_value_t));
            task->arg_count = arg_count;
        }
    }
    if (*tail == NULL) {
        *head = task;
    } else {
        (*tail)->next = task;
    }
    *tail = task;
}

void djs_queue_promise_job(djs_runtime_t *rt, djs_value_t callback, djs_value_t *args,
                           size_t arg_count) {
    task_enqueue(rt, &rt->promise_jobs, &rt->promise_job_tail, callback, args, arg_count);
}

void djs_queue_microtask(djs_runtime_t *rt, djs_value_t callback, djs_value_t *args,
                         size_t arg_count) {
    task_enqueue(rt, &rt->microtasks, &rt->microtask_tail, callback, args, arg_count);
}

void djs_queue_tick(djs_runtime_t *rt, djs_value_t callback, djs_value_t *args,
                    size_t arg_count) {
    task_enqueue(rt, &rt->ticks, &rt->tick_tail, callback, args, arg_count);
}

/* ----------------------------------------------------------------- timers */

uint64_t djs_timer_add(djs_runtime_t *rt, djs_value_t callback, double delay_ms,
                       bool repeating, djs_value_t *args, size_t arg_count) {
    djs_timer_t *timer = (djs_timer_t *)djs_arena_calloc(rt, 1, sizeof(*timer));
    if (timer == NULL) return 0;
    timer->due_ms = rt->timer_now + (delay_ms < 0.0 ? 0.0 : delay_ms);
    timer->interval_ms = delay_ms;
    timer->repeating = repeating;
    timer->callback = callback;
    if (arg_count > 0 && args != NULL) {
        timer->args = (djs_value_t *)djs_arena_alloc(rt, arg_count * sizeof(djs_value_t));
        if (timer->args != NULL) {
            djs_memcpy(timer->args, args, arg_count * sizeof(djs_value_t));
            timer->arg_count = arg_count;
        }
    }
    timer->id = ++rt->next_timer_id;
    timer->next = rt->timers;
    rt->timers = timer;
    return timer->id;
}

void djs_timer_clear(djs_runtime_t *rt, uint64_t id) {
    djs_timer_t **cursor = &rt->timers;
    while (*cursor != NULL) {
        if ((*cursor)->id == id) {
            *cursor = (*cursor)->next;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

/* ------------------------------------------------------------- event loop */

static int run_task_list(djs_runtime_t *rt, djs_task_t **head, djs_task_t **tail) {
    int ran = 0;
    while (*head != NULL) {
        djs_task_t *task = *head;
        *head = task->next;
        if (*head == NULL) *tail = NULL;
        ran++;
        if (djs_value_is_callable(task->callback)) {
            djs_call(rt, task->callback, DJS_UNDEFINED, task->args, task->arg_count);
        }
    }
    return ran;
}

static int drain_microtasks(djs_runtime_t *rt) {
    int count = 0;
    for (int iter = 0; iter < 10000; iter++) {
        int ran = 0;
        ran += run_task_list(rt, &rt->ticks, &rt->tick_tail);
        ran += run_task_list(rt, &rt->promise_jobs, &rt->promise_job_tail);
        ran += run_task_list(rt, &rt->microtasks, &rt->microtask_tail);
        if (ran == 0) break;
        count += ran;
    }
    return count;
}

int djs_run_event_loop(djs_runtime_t *rt) {
    if (rt == NULL) return 0;
    int failures = 0;

    for (;;) {
        drain_microtasks(rt);

        if (rt->timers == NULL) break;

        /* Find the earliest due timer */
        djs_timer_t *earliest = NULL;
        for (djs_timer_t *t = rt->timers; t != NULL; t = t->next) {
            if (earliest == NULL || t->due_ms < earliest->due_ms) earliest = t;
        }
        if (earliest == NULL) break;

        if (earliest->due_ms > rt->timer_now) {
            rt->timer_now = earliest->due_ms;
        }

        /* Fire all timers due at or before timer_now */
        djs_timer_t **cursor = &rt->timers;
        bool fired_any = false;
        while (*cursor != NULL) {
            djs_timer_t *t = *cursor;
            if (t->due_ms <= rt->timer_now) {
                fired_any = true;
                if (!t->repeating) {
                    *cursor = t->next;
                } else {
                    t->due_ms = rt->timer_now + t->interval_ms;
                    cursor = &t->next;
                }
                if (djs_value_is_callable(t->callback)) {
                    djs_call(rt, t->callback, DJS_UNDEFINED, t->args, t->arg_count);
                }
            } else {
                cursor = &t->next;
            }
        }
        if (!fired_any) break;
    }
    drain_microtasks(rt);
    return failures;
}

/* ------------------------------------------------------------- public API */

int djs_eval(djs_runtime_t *rt, const char *source, const char *filename,
             djs_error_t *error) {
    if (rt == NULL || source == NULL) return -1;
    if (filename == NULL) filename = "<eval>";

    djs_catch_frame_t root_catch;
    root_catch.previous = NULL;
    root_catch.error = DJS_UNDEFINED;
    rt->catch_frame = &root_catch;
    djs_root_jmp = &root_catch.saved;
    djs_frame_t *entry_frame = rt->frame;

    volatile int jumped = djs_setjmp(&root_catch.saved);
    if (jumped == 0) {
        char *parse_err = NULL;
        djs_value_t result = djs_eval_source(rt, source, filename, &parse_err);
        (void)result;
        if (parse_err != NULL) {
            if (error != NULL) {
                error->message = parse_err;
                error->length = djs_strlen(parse_err);
            }
            rt->catch_frame = NULL;
            djs_root_jmp = NULL;
            rt->frame = entry_frame;
            return -1;
        }
        rt->catch_frame = NULL;
        djs_root_jmp = NULL;
        rt->frame = entry_frame;
        return 0;
    }

    /* Exception escaped */
    rt->catch_frame = NULL;
    djs_root_jmp = NULL;
    rt->frame = entry_frame;
    if (error != NULL) {
        char buf[256];
        djs_format_error(rt, root_catch.error, buf, sizeof(buf));
        error->message = djs_arena_strdup(rt, buf);
        error->length = djs_strlen(error->message);
    }
    return -1;
}

int djs_run_file(djs_runtime_t *rt, const char *path, char *const *args,
                 size_t arg_count, djs_error_t *error) {
    if (rt == NULL || path == NULL) return -1;

    /* Read the source file */
#if defined(DJS_FREESTANDING)
    int fd = helix_open(path, HELIX_O_RDONLY);
    if (fd < 0) {
        if (error) {
            error->message = "failed to open file";
            error->length = djs_strlen(error->message);
        }
        return -1;
    }
    int64_t size = helix_lseek(fd, 0, HELIX_SEEK_END);
    helix_lseek(fd, 0, HELIX_SEEK_SET);
    if (size < 0) { helix_close(fd); return -1; }
    char *buf = (char *)djs_arena_alloc(rt, (size_t)size + 1);
    if (buf == NULL) { helix_close(fd); return -1; }
    int64_t got = helix_read(fd, buf, (size_t)size);
    helix_close(fd);
    if (got < 0) return -1;
    buf[got] = '\0';
#else
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        if (error) {
            error->message = "failed to open file";
            error->length = djs_strlen(error->message);
        }
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) { fclose(fp); return -1; }
    char *buf = (char *)djs_arena_alloc(rt, (size_t)size + 1);
    if (buf == NULL) { fclose(fp); return -1; }
    size_t read_bytes = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[read_bytes] = '\0';
#endif

    /* Setup process.argv with script args */
    if (rt->global != NULL && djs_object_has(rt->global, "process")) {
        djs_value_t proc = djs_object_get(rt, rt->global, "process");
        if (proc.tag == DJS_TAG_OBJECT && proc.as.object != NULL) {
            djs_value_t argv_arr = djs_make_array(rt, NULL, 0);
            djs_array_push(rt, argv_arr.as.object,
                           djs_string_value(djs_string_new_cstr(rt, rt->exec_path ? rt->exec_path : "donejs")));
            djs_array_push(rt, argv_arr.as.object,
                           djs_string_value(djs_string_new_cstr(rt, path)));
            for (size_t i = 0; i < arg_count; i++) {
                djs_array_push(rt, argv_arr.as.object,
                               djs_string_value(djs_string_new_cstr(rt, args[i])));
            }
            djs_object_set(rt, proc.as.object, "argv", argv_arr);
        }
    }

    int rc = djs_eval(rt, buf, path, error);
    if (rc == 0) {
        djs_run_event_loop(rt);
    }
    return rc;
}

int djs_selftest(djs_runtime_t *rt, djs_write_fn write_fn, void *write_user) {
    if (rt == NULL) return 1;
    djs_write_fn orig_fn = rt->write_fn;
    void *orig_user = rt->write_user;
    if (write_fn != NULL) {
        rt->write_fn = write_fn;
        rt->write_user = write_user;
    }

    int failed = 0;
    const struct { const char *code; const char *desc; } tests[] = {
        { "1 + 2 === 3", "arithmetic" },
        { "'a' + 'b' === 'ab'", "string concatenation" },
        { "typeof 42 === 'number'", "typeof number" },
        { "typeof 'x' === 'string'", "typeof string" },
        { "typeof {} === 'object'", "typeof object" },
        { "Array.isArray([1, 2])", "Array.isArray" },
        { "Object.keys({ a: 1, b: 2 }).length === 2", "Object.keys" },
        { "let s = 0; for (let i = 1; i <= 4; i++) s += i; s === 10", "for loop" },
        { "let f = (x, y) => x * y; f(3, 4) === 12", "arrow function" },
        { "class C { get x() { return 7; } } new C().x === 7", "class getter" },
        { "let p = Promise.resolve(42); let out = 0; p.then(v => { out = v; }); queueMicrotask(() => {}); out === 0", "promise deferral" },
        { "JSON.stringify({ x: 1 }) === '{\"x\":1}'", "JSON.stringify" },
        { "JSON.parse('{\"x\":1}').x === 1", "JSON.parse" },
        { "require('path').join('a', 'b') === 'a/b'", "require path" },
        { "require('assert').equal !== undefined", "require assert" },
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        djs_error_t err;
        int rc = djs_eval(rt, tests[i].code, "<selftest>", &err);
        if (rc == 0 && djs_to_boolean(rt->last_value)) {
            djs_write_cstr(rt, "PASS: ");
            djs_write_cstr(rt, tests[i].desc);
            djs_write_cstr(rt, "\n");
        } else {
            failed++;
            djs_write_cstr(rt, "FAIL: ");
            djs_write_cstr(rt, tests[i].desc);
            djs_write_cstr(rt, " (err: ");
            if (err.message) djs_write(rt, err.message, err.length);
            djs_write_cstr(rt, ")\n");
        }
    }

    rt->write_fn = orig_fn;
    rt->write_user = orig_user;
    return failed;
}
