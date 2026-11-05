/* donejs.h — donejs JavaScript runtime, ported to native Helix.
 *
 * The runtime is a C implementation of the donejs engine and its Node-style
 * host layer: a JavaScript interpreter with the globals, module system and
 * event-loop ordering that donejs exposes (console, process, timers, promises,
 * require() of the builtin modules).
 *
 * The same sources build twice:
 *   - hosted   (tests, development): libc allocator, stdio output;
 *   - freestanding (Helix): helix_alloc/helix_free and the kernel syscall
 *     interface, no libc at all.
 */
#ifndef HELIX_DONEJS_H
#define HELIX_DONEJS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct djs_runtime djs_runtime_t;

/* Sink for console output. `text` is not NUL-terminated; `length` is the
 * byte count. */
typedef void (*djs_write_fn)(void *user, const char *text, size_t length);

/* Options for djs_runtime_new(). Every string is copied into the runtime. */
typedef struct {
    djs_write_fn write_fn;   /* console sink; NULL discards output */
    void *write_user;        /* passed back to write_fn */
    const char *platform;    /* process.platform, e.g. "helix" */
    const char *arch;        /* process.arch, e.g. "x64" */
    const char *version;     /* process.version */
    const char *exec_path;   /* process.execPath */
    const char *cwd;         /* process.cwd() */
    char *const *argv;       /* process.argv (argv[0] is the runtime name) */
    size_t argv_count;
} djs_options_t;

/* Error description. The message is owned by the runtime and stays valid
 * until djs_runtime_free(). */
typedef struct {
    const char *message;
    size_t length;
} djs_error_t;

/* Create a runtime with the global object, builtin prototypes and the
 * builtin module registry installed. Returns NULL when out of memory. */
djs_runtime_t *djs_runtime_new(const djs_options_t *options);

/* Release every allocation the runtime owns. */
void djs_runtime_free(djs_runtime_t *rt);

/* Evaluate `source` as a program. `filename` is used in error messages.
 * Returns 0 on success; on failure returns non-zero and fills `error` when
 * `error` is not NULL. */
int djs_eval(djs_runtime_t *rt, const char *source, const char *filename,
             djs_error_t *error);

/* Run a script file. `args` are the script arguments (process.argv[2..]).
 * Returns 0 on success. */
int djs_run_file(djs_runtime_t *rt, const char *path, char *const *args,
                 size_t arg_count, djs_error_t *error);

/* Drain the event loop: microtasks (promise reactions, process.nextTick) and
 * timers, in the order donejs runs them. Returns the number of failures. */
int djs_run_event_loop(djs_runtime_t *rt);

/* Run the builtin API self test (the port of donejs' --selftest). Writes
 * one line per check to `write_fn`. Returns the number of failed checks. */
int djs_selftest(djs_runtime_t *rt, djs_write_fn write_fn, void *write_user);

/* Version string of the ported runtime ("0.1.0-helix"). */
const char *djs_version(void);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_DONEJS_H */
