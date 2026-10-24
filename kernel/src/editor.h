/* editor.h — Helix's initial in-memory text editor. */
#ifndef HELIX_EDITOR_H
#define HELIX_EDITOR_H

#include <stdbool.h>

void editor_run(void);
bool editor_run_file(const char *path);

#endif /* HELIX_EDITOR_H */
