/* The Weaver, as a Cell. It runs outside the kernel: it reads the events the kernel sends (capacity
 * changes, the current Morphology, the tick), computes a proposal with the same rules the kernel uses,
 * and asks the kernel to make it (SYS_PROPOSE). The kernel's rules decide. The Weaver holds no
 * authority to change the Field or a Continuity directly.
 *
 * Tick 24 is a deliberate misbehaviour: the Weaver proposes the contract-breaking Morphology D. The
 * kernel must refuse it, and the demo checks that it does. */
#include "ulib.h"
#include "string.h"
#include "loom.h"
#include "loom_video.h"

#define H_CAP    0      /* RECV: capacity changes (type MSG_CAP) */
#define H_CUR    1      /* RECV: the current Morphology (type MSG_CUR) */
#define H_TICK   2      /* RECV: a tick (type MSG_TICK) */
#define H_PROP   3      /* PROPOSE on the video Continuity */

#define MSG_CAP   1
#define MSG_CUR   2
#define MSG_TICK  3

#define MISBEHAVE_TICK   24
#define MISBEHAVE_MORPH  3

static uint32_t read_u32(const struct msg *m, int at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)m->payload[at + i] << (8 * i);
    return v;
}

int main(void) {
    loom_continuity_t video;
    memset(&video, 0, sizeof video);
    loom_video_define(&video);                   /* the same choices the kernel has */

    loom_field_t field;
    memset(&field, 0, sizeof field);             /* capacities arrive as CAP messages */

    uint32_t current = 0;
    struct msg m;

    for (;;) {
        /* Capacity changes first, then the current Morphology, then the tick. Each queue is drained
         * before the next, so the tick is processed against the newest state. */
        while (sys_recv(H_CAP, &m) == 0) {
            uint32_t kind = read_u32(&m, 0);
            uint32_t value = read_u32(&m, 4);
            if (kind < LOOM_RES_COUNT) field.capacity.amount[kind] = value;
        }
        while (sys_recv(H_CUR, &m) == 0) {
            current = read_u32(&m, 0);
        }

        if (sys_recv(H_TICK, &m) != 0) {
            sys_yield();                          /* nothing for this tick yet */
            continue;
        }
        uint32_t tick = read_u32(&m, 0);

        /* The Weaver's view of the Continuity: running in `current`, holding that demand. */
        video.current = current;
        video.running = true;
        video.reserved = video.morphs[current].demand;
        field.reserved = video.morphs[current].demand;

        if (tick == MISBEHAVE_TICK) {
            sys_propose(H_PROP, MISBEHAVE_MORPH); /* the kernel refuses this; its reply is in the log */
        }

        int best = loom_weaver_propose(&field, &video);
        if (best >= 0 && (uint32_t)best != current) {
            sys_propose(H_PROP, (uint32_t)best);
        }
        sys_yield();
    }
    return 0;
}
