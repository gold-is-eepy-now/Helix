/* object.h — native kernel objects and rights-bearing handles. */
#ifndef HELIX_OBJECT_H
#define HELIX_OBJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HANDLE_TABLE_CAPACITY 64u

typedef uint32_t handle_t;

typedef enum {
    OBJECT_TYPE_NONE = 0,
    OBJECT_TYPE_EVENT,
    OBJECT_TYPE_CHANNEL,
    OBJECT_TYPE_MEMORY,
    OBJECT_TYPE_PROCESS,
    OBJECT_TYPE_CAPABILITY,
} object_type_t;

enum {
    OBJECT_RIGHT_READ      = 1u << 0,
    OBJECT_RIGHT_WRITE     = 1u << 1,
    OBJECT_RIGHT_DUPLICATE = 1u << 2,
    OBJECT_RIGHT_INSPECT   = 1u << 3,
    OBJECT_RIGHT_CONTROL   = 1u << 4,
    OBJECT_RIGHT_ALL       = (1u << 5) - 1,
};

typedef struct object object_t;
typedef void (*object_destroy_fn)(object_t *object, void *payload);

typedef struct {
    object_t *object;
    uint32_t rights;
    uint16_t generation;
} handle_entry_t;

/* Embed one table in each process once processes exist. The current kernel has
 * one system table, and the subsystem is intentionally single-threaded. */
typedef struct {
    handle_entry_t entries[HANDLE_TABLE_CAPACITY];
} handle_table_t;

typedef struct {
    uint64_t object_id;
    object_type_t type;
    uint32_t reference_count;
    uint32_t rights;
} handle_info_t;

void object_manager_init(void);

/* A new object starts with one owned reference. payload_size bytes are zeroed. */
object_t *object_create(object_type_t type, size_t payload_size,
                        object_destroy_fn destroy);
object_t *object_lookup(uint64_t object_id); /* returns a retained reference */
bool object_retain(object_t *object);
void object_release(object_t *object);
void *object_payload(object_t *object);
uint64_t object_id(const object_t *object);
object_type_t object_type(const object_t *object);
uint32_t object_reference_count(const object_t *object);
uint64_t object_count(void);

void handle_table_init(handle_table_t *table);
void handle_table_close_all(handle_table_t *table);
handle_t handle_create(handle_table_t *table, object_t *object, uint32_t rights);
object_t *handle_get(handle_table_t *table, handle_t handle,
                     uint32_t required_rights, object_type_t expected_type);
handle_t handle_duplicate(handle_table_t *table, handle_t source,
                          uint32_t reduced_rights);
bool handle_close(handle_table_t *table, handle_t handle);
bool handle_inspect(handle_table_t *table, handle_t handle, handle_info_t *info);
uint64_t handle_count(const handle_table_t *table);
handle_table_t *object_kernel_handles(void);

#endif /* HELIX_OBJECT_H */
