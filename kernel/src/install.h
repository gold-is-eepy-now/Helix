/* install.h — boot image module and guarded USB disk installer. */
#ifndef HELIX_INSTALL_H
#define HELIX_INSTALL_H

#include <stdbool.h>
#include <stddef.h>

bool install_image_set(const void *image, size_t size);
bool install_to_disk(const char *device_name);

#endif /* HELIX_INSTALL_H */
