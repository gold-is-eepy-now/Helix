/* linux/module.h — module metadata and the init hook.
 *
 * Helix has no loadable modules yet (milestone LC-2 adds .ko loading). For now a
 * driver is built into the kernel and its module_init function is collected into
 * the `compat_initcalls` linker section; compat_run_initcalls() runs them at boot.
 * That is how Linux's initcall sections work, so the same source will fit the
 * loader later. */
#ifndef HELIX_COMPAT_LINUX_MODULE_H
#define HELIX_COMPAT_LINUX_MODULE_H

#include <linux/init.h>

typedef int (*initcall_t)(void);

#define module_init(fn) \
    static initcall_t __compat_initcall_##fn \
        __attribute__((used, section("compat_initcalls"))) = (fn)

/* Built-in drivers have no exit path yet, so module_exit expands to nothing. */
#define module_exit(fn)

/* Metadata lives in a string the kernel never reads; it keeps the driver's
 * declaration visible in the object file, as Linux does with .modinfo. */
#define MODULE_LICENSE(s) \
    static const char __compat_license[] __attribute__((used)) = "license=" s
#define MODULE_AUTHOR(s) \
    static const char __compat_author[] __attribute__((used)) = "author=" s
#define MODULE_DESCRIPTION(s) \
    static const char __compat_description[] __attribute__((used)) = "description=" s

#endif /* HELIX_COMPAT_LINUX_MODULE_H */
