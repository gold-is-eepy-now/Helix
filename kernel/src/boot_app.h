/* boot_app.h — access to an optional native app supplied by Limine. */
#ifndef HELIX_BOOT_APP_H
#define HELIX_BOOT_APP_H

#include <stdbool.h>
#include <stddef.h>

bool boot_app_set(const char *name, const void *data, size_t size);
bool boot_app_get(const char *name, const char **resolved_name,
                  const void **data, size_t *size);

#endif /* HELIX_BOOT_APP_H */
