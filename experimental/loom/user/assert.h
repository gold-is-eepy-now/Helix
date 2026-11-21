/* The library's assert, for the Weaver Cell: a failed check ends the Cell with status 0x7ff. */
#ifndef LOOM_USER_ASSERT_H
#define LOOM_USER_ASSERT_H
#include "ulib.h"
#define assert(e) ((e) ? (void)0 : sys_exit(0x7ff))
#endif
