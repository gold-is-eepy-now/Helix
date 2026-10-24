/* driver.c — bounded driver registry backed by Helix device objects. */
#include "driver.h"

#include "capability.h"
#include "kstring.h"
#include "object.h"
#include "sync.h"

typedef struct {
    driver_info_t info;
    driver_descriptor_t descriptor;
    object_t *device;
    size_t in_flight;
} driver_entry_t;

static driver_entry_t g_drivers[DRIVER_REGISTRY_CAPACITY];
static size_t g_driver_count;
static bool g_initialized;

static bool copy_name(char *destination, size_t capacity, const char *source) {
    if (source == NULL || source[0] == '\0') return false;
    size_t length = 0;
    while (source[length] != '\0') {
        if (length + 1 >= capacity) return false;
        destination[length] = source[length];
        length++;
    }
    destination[length] = '\0';
    return true;
}

void driver_manager_init(void) {
    IRQ_GUARD;
    if (g_initialized) return;
    memset(g_drivers, 0, sizeof(g_drivers));
    g_driver_count = 0;
    g_initialized = true;
}

static size_t driver_find(const char *name) {
    if (name == NULL) return DRIVER_REGISTRY_CAPACITY;
    for (size_t i = 0; i < g_driver_count; i++)
        if (strcmp(g_drivers[i].info.name, name) == 0) return i;
    return DRIVER_REGISTRY_CAPACITY;
}

bool driver_register(const driver_descriptor_t *descriptor) {
    IRQ_GUARD;
    if (!g_initialized || descriptor == NULL || descriptor->start == NULL ||
        g_driver_count == DRIVER_REGISTRY_CAPACITY) return false;
    if (driver_find(descriptor->name) != DRIVER_REGISTRY_CAPACITY) return false;

    driver_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    if (!copy_name(entry.info.name, sizeof(entry.info.name), descriptor->name) ||
        !copy_name(entry.info.device_class, sizeof(entry.info.device_class),
                   descriptor->device_class)) return false;
    entry.descriptor = *descriptor;
    entry.info.sector_size = descriptor->sector_size;
    entry.info.sector_count = descriptor->sector_count;

    entry.device = object_create(OBJECT_TYPE_DEVICE, 0, NULL);
    if (entry.device == NULL) return false;
    entry.info.object_id = object_id(entry.device);
    handle_table_t *handles = object_kernel_handles();
    handle_t source = handle_create(handles, entry.device,
        OBJECT_RIGHT_READ | OBJECT_RIGHT_INSPECT | OBJECT_RIGHT_DUPLICATE |
        OBJECT_RIGHT_CONTROL | (descriptor->write != NULL ? OBJECT_RIGHT_WRITE : 0));
    entry.info.control_handle = source != 0
        ? capability_create(handles, source, OBJECT_RIGHT_CONTROL) : 0;
    entry.info.read_handle = source != 0 && descriptor->read != NULL
        ? capability_create(handles, source, OBJECT_RIGHT_READ) : 0;
    entry.info.write_handle = source != 0 && descriptor->write != NULL
        ? capability_create(handles, source, OBJECT_RIGHT_WRITE) : 0;
    if (source != 0) (void)handle_close(handles, source);
    if (entry.info.control_handle == 0 ||
        (descriptor->read != NULL && entry.info.read_handle == 0) ||
        (descriptor->write != NULL && entry.info.write_handle == 0)) {
        if (entry.info.control_handle != 0)
            (void)handle_close(handles, entry.info.control_handle);
        if (entry.info.read_handle != 0)
            (void)handle_close(handles, entry.info.read_handle);
        if (entry.info.write_handle != 0)
            (void)handle_close(handles, entry.info.write_handle);
        object_release(entry.device);
        return false;
    }
    if (!descriptor->start(descriptor->context)) {
        (void)handle_close(handles, entry.info.control_handle);
        if (entry.info.read_handle != 0)
            (void)handle_close(handles, entry.info.read_handle);
        if (entry.info.write_handle != 0)
            (void)handle_close(handles, entry.info.write_handle);
        object_release(entry.device);
        return false;
    }
    entry.info.active = true;
    g_drivers[g_driver_count++] = entry;
    object_release(entry.device);
    return true;
}

bool driver_start(const char *name) {
    IRQ_GUARD;
    size_t index = driver_find(name);
    if (index == DRIVER_REGISTRY_CAPACITY) return false;
    driver_entry_t *entry = &g_drivers[index];
    if (entry->info.active) return true;
    if (entry->descriptor.start == NULL ||
        !entry->descriptor.start(entry->descriptor.context)) return false;
    entry->info.active = true;
    return true;
}

bool driver_stop(const char *name) {
    IRQ_GUARD;
    size_t index = driver_find(name);
    if (index == DRIVER_REGISTRY_CAPACITY) return false;
    driver_entry_t *entry = &g_drivers[index];
    if (!entry->info.active || entry->descriptor.stop == NULL ||
        entry->in_flight != 0 ||
        !entry->descriptor.stop(entry->descriptor.context)) return false;
    entry->info.active = false;
    return true;
}

bool driver_control(handle_table_t *handles, handle_t control_capability,
                    uint32_t request, uint64_t argument) {
    object_t *device = NULL;
    driver_entry_t *leased = NULL;
    driver_control_fn callback = NULL;
    void *context = NULL;
    {
        IRQ_GUARD;
        device = capability_resolve(handles, control_capability,
            OBJECT_RIGHT_CONTROL, OBJECT_TYPE_DEVICE);
        if (device != NULL) {
            for (size_t i = 0; i < g_driver_count; i++) {
                driver_entry_t *entry = &g_drivers[i];
                if (entry->info.object_id == object_id(device) &&
                    entry->info.active && entry->descriptor.control != NULL) {
                    leased = entry;
                    callback = entry->descriptor.control;
                    context = entry->descriptor.context;
                    entry->in_flight++;
                    break;
                }
            }
        }
    }
    if (leased == NULL) {
        if (device != NULL) object_release(device);
        return false;
    }
    bool result = callback(context, request, argument);
    {
        IRQ_GUARD;
        if (leased->in_flight != 0) leased->in_flight--;
    }
    object_release(device);
    return result;
}

bool driver_read(handle_table_t *handles, handle_t read_capability,
                 uint64_t offset, void *buffer, size_t length) {
    if (buffer == NULL || length == 0) return false;
    object_t *device = NULL;
    driver_entry_t *leased = NULL;
    driver_read_fn callback = NULL;
    void *context = NULL;
    {
        IRQ_GUARD;
        device = capability_resolve(handles, read_capability,
            OBJECT_RIGHT_READ, OBJECT_TYPE_DEVICE);
        if (device != NULL) {
            for (size_t i = 0; i < g_driver_count; i++) {
                driver_entry_t *entry = &g_drivers[i];
                if (entry->info.object_id == object_id(device) &&
                    entry->info.active && entry->descriptor.read != NULL) {
                    leased = entry;
                    callback = entry->descriptor.read;
                    context = entry->descriptor.context;
                    entry->in_flight++;
                    break;
                }
            }
        }
    }
    if (leased == NULL) {
        if (device != NULL) object_release(device);
        return false;
    }
    bool result = callback(context, offset, buffer, length);
    {
        IRQ_GUARD;
        if (leased->in_flight != 0) leased->in_flight--;
    }
    object_release(device);
    return result;
}

bool driver_write(handle_table_t *handles, handle_t write_capability,
                  uint64_t offset, const void *buffer, size_t length) {
    if (buffer == NULL || length == 0) return false;
    object_t *device = NULL;
    driver_entry_t *leased = NULL;
    driver_write_fn callback = NULL;
    void *context = NULL;
    {
        IRQ_GUARD;
        device = capability_resolve(handles, write_capability,
            OBJECT_RIGHT_WRITE, OBJECT_TYPE_DEVICE);
        if (device != NULL) {
            for (size_t i = 0; i < g_driver_count; i++) {
                driver_entry_t *entry = &g_drivers[i];
                if (entry->info.object_id == object_id(device) &&
                    entry->info.active && entry->descriptor.write != NULL) {
                    leased = entry;
                    callback = entry->descriptor.write;
                    context = entry->descriptor.context;
                    entry->in_flight++;
                    break;
                }
            }
        }
    }
    if (leased == NULL) {
        if (device != NULL) object_release(device);
        return false;
    }
    bool result = callback(context, offset, buffer, length);
    {
        IRQ_GUARD;
        if (leased->in_flight != 0) leased->in_flight--;
    }
    object_release(device);
    return result;
}

bool driver_flush(handle_table_t *handles, handle_t write_capability) {
    object_t *device = NULL;
    driver_entry_t *leased = NULL;
    driver_flush_fn callback = NULL;
    void *context = NULL;
    {
        IRQ_GUARD;
        device = capability_resolve(handles, write_capability,
            OBJECT_RIGHT_WRITE, OBJECT_TYPE_DEVICE);
        if (device != NULL) {
            for (size_t i = 0; i < g_driver_count; i++) {
                driver_entry_t *entry = &g_drivers[i];
                if (entry->info.object_id == object_id(device) &&
                    entry->info.active) {
                    leased = entry;
                    callback = entry->descriptor.flush;
                    context = entry->descriptor.context;
                    entry->in_flight++;
                    break;
                }
            }
        }
    }
    if (leased == NULL) {
        if (device != NULL) object_release(device);
        return false;
    }
    bool result = callback == NULL || callback(context);
    {
        IRQ_GUARD;
        if (leased->in_flight != 0) leased->in_flight--;
    }
    object_release(device);
    return result;
}

size_t driver_count(void) {
    IRQ_GUARD;
    return g_driver_count;
}

bool driver_get_info(size_t index, driver_info_t *info) {
    IRQ_GUARD;
    if (info == NULL || index >= g_driver_count) return false;
    *info = g_drivers[index].info;
    return true;
}
