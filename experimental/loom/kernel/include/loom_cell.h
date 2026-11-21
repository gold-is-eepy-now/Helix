/* A Cell as the kernel sees it: its run record, plus its capability table. */
#ifndef LOOM_CELL_H
#define LOOM_CELL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "arch.h"

#define CELL_MAX_CAPS  8

/* What a capability names. */
enum { CAP_OBJ_NONE = 0, CAP_OBJ_CHANNEL = 1, CAP_OBJ_CONTINUITY = 2 };

/* Rights. SEND and RECV apply to channels; PROPOSE applies to a continuity: it lets the Cell ask the
 * kernel for a Morphology transition, which the kernel still checks. */
enum { CAP_SEND = 1, CAP_RECV = 2, CAP_PROPOSE = 4 };

/* A capability: a handle (its index in the table) names an object and the rights on it. */
struct cap {
    bool     valid;
    uint8_t  object;     /* CAP_OBJ_* */
    uint8_t  rights;     /* bitmask of CAP_* */
    uint8_t  index;      /* index into the kernel's channel or continuity table */
};

struct loom_cell {
    struct cell_ctx ctx;               /* first: cell_current() returns this and it is cast back */
    struct cap caps[CELL_MAX_CAPS];
    struct trap_frame frame;           /* saved on yield, used by cell_resume */
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
int  channel_send_checked(uint8_t ch, const struct msg *m);
int  channel_recv_checked(uint8_t ch, struct msg *out);

/* User memory (uaccess.c): every byte is checked against the Cell's own page tables. */
bool copy_from_user(const struct loom_cell *c, void *dst, uint64_t src, size_t n);
bool copy_to_user(const struct loom_cell *c, uint64_t dst, const void *src, size_t n);

#endif
