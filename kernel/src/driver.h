/* driver.h — kernel driver registry and device-object inventory. */
#ifndef HELIX_DRIVER_H
#define HELIX_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "object.h"

#define DRIVER_NAME_MAX 31u
#define DRIVER_CLASS_MAX 23u
#define DRIVER_REGISTRY_CAPACITY 16u
#define DRIVER_CONTROL_UART_WRITE_BYTE 1u

typedef struct {
    char name[DRIVER_NAME_MAX + 1u];
    char device_class[DRIVER_CLASS_MAX + 1u];
    uint64_t object_id;
    uint32_t control_handle;
    uint32_t read_handle;
    uint32_t write_handle;
    uint32_t sector_size;
    uint64_t sector_count;
    bool active;
} driver_info_t;

typedef bool (*driver_start_fn)(void *context);
typedef bool (*driver_stop_fn)(void *context);
typedef bool (*driver_control_fn)(void *context, uint32_t request,
                                  uint64_t argument);
typedef bool (*driver_read_fn)(void *context, uint64_t offset,
                               void *buffer, size_t length);
typedef bool (*driver_write_fn)(void *context, uint64_t offset,
                                const void *buffer, size_t length);

typedef bool (*driver_flush_fn)(void *context);

typedef struct {
    const char *name;
    const char *device_class;
    void *context;
    driver_start_fn start;
    driver_stop_fn stop;
    driver_control_fn control;
    driver_read_fn read;
    driver_write_fn write;
    driver_flush_fn flush; /* optional: commit the device write cache */
    uint32_t sector_size;
    uint64_t sector_count;
} driver_descriptor_t;

void driver_manager_init(void);
/* Start callbacks run during registry setup and must be bounded. Control and
 * block callbacks run outside the registry guard, in the caller's interrupt
 * state, and must not sleep; normal thread callers remain timer-preemptible. */
bool driver_register(const driver_descriptor_t *descriptor);
bool driver_start(const char *name);
bool driver_stop(const char *name);
bool driver_control(handle_table_t *handles, handle_t control_capability,
                    uint32_t request, uint64_t argument);
bool driver_read(handle_table_t *handles, handle_t read_capability,
                 uint64_t offset, void *buffer, size_t length);
bool driver_write(handle_table_t *handles, handle_t write_capability,
                  uint64_t offset, const void *buffer, size_t length);
/* Ask a block device to commit its write cache (no-op success if the driver
 * has no flush callback). */
bool driver_flush(handle_table_t *handles, handle_t write_capability);
size_t driver_count(void);
bool driver_get_info(size_t index, driver_info_t *info);

#endif /* HELIX_DRIVER_H */
