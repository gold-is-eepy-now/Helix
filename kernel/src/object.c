/* object.c — global object registry and rights-bearing handle tables.
 * Short operations are guarded against PIT preemption on the current single
 * CPU. Destruction callbacks must remain non-blocking and must not yield.
 */
#include "object.h"

#include "heap.h"
#include "kstring.h"
#include "sync.h"

struct object {
    uint64_t id;
    object_type_t type;
    uint32_t references;
    size_t payload_size;
    void *payload;
    object_destroy_fn destroy;
    bool destroying;
    struct object *next;
};

static object_t *g_objects;
static uint64_t g_next_object_id;
static uint64_t g_object_count;
static handle_table_t g_kernel_handles;
static bool g_initialized;

void object_manager_init(void) {
    IRQ_GUARD;
    if (g_initialized) return;
    g_objects = NULL;
    g_next_object_id = 1;
    g_object_count = 0;
    handle_table_init(&g_kernel_handles);
    g_initialized = true;
}

object_t *object_create(object_type_t type, size_t payload_size,
                        object_destroy_fn destroy) {
    IRQ_GUARD;
    if (!g_initialized || type <= OBJECT_TYPE_NONE || type > OBJECT_TYPE_CAPABILITY ||
        g_next_object_id == 0) {
        return NULL;
    }
    object_t *object = kzalloc(sizeof(*object));
    if (object == NULL) return NULL;
    if (payload_size != 0) {
        object->payload = kzalloc(payload_size);
        if (object->payload == NULL) {
            kfree(object);
            return NULL;
        }
    }
    object->id = g_next_object_id++;
    object->type = type;
    object->references = 1;
    object->payload_size = payload_size;
    object->destroy = destroy;
    object->next = g_objects;
    g_objects = object;
    g_object_count++;
    return object;
}

object_t *object_lookup(uint64_t id) {
    IRQ_GUARD;
    if (!g_initialized || id == 0) return NULL;
    for (object_t *object = g_objects; object != NULL; object = object->next) {
        if (object->id == id) {
            return object_retain(object) ? object : NULL;
        }
    }
    return NULL;
}

bool object_retain(object_t *object) {
    IRQ_GUARD;
    if (object == NULL || object->destroying || object->references == 0 ||
        object->references == UINT32_MAX) return false;
    object->references++;
    return true;
}

void object_release(object_t *object) {
    IRQ_GUARD;
    if (object == NULL || object->references == 0) return;
    if (--object->references != 0) return;

    object_t **link = &g_objects;
    while (*link != NULL && *link != object) link = &(*link)->next;
    if (*link == object) {
        *link = object->next;
        if (g_object_count != 0) g_object_count--;
    }
    object->destroying = true;
    if (object->destroy != NULL) object->destroy(object, object->payload);
    kfree(object->payload);
    kfree(object);
}

void *object_payload(object_t *object) { IRQ_GUARD; return object ? object->payload : NULL; }
uint64_t object_id(const object_t *object) { IRQ_GUARD; return object ? object->id : 0; }
object_type_t object_type(const object_t *object) {
    IRQ_GUARD;
    return object ? object->type : OBJECT_TYPE_NONE;
}
uint32_t object_reference_count(const object_t *object) {
    IRQ_GUARD;
    return object ? object->references : 0;
}
uint64_t object_count(void) { IRQ_GUARD; return g_object_count; }

void handle_table_init(handle_table_t *table) {
    IRQ_GUARD;
    if (table == NULL) return;
    memset(table, 0, sizeof(*table));
    for (size_t i = 0; i < HANDLE_TABLE_CAPACITY; i++) {
        table->entries[i].generation = 1;
    }
}

/* Handles devote 16 bits to a slot generation. Retire a slot when that
 * generation would wrap so an old stale handle can never become valid again.
 */
static void advance_generation(handle_entry_t *entry) {
    if (entry->generation == UINT16_MAX) {
        entry->generation = 0; /* permanently retired */
    } else {
        entry->generation++;
    }
}

void handle_table_close_all(handle_table_t *table) {
    IRQ_GUARD;
    if (table == NULL) return;
    for (size_t i = 0; i < HANDLE_TABLE_CAPACITY; i++) {
        handle_entry_t *entry = &table->entries[i];
        if (entry->object == NULL) continue;
        object_t *object = entry->object;
        entry->object = NULL;
        entry->rights = 0;
        advance_generation(entry);
        object_release(object);
    }
}

static handle_t encode_handle(size_t index, uint16_t generation) {
    return ((uint32_t)generation << 16) | (uint32_t)(index + 1);
}

static handle_entry_t *resolve_entry(handle_table_t *table, handle_t handle) {
    if (table == NULL || handle == 0) return NULL;
    uint32_t encoded_index = handle & 0xffffu;
    uint16_t generation = (uint16_t)(handle >> 16);
    if (encoded_index == 0 || encoded_index > HANDLE_TABLE_CAPACITY || generation == 0) {
        return NULL;
    }
    handle_entry_t *entry = &table->entries[encoded_index - 1];
    if (entry->object == NULL || entry->generation != generation) return NULL;
    return entry;
}

handle_t handle_create(handle_table_t *table, object_t *object, uint32_t rights) {
    IRQ_GUARD;
    if (table == NULL || object == NULL || rights == 0 ||
        (rights & ~OBJECT_RIGHT_ALL) != 0) return 0;
    for (size_t i = 0; i < HANDLE_TABLE_CAPACITY; i++) {
        handle_entry_t *entry = &table->entries[i];
        if (entry->object != NULL || entry->generation == 0) continue;
        if (!object_retain(object)) return 0;
        entry->object = object;
        entry->rights = rights;
        return encode_handle(i, entry->generation);
    }
    return 0;
}

object_t *handle_get(handle_table_t *table, handle_t handle,
                     uint32_t required_rights, object_type_t expected_type) {
    IRQ_GUARD;
    if (required_rights == 0 || (required_rights & ~OBJECT_RIGHT_ALL) != 0) return NULL;
    handle_entry_t *entry = resolve_entry(table, handle);
    if (entry == NULL || (entry->rights & required_rights) != required_rights ||
        (expected_type != OBJECT_TYPE_NONE && entry->object->type != expected_type) ||
        !object_retain(entry->object)) return NULL;
    return entry->object;
}

handle_t handle_duplicate(handle_table_t *table, handle_t source,
                          uint32_t reduced_rights) {
    IRQ_GUARD;
    handle_entry_t *entry = resolve_entry(table, source);
    if (entry == NULL || !(entry->rights & OBJECT_RIGHT_DUPLICATE) ||
        reduced_rights == 0 || (reduced_rights & ~entry->rights) != 0) return 0;
    return handle_create(table, entry->object, reduced_rights);
}

bool handle_close(handle_table_t *table, handle_t handle) {
    IRQ_GUARD;
    handle_entry_t *entry = resolve_entry(table, handle);
    if (entry == NULL) return false;
    object_t *object = entry->object;
    entry->object = NULL;
    entry->rights = 0;
    advance_generation(entry);
    object_release(object);
    return true;
}

bool handle_inspect(handle_table_t *table, handle_t handle, handle_info_t *info) {
    IRQ_GUARD;
    handle_entry_t *entry = resolve_entry(table, handle);
    if (entry == NULL || info == NULL || !(entry->rights & OBJECT_RIGHT_INSPECT)) {
        return false;
    }
    info->object_id = entry->object->id;
    info->type = entry->object->type;
    info->reference_count = entry->object->references;
    info->rights = entry->rights;
    return true;
}

uint64_t handle_count(const handle_table_t *table) {
    IRQ_GUARD;
    if (table == NULL) return 0;
    uint64_t count = 0;
    for (size_t i = 0; i < HANDLE_TABLE_CAPACITY; i++) {
        if (table->entries[i].object != NULL) count++;
    }
    return count;
}

handle_table_t *object_kernel_handles(void) { IRQ_GUARD; return &g_kernel_handles; }
