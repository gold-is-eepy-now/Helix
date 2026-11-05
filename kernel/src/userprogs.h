/* userprogs.h — the ring-3 programs embedded in the kernel image.
 *
 * Each program is a static ELF built from userspace/ by kernel/Makefile and
 * linked into the kernel as a blob. The kernel installs them into /bin on the
 * root at boot (userbin.c), and starts /bin/init from there. */
#ifndef HELIX_USERPROGS_H
#define HELIX_USERPROGS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;          /* short name, e.g. "sh" */
    const char *install_path;  /* where it is installed on the root, e.g. "/bin/sh" */
    const uint8_t *start;      /* the ELF image, in the kernel's read-only data */
    const uint8_t *end;
} userprog_t;

/* The program with this short name, or NULL. */
const userprog_t *userprog_find(const char *name);

/* The embedded programs, by index. */
size_t userprog_count(void);
const userprog_t *userprog_at(size_t index);

#endif /* HELIX_USERPROGS_H */
