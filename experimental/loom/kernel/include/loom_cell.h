/* A Cell as the kernel sees it: its run record, plus its capability table. */
#ifndef LOOM_CELL_H
#define LOOM_CELL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "arch.h"

#define CELL_MAX_CAPS  8

enum { CAP_SEND = 1, CAP_RECV = 2 };

/* A capability: a handle (its index in the table) names a channel and the rights on it. */
struct cap {
    bool     valid;
    uint8_t  rights;     /* CAP_SEND and/or CAP_RECV */
    uint8_t  channel;    /* index into the channel table */
};

struct loom_cell {
    struct cell_ctx ctx;               /* first: cell_current() returns this and it is cast back */
    struct cap caps[CELL_MAX_CAPS];
};

/* A typed message: the sender's declared type plus a fixed-size payload. */
struct msg {
    uint32_t type;
    uint8_t  payload[32];
};

/* Channels (channel.c): one declared type, a short fixed queue. Kernel-owned; Cells reach them
 * only through handles. */
int  channel_create(uint32_t type);                          /* index, or -1 */
int  channel_send(int ch, const struct msg *m);              /* 0, -EINVAL (wrong type), -EAGAIN (full) */
int  channel_recv(int ch, struct msg *out);                  /* 0, or -EAGAIN (empty) */

/* User memory (uaccess.c): every byte is checked against the Cell's own page tables. */
bool copy_from_user(const struct loom_cell *c, void *dst, uint64_t src, size_t n);
bool copy_to_user(const struct loom_cell *c, uint64_t dst, const void *src, size_t n);

#endif
