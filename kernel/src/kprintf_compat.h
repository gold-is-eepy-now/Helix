/* kprintf_compat.h — the kernel's kprintf, or a host stand-in when the unit test builds
 * these files on a build machine. The host test defines HELIX_HOST_TEST. */
#ifndef HELIX_KPRINTF_COMPAT_H
#define HELIX_KPRINTF_COMPAT_H
#ifdef HELIX_HOST_TEST
#include <stdio.h>
#define kprintf(...) ((void)printf(__VA_ARGS__))
#else
#include "panic.h"
#endif
#endif
