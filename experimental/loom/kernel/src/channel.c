/* Channels: kernel objects that carry typed messages between Cells. A channel has one declared type;
 * a send with any other type is refused. The queue is fixed-size, so a full channel refuses the
 * send (the sender must retry) instead of growing. No blocking: a Cell gets an error at once. */
#include <stdint.h>
#include <string.h>
#include "loom_cell.h"
#include "loom_kernel.h"

#define CHAN_MAX    4
#define CHAN_QUEUE  8
#define EINVAL_RET  (-22)
#define EAGAIN_RET  (-11)

struct channel {
    bool       used;
    uint32_t   type;
    struct msg q[CHAN_QUEUE];
    uint32_t   head, count;
};

static struct channel g_chan[CHAN_MAX];

int channel_create(uint32_t type) {
    for (int i = 0; i < CHAN_MAX; i++) {
        if (!g_chan[i].used) {
            memset(&g_chan[i], 0, sizeof g_chan[i]);
            g_chan[i].used = true;
            g_chan[i].type = type;
            return i;
        }
    }
    return -1;
}

int channel_send(int ch, const struct msg *m) {
    struct channel *c = &g_chan[ch];
    if (m->type != c->type) return EINVAL_RET;
    if (c->count == CHAN_QUEUE) return EAGAIN_RET;
    c->q[(c->head + c->count) % CHAN_QUEUE] = *m;
    c->count++;
    return 0;
}

int channel_recv(int ch, struct msg *out) {
    struct channel *c = &g_chan[ch];
    if (c->count == 0) return EAGAIN_RET;
    *out = c->q[c->head];
    c->head = (c->head + 1) % CHAN_QUEUE;
    c->count--;
    return 0;
}

/* Wrappers for the system call path: the channel index comes from a capability, so it is in range. */
int channel_send_checked(uint8_t ch, const struct msg *m) { return channel_send(ch, m); }
int channel_recv_checked(uint8_t ch, struct msg *out)    { return channel_recv(ch, out); }
