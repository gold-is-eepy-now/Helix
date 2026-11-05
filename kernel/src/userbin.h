/* userbin.h — install the embedded ring-3 programs into /bin on the root. */
#ifndef HELIX_USERBIN_H
#define HELIX_USERBIN_H

#include <stdbool.h>

/* Create /bin if needed and write every embedded program into it. Returns true
 * only if every program was written in full. Needs the root to be mounted. */
bool userbin_install(void);

#endif /* HELIX_USERBIN_H */
