/* djs_object.c — objects, properties, arrays, environments, errors and the
 * type conversions.
 *
 * Objects carry their own properties in an insertion-ordered list; arrays
 * keep their elements in a separate vector so that element access stays O(1).
 * Property lookup walks the prototype chain; getters and setters are stored
 * as ordinary properties carrying the DJS_PROP_GETTER / DJS_PROP_SETTER flag.
 */

#include "djs_internal.h"

/* ---------------------------------------------------------------- objects */

djs_object_t *djs_object_new(djs_runtime_t *rt, djs_object_t *prototype) {
    djs_object_t *object = (djs_object_t *)djs_arena_calloc(rt, 1, sizeof(*object));
    if (object == NULL) return NULL;
    object->class = DJS_CLASS_OBJECT;
    object->prototype = prototype ? djs_object_value(prototype) : DJS_NULL;
    object->extensible = true;
    object->promise_state = DJS_PROMISE_PENDING;
    return object;
}

djs_object_t *djs_object_create(djs_runtime_t *rt, djs_object_t *prototype) {
    return djs_object_new(rt, prototype);
}

djs_object_t *djs_array_new(djs_runtime_t *rt) {
    djs_object_t *array = djs_object_new(rt, rt->array_prototype);
    if (array == NULL) return NULL;
    array->class = DJS_CLASS_ARRAY;
    return array;
}

djs_object_t *djs_function_new(djs_runtime_t *rt, djs_native_fn native,
                                const char *name, int arity) {
    djs_object_t *object = djs_object_new(rt, rt->function_prototype);
    if (object == NULL) return NULL;
    object->class = DJS_CLASS_FUNCTION;
    djs_function_t *function = (djs_function_t *)djs_arena_calloc(rt, 1, sizeof(*function));
    if (function == NULL) return NULL;
    function->native = native;
    function->arity = arity;
    function->name = djs_string_value(djs_string_new_cstr(rt, name ? name : ""));
    function->owner = object;
    object->function = function;
    return object;
}

/* Parse a canonical array index ("0", "12"). */
static bool array_index(const char *name, size_t *out) {
    if (name == NULL || name[0] == '\0') return false;
    size_t value = 0;
    for (const char *p = name; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') return false;
        value = value * 10 + (size_t)(*p - '0');
        if (value > 0xFFFFFFFFu) return false;
    }
    *out = value;
    return true;
}

djs_property_t *djs_property_get(djs_object_t *object, const char *name) {
    if (object == NULL) return NULL;
    for (djs_property_t *property = object->first_property; property != NULL;
         property = property->next) {
        if (property->key.tag == DJS_TAG_STRING &&
            djs_string_equals_cstr(property->key.as.string, name)) {
            return property;
        }
    }
    return NULL;
}

void djs_property_set(djs_runtime_t *rt, djs_object_t *object, const char *name,
                      djs_value_t value, uint32_t flags) {
    if (object == NULL) return;
    djs_property_t *property = djs_property_get(object, name);
    if (property != NULL) {
        property->value = value;
        property->flags |= flags;
        return;
    }
    property = (djs_property_t *)djs_arena_calloc(rt, 1, sizeof(*property));
    if (property == NULL) return;
    property->key = djs_string_value(djs_string_new_cstr(rt, name));
    property->value = value;
    property->flags = flags | DJS_PROP_ENUMERABLE | DJS_PROP_WRITABLE | DJS_PROP_CONFIGURABLE;
    if (object->last_property == NULL) {
        object->first_property = property;
    } else {
        object->last_property->next = property;
    }
    object->last_property = property;
}

bool djs_property_delete(djs_runtime_t *rt, djs_object_t *object, const char *name) {
    if (object == NULL) return false;
    djs_property_t *previous = NULL;
    for (djs_property_t *property = object->first_property; property != NULL;
         property = property->next) {
        if (property->key.tag == DJS_TAG_STRING &&
            djs_string_equals_cstr(property->key.as.string, name)) {
            if (previous == NULL) {
                object->first_property = property->next;
            } else {
                previous->next = property->next;
            }
            if (object->last_property == property) object->last_property = previous;
            return true;
        }
        previous = property;
    }
    return false;
}

void djs_define_method(djs_runtime_t *rt, djs_object_t *object, const char *name,
                       djs_native_fn fn, int arity) {
    if (object == NULL) return;
    djs_object_t *function = djs_function_new(rt, fn, name, arity);
    if (function == NULL) return;
    djs_property_set(rt, object, name, djs_object_value(function), 0);
}

void djs_define_value(djs_runtime_t *rt, djs_object_t *object, const char *name,
                      djs_value_t value) {
    djs_property_set(rt, object, name, value, 0);
}

/* ----------------------------------------------------------------- arrays */

size_t djs_array_length(djs_object_t *array) {
    return array == NULL ? 0 : array->element_count;
}

djs_value_t djs_array_get(djs_runtime_t *rt, djs_object_t *array, size_t index) {
    (void)rt;
    if (array == NULL) return DJS_UNDEFINED;
    if (index < array->element_count) return array->elements[index];
    return DJS_UNDEFINED;
}

void djs_array_set_length(djs_runtime_t *rt, djs_object_t *array, size_t length) {
    if (array == NULL) return;
    if (length < array->element_count) {
        for (size_t i = length; i < array->element_count; i++) array->elements[i] = DJS_UNDEFINED;
    }
    array->element_count = length;
}

void djs_array_set(djs_runtime_t *rt, djs_object_t *array, size_t index, djs_value_t value) {
    if (array == NULL) return;
    if (index >= array->element_capacity) {
        size_t capacity = array->element_capacity == 0 ? 8 : array->element_capacity;
        while (capacity <= index) capacity *= 2;
        djs_value_t *elements = (djs_value_t *)djs_arena_alloc(rt, capacity * sizeof(*elements));
        if (elements == NULL) return;
        if (array->element_count > 0) {
            djs_memcpy(elements, array->elements, array->element_count * sizeof(*elements));
        }
        for (size_t i = array->element_count; i < capacity; i++) elements[i] = DJS_UNDEFINED;
        array->elements = elements;
        array->element_capacity = capacity;
    }
    if (index >= array->element_count) {
        for (size_t i = array->element_count; i < index; i++) array->elements[i] = DJS_UNDEFINED;
        array->element_count = index + 1;
    }
    array->elements[index] = value;
}

void djs_array_push(djs_runtime_t *rt, djs_object_t *array, djs_value_t value) {
    djs_array_set(rt, array, djs_array_length(array), value);
}

djs_value_t djs_make_array(djs_runtime_t *rt, djs_value_t *items, size_t count) {
    djs_object_t *array = djs_array_new(rt);
    if (array == NULL) return DJS_UNDEFINED;
    for (size_t i = 0; i < count; i++) djs_array_set(rt, array, i, items[i]);
    return djs_object_value(array);
}

djs_value_t djs_make_object(djs_runtime_t *rt) {
    djs_object_t *object = djs_object_new(rt, rt->object_prototype);
    if (object == NULL) return DJS_UNDEFINED;
    return djs_object_value(object);
}

/* --------------------------------------------------------------- accessors */

djs_value_t djs_object_get_value(djs_runtime_t *rt, djs_object_t *object, djs_value_t key) {
    if (object == NULL) return DJS_UNDEFINED;
    if (key.tag == DJS_TAG_STRING) {
        const char *name = key.as.string->bytes;
        if (object->class == DJS_CLASS_ARRAY) {
            size_t index;
            if (djs_strcmp(name, "length") == 0) {
                return djs_number((double)object->element_count);
            }
            if (array_index(name, &index)) {
                if (index < object->element_count) return object->elements[index];
                return DJS_UNDEFINED;
            }
        }
        djs_property_t *property = djs_property_get(object, name);
        if (property != NULL) {
            if (property->flags & DJS_PROP_GETTER) {
                if (property->value.tag == DJS_TAG_OBJECT &&
                    property->value.as.object->function != NULL) {
                    return djs_call(rt, property->value, djs_object_value(object), NULL, 0);
                }
                return DJS_UNDEFINED;
            }
            return property->value;
        }
        if (object->prototype.tag == DJS_TAG_OBJECT) {
            return djs_object_get_value(rt, object->prototype.as.object, key);
        }
        return DJS_UNDEFINED;
    }
    if (key.tag == DJS_TAG_NUMBER) {
        double number = key.as.number;
        if (object->class == DJS_CLASS_ARRAY && number >= 0 &&
            number == (double)(size_t)number) {
            return djs_array_get(rt, object, (size_t)number);
        }
        return DJS_UNDEFINED;
    }
    return DJS_UNDEFINED;
}

djs_value_t djs_object_get(djs_runtime_t *rt, djs_object_t *object, const char *name) {
    return djs_object_get_value(rt, object, djs_string_value(djs_string_new_cstr(rt, name)));
}

void djs_object_set(djs_runtime_t *rt, djs_object_t *object, const char *name,
                    djs_value_t value) {
    if (object == NULL) return;
    if (object->class == DJS_CLASS_ARRAY) {
        size_t index;
        if (djs_strcmp(name, "length") == 0) {
            double number = value.tag == DJS_TAG_NUMBER ? value.as.number : 0.0;
            if (number >= 0 && number == (double)(size_t)number) {
                djs_array_set_length(rt, object, (size_t)number);
            }
            return;
        }
        if (array_index(name, &index)) {
            djs_array_set(rt, object, index, value);
            return;
        }
    }
    djs_property_t *property = djs_property_get(object, name);
    if (property != NULL && (property->flags & DJS_PROP_SETTER)) {
        if (property->value2.tag == DJS_TAG_OBJECT &&
            property->value2.as.object->function != NULL) {
            djs_value_t argument = value;
            djs_call(rt, property->value2, djs_object_value(object), &argument, 1);
        }
        return;
    }
    if (property != NULL && (property->flags & DJS_PROP_GETTER)) {
        return; /* a getter without a setter ignores writes */
    }
    djs_property_set(rt, object, name, value, 0);
}

bool djs_object_has(djs_object_t *object, const char *name) {
    return djs_property_get(object, name) != NULL;
}

/* Property access used by the interpreter: the key may be a string or a
 * number. */
djs_value_t djs_get_prop(djs_runtime_t *rt, djs_value_t base, djs_value_t key) {
    if (base.tag != DJS_TAG_OBJECT) {
        djs_throw_type_error(rt, "cannot read properties of a non-object value");
    }
    if (key.tag == DJS_TAG_NUMBER) {
        djs_value_t name = djs_to_string(rt, key);
        return djs_object_get_value(rt, base.as.object, name);
    }
    if (key.tag == DJS_TAG_STRING) {
        return djs_object_get_value(rt, base.as.object, key);
    }
    djs_value_t name = djs_to_string(rt, key);
    return djs_object_get_value(rt, base.as.object, name);
}

void djs_set_prop(djs_runtime_t *rt, djs_value_t base, djs_value_t key, djs_value_t value) {
    if (base.tag != DJS_TAG_OBJECT) {
        djs_throw_type_error(rt, "cannot set properties of a non-object value");
    }
    djs_value_t name = djs_to_string(rt, key);
    djs_object_set(rt, base.as.object, name.as.string->bytes, value);
}

/* ----------------------------------------------------------- environments */

djs_env_t *djs_env_new(djs_runtime_t *rt, djs_env_t *parent) {
    djs_env_t *env = (djs_env_t *)djs_arena_calloc(rt, 1, sizeof(*env));
    if (env == NULL) return NULL;
    env->parent = parent;
    return env;
}

djs_binding_t *djs_env_lookup(djs_env_t *env, const char *name) {
    if (env == NULL || name == NULL) return NULL;
    for (djs_env_t *scope = env; scope != NULL; scope = scope->parent) {
        for (size_t i = 0; i < scope->count; i++) {
            if (scope->bindings[i].name != NULL &&
                djs_strcmp(scope->bindings[i].name, name) == 0) {
                return &scope->bindings[i];
            }
        }
    }
    return NULL;
}

djs_binding_t *djs_env_declare(djs_runtime_t *rt, djs_env_t *env, const char *name,
                               djs_value_t value, bool mutable_binding) {
    if (env == NULL) return NULL;
    for (size_t i = 0; i < env->count; i++) {
        if (djs_strcmp(env->bindings[i].name, name) == 0) {
            env->bindings[i].value = value;
            env->bindings[i].initialized = true;
            if (mutable_binding) env->bindings[i].flags |= 1u;
            return &env->bindings[i];
        }
    }
    if (env->count == env->capacity) {
        size_t capacity = env->capacity == 0 ? 8 : env->capacity * 2;
        djs_binding_t *bindings = (djs_binding_t *)djs_arena_alloc(rt, capacity * sizeof(*bindings));
        if (bindings == NULL) return NULL;
        if (env->count > 0) {
            djs_memcpy(bindings, env->bindings, env->count * sizeof(*bindings));
        }
        env->bindings = bindings;
        env->capacity = capacity;
    }
    djs_binding_t *binding = &env->bindings[env->count++];
    binding->name = djs_arena_strdup(rt, name);
    binding->value = value;
    binding->initialized = true;
    binding->flags = mutable_binding ? 1u : 0u;
    return binding;
}

void djs_env_assign(djs_runtime_t *rt, djs_env_t *env, const char *name, djs_value_t value) {
    djs_binding_t *binding = djs_env_lookup(env, name);
    if (binding != NULL) {
        if ((binding->flags & 1u) == 0) {
            djs_throw_type_error(rt, "assignment to constant variable");
        }
        binding->value = value;
        binding->initialized = true;
        return;
    }
    /* A global property wins over a fresh global binding. */
    if (rt->global != NULL && djs_property_get(rt->global, name) != NULL) {
        djs_object_set(rt, rt->global, name, value);
        return;
    }
    /* Undeclared assignment creates a global (sloppy mode). */
    djs_env_declare(rt, rt->global_env, name, value, true);
}

/* ------------------------------------------------------------------ errors */

djs_value_t djs_make_error(djs_runtime_t *rt, const char *kind, const char *message) {
    djs_object_t *prototype = rt->error_prototype;
    if (kind != NULL) {
        djs_object_t *constructor = NULL;
        if (djs_strcmp(kind, "TypeError") == 0) constructor = rt->type_error_constructor;
        else if (djs_strcmp(kind, "RangeError") == 0) constructor = rt->range_error_constructor;
        else if (djs_strcmp(kind, "SyntaxError") == 0) constructor = rt->syntax_error_constructor;
        else if (djs_strcmp(kind, "ReferenceError") == 0) constructor = rt->reference_error_constructor;
        if (constructor != NULL) {
            djs_property_t *property = djs_property_get(constructor, "prototype");
            if (property != NULL && property->value.tag == DJS_TAG_OBJECT) {
                prototype = property->value.as.object;
            }
        }
    }
    djs_object_t *error = djs_object_new(rt, prototype);
    if (error == NULL) return DJS_UNDEFINED;
    error->class = DJS_CLASS_ERROR;
    error->error_message = djs_string_value(djs_string_new_cstr(rt, message ? message : ""));
    djs_property_set(rt, error, "message", error->error_message, 0);
    djs_property_set(rt, error, "name",
                     djs_string_value(djs_string_new_cstr(rt, kind ? kind : "Error")), 0);
    return djs_object_value(error);
}

__attribute__((noreturn)) void djs_throw(djs_runtime_t *rt, djs_value_t error) {
    if (rt->catch_frame != NULL) {
        djs_catch_frame_t *frame = rt->catch_frame;
        rt->catch_frame = frame->previous;
        frame->error = error;
        djs_longjmp(&frame->saved, 1);
    }
    char message[512];
    djs_format_error(rt, error, message, sizeof(message));
    djs_write_cstr(rt, "Uncaught ");
    djs_write_cstr(rt, message);
    djs_write_cstr(rt, "\n");
    rt->exit_status = 1;
    rt->exiting = true;
    djs_longjmp(NULL, 1);
}

__attribute__((noreturn)) void djs_throw_error(djs_runtime_t *rt, const char *kind,
                                               const char *message) {
    djs_throw(rt, djs_make_error(rt, kind, message));
}

__attribute__((noreturn)) void djs_throw_type_error(djs_runtime_t *rt, const char *message) {
    djs_throw_error(rt, "TypeError", message);
}

void djs_format_error(djs_runtime_t *rt, djs_value_t error, char *buffer, size_t size) {
    if (size == 0) return;
    buffer[0] = '\0';
    if (error.tag != DJS_TAG_OBJECT) {
        djs_value_t text = djs_to_string(rt, error);
        if (text.tag == DJS_TAG_STRING) {
            size_t length = text.as.string->length;
            if (length >= size) length = size - 1;
            djs_memcpy(buffer, text.as.string->bytes, length);
            buffer[length] = '\0';
        }
        return;
    }
    djs_value_t name = djs_object_get(rt, error.as.object, "name");
    djs_value_t message = djs_object_get(rt, error.as.object, "message");
    const char *name_text = name.tag == DJS_TAG_STRING ? name.as.string->bytes : "Error";
    const char *message_text = message.tag == DJS_TAG_STRING ? message.as.string->bytes : "";
    size_t written = 0;
    while (name_text[written] != '\0' && written + 1 < size) {
        buffer[written] = name_text[written];
        written++;
    }
    if (message_text[0] != '\0' && written + 2 < size) {
        buffer[written++] = ':';
        buffer[written++] = ' ';
        size_t index = 0;
        while (message_text[index] != '\0' && written + 1 < size) {
            buffer[written++] = message_text[index++];
        }
    }
    buffer[written] = '\0';
}

/* ------------------------------------------------------------ conversions */

bool djs_to_boolean(djs_value_t value) {
    switch (value.tag) {
    case DJS_TAG_UNDEFINED:
    case DJS_TAG_NULL: return false;
    case DJS_TAG_BOOL: return value.as.boolean;
    case DJS_TAG_NUMBER: return value.as.number != 0.0 && value.as.number == value.as.number;
    case DJS_TAG_STRING: return value.as.string->length != 0;
    case DJS_TAG_OBJECT: return true;
    }
    return false;
}

djs_value_t djs_to_primitive(djs_runtime_t *rt, djs_value_t value, int hint) {
    if (value.tag != DJS_TAG_OBJECT) return value;
    djs_object_t *object = value.as.object;
    const char *first = hint == 0 ? "valueOf" : "toString";
    const char *second = hint == 0 ? "toString" : "valueOf";
    for (int attempt = 0; attempt < 2; attempt++) {
        const char *name = attempt == 0 ? first : second;
        djs_value_t method = djs_object_get(rt, object, name);
        if (method.tag == DJS_TAG_OBJECT && method.as.object->function != NULL) {
            djs_value_t result = djs_call(rt, method, value, NULL, 0);
            if (result.tag != DJS_TAG_OBJECT) return result;
        }
    }
    djs_throw_type_error(rt, "Cannot convert object to primitive value");
}

djs_value_t djs_to_string(djs_runtime_t *rt, djs_value_t value) {
    switch (value.tag) {
    case DJS_TAG_UNDEFINED: return djs_string_value(djs_string_new_cstr(rt, "undefined"));
    case DJS_TAG_NULL: return djs_string_value(djs_string_new_cstr(rt, "null"));
    case DJS_TAG_BOOL:
        return djs_string_value(djs_string_new_cstr(rt, value.as.boolean ? "true" : "false"));
    case DJS_TAG_NUMBER: {
        char buffer[64];
        djs_number_to_string(value.as.number, buffer, sizeof(buffer));
        return djs_string_value(djs_string_new_cstr(rt, buffer));
    }
    case DJS_TAG_STRING: return value;
    case DJS_TAG_OBJECT: {
        djs_value_t primitive = djs_to_primitive(rt, value, 1);
        if (primitive.tag == DJS_TAG_OBJECT) {
            return djs_string_value(djs_string_new_cstr(rt, "[object Object]"));
        }
        return djs_to_string(rt, primitive);
    }
    }
    return DJS_UNDEFINED;
}

djs_value_t djs_to_number(djs_runtime_t *rt, djs_value_t value) {
    switch (value.tag) {
    case DJS_TAG_UNDEFINED: return djs_number(0.0 / 0.0);
    case DJS_TAG_NULL: return djs_number(0.0);
    case DJS_TAG_BOOL: return djs_number(value.as.boolean ? 1.0 : 0.0);
    case DJS_TAG_NUMBER: return value;
    case DJS_TAG_STRING: {
        double number = 0.0;
        if (!djs_string_to_number(value.as.string->bytes, value.as.string->length, &number)) {
            return djs_number(0.0 / 0.0);
        }
        return djs_number(number);
    }
    case DJS_TAG_OBJECT: {
        djs_value_t primitive = djs_to_primitive(rt, value, 0);
        return djs_to_number(rt, primitive);
    }
    }
    return djs_number(0.0 / 0.0);
}

bool djs_strict_equals(djs_value_t a, djs_value_t b) {
    if (a.tag != b.tag) return false;
    switch (a.tag) {
    case DJS_TAG_UNDEFINED:
    case DJS_TAG_NULL: return true;
    case DJS_TAG_BOOL: return a.as.boolean == b.as.boolean;
    case DJS_TAG_NUMBER:
        if (a.as.number != a.as.number || b.as.number != b.as.number) return false;
        return a.as.number == b.as.number;
    case DJS_TAG_STRING: return djs_string_equals(a.as.string, b.as.string);
    case DJS_TAG_OBJECT: return a.as.object == b.as.object;
    }
    return false;
}

bool djs_loose_equals(djs_runtime_t *rt, djs_value_t a, djs_value_t b) {
    if (a.tag == b.tag) return djs_strict_equals(a, b);
    if ((a.tag == DJS_TAG_NULL && b.tag == DJS_TAG_UNDEFINED) ||
        (a.tag == DJS_TAG_UNDEFINED && b.tag == DJS_TAG_NULL)) {
        return true;
    }
    if (a.tag == DJS_TAG_OBJECT) {
        a = djs_to_primitive(rt, a, 0);
        return djs_loose_equals(rt, a, b);
    }
    if (b.tag == DJS_TAG_OBJECT) {
        b = djs_to_primitive(rt, b, 0);
        return djs_loose_equals(rt, a, b);
    }
    djs_value_t number_a = djs_to_number(rt, a);
    djs_value_t number_b = djs_to_number(rt, b);
    if (number_a.tag == DJS_TAG_NUMBER && number_b.tag == DJS_TAG_NUMBER) {
        if (number_a.as.number != number_a.as.number || number_b.as.number != number_b.as.number) {
            return false;
        }
        return number_a.as.number == number_b.as.number;
    }
    return false;
}

bool djs_value_equals(djs_runtime_t *rt, djs_value_t a, djs_value_t b) {
    return djs_strict_equals(a, b) || djs_loose_equals(rt, a, b);
}

const char *djs_typeof(djs_value_t value) {
    switch (value.tag) {
    case DJS_TAG_UNDEFINED: return "undefined";
    case DJS_TAG_NULL: return "object";
    case DJS_TAG_BOOL: return "boolean";
    case DJS_TAG_NUMBER: return "number";
    case DJS_TAG_STRING: return "string";
    case DJS_TAG_OBJECT:
        return value.as.object->function != NULL ? "function" : "object";
    }
    return "undefined";
}
