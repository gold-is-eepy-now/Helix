/* The Weaver running as a Cell. The kernel is the event dispatcher: it sends each tick's events on
 * typed channels, resumes the Weaver until it yields, then prints what the kernel did with the
 * Weaver's proposals. The scenario and its expected summary match the kernel-side demo (demo.c). */
#include <stdint.h>
#include <string.h>
#include "arch.h"
#include "loom.h"
#include "loom_cell.h"
#include "loom_kernel.h"
#include "loom_video.h"
#include "registry.h"
#include <assert.h>

#define TICKS            32
#define STATE_BASE       8

/* Event channels: one per message type (a channel carries one declared type). */
#define MSG_CAP          1
#define MSG_CUR          2
#define MSG_TICK         3

extern const uint8_t weaver_image_start[], weaver_image_end[];

static loom_field_t g_field;
static loom_continuity_t g_video;
static int g_fail_once;

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static bool video_migrate(void *ctx, uint32_t from, uint32_t to, loom_state_t *s) {
    int *fail_once = ctx;
    (void)from;
    if (*fail_once) { *fail_once = 0; return false; }
    if (s->len < STATE_BASE) return false;
    s->len = STATE_BASE + (to == 1 ? 0u : 4u);
    if (to == 2) put32(s->data + STATE_BASE, 1);
    if (to == 3) put32(s->data + STATE_BASE, 2);
    return true;
}

static void tick_prefix(uint32_t tick) {
    serial_puts("t=");
    serial_putc((char)('0' + (tick / 10) % 10));
    serial_putc((char)('0' + tick % 10));
    serial_puts("  ");
}

static void send_pair(int ch, uint32_t type, uint32_t a, uint32_t b) {
    struct msg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    put32(m.payload, a);
    put32(m.payload + 4, b);
    int r = channel_send(ch, &m);
    assert(r == 0);
    (void)r;
}

static const char *name_of(uint32_t idx) { return g_video.morphs[idx].name; }

/* Prints the proposals the kernel handled this tick and counts them. */
static void report_proposals(uint32_t tick, uint32_t *transitions, uint32_t *refused, uint32_t *contract) {
    for (uint32_t i = 0; i < registry_log_count(); i++) {
        const struct reg_record *r = registry_log_at(i);
        tick_prefix(tick);
        if (r->result == LOOM_OK) {
            serial_puts("transition "); serial_puts(name_of(r->from)); serial_puts(" -> ");
            serial_puts(name_of(r->to));
            serial_puts("  [weaver proposal]  state kept: frames=");
            serial_putu64(get32(g_video.state.data)); serial_puts(" ref=");
            serial_putu64(get32(g_video.state.data + 4)); serial_puts("\n");
            (*transitions)++;
        } else {
            if (r->to == 3 && r->result == LOOM_ERR_CONTRACT) {
                serial_puts("rogue proposal: morph D (quality 30) refused by the kernel: ");
                (*contract)++;
            } else {
                serial_puts("transition refused: ");
            }
            serial_puts(loom_result_str(r->result));
            serial_puts("  [weaver proposal]\n");
            (*refused)++;
        }
    }
}

bool weaver_selftest(void) {
    uint64_t free0 = pmm_free_count();

    loom_demand_t cap;
    memset(&cap, 0, sizeof cap);
    cap.amount[LOOM_RES_CPU] = 4000;
    cap.amount[LOOM_RES_GPU] = 1000;
    cap.amount[LOOM_RES_MEM] = 1048576;
    cap.amount[LOOM_RES_MEM_BW] = 2000000;
    cap.amount[LOOM_RES_POWER] = 15000;
    loom_field_init(&g_field, &cap);
    registry_bind_field(&g_field);
    registry_log_clear();

    memset(&g_video, 0, sizeof g_video);
    loom_video_define(&g_video);
    g_video.migrate = video_migrate;
    g_video.migrate_ctx = &g_fail_once;
    g_video.state.schema = 1;
    g_video.state.len = STATE_BASE;
    registry_add(0, &g_video);
    assert(loom_continuity_start(&g_field, &g_video, 0) == LOOM_OK);
    serial_puts("t=00  start video-playback in A hw-decode (kernel start; the Weaver proposes from tick 1)\n");

    /* Event channels, and the Weaver's capabilities: three receive handles and one PROPOSE handle. */
    int ch_cap = channel_create(MSG_CAP), ch_cur = channel_create(MSG_CUR), ch_tick = channel_create(MSG_TICK);
    assert(ch_cap >= 0 && ch_cur >= 0 && ch_tick >= 0);
    struct cap caps[CELL_MAX_CAPS];
    memset(caps, 0, sizeof caps);
    caps[0] = (struct cap){ .valid = true, .object = CAP_OBJ_CHANNEL, .rights = CAP_RECV, .index = (uint8_t)ch_cap };
    caps[1] = (struct cap){ .valid = true, .object = CAP_OBJ_CHANNEL, .rights = CAP_RECV, .index = (uint8_t)ch_cur };
    caps[2] = (struct cap){ .valid = true, .object = CAP_OBJ_CHANNEL, .rights = CAP_RECV, .index = (uint8_t)ch_tick };
    caps[3] = (struct cap){ .valid = true, .object = CAP_OBJ_CONTINUITY, .rights = CAP_PROPOSE, .index = 0 };

    /* Start the Weaver and send the initial state. It drains the queues and yields. */
    struct loom_cell weaver;
    bool created = cell_create(&weaver, weaver_image_start, weaver_image_end, caps);
    assert(created);
    send_pair(ch_cap, MSG_CAP, LOOM_RES_CPU, 4000);
    send_pair(ch_cap, MSG_CAP, LOOM_RES_GPU, 1000);
    send_pair(ch_cap, MSG_CAP, LOOM_RES_MEM, 1048576);
    send_pair(ch_cap, MSG_CAP, LOOM_RES_MEM_BW, 2000000);
    send_pair(ch_cap, MSG_CAP, LOOM_RES_POWER, 15000);
    send_pair(ch_cur, MSG_CUR, 0, 0);
    int64_t st = cell_run(&weaver.ctx);
    if (st != CELL_STATUS_YIELD) { serial_puts("weaver did not start\n"); return false; }

    static const struct { uint32_t tick; loom_res_t res; uint32_t value; const char *why; } events[] = {
        { 8,  LOOM_RES_GPU, 0,    "GPU withdrawn by its driver (simulated fault)" },
        { 14, LOOM_RES_CPU, 1500, "thermal throttle: CPU budget drops to 1500 mCPU" },
        { 18, LOOM_RES_CPU, 4000, "throttle cleared" },
        { 26, LOOM_RES_GPU, 1000, "GPU returned" },
    };

    uint32_t stalls = 0, transitions = 0, refused = 0, contract = 0;
    uint32_t frames = 0;
    size_t next_event = 0;
    bool weaver_alive = true;

    for (uint32_t tick = 1; tick <= TICKS && weaver_alive; tick++) {
        /* Event dispatch: pressure events become capacity messages for the Weaver. */
        while (next_event < sizeof events / sizeof events[0] && events[next_event].tick == tick) {
            g_field.capacity.amount[events[next_event].res] = events[next_event].value;
            send_pair(ch_cap, MSG_CAP, events[next_event].res, events[next_event].value);
            tick_prefix(tick); serial_puts("pressure: "); serial_puts(events[next_event].why); serial_puts("\n");
            next_event++;
        }
        if (tick == 18) { g_fail_once = 1; tick_prefix(tick); serial_puts("injecting one state-migration failure\n"); }

        send_pair(ch_cur, MSG_CUR, g_video.current, 0);
        send_pair(ch_tick, MSG_TICK, tick, 0);

        /* Run the Weaver for this tick. It proposes, the kernel decides, and the Weaver yields. */
        registry_log_clear();
        st = cell_resume(&weaver.ctx, &weaver.frame);
        if (st != CELL_STATUS_YIELD) {
            tick_prefix(tick); serial_puts("weaver stopped unexpectedly\n");
            weaver_alive = false;
            break;
        }
        report_proposals(tick, &transitions, &refused, &contract);

        /* Frame delivery, as in the kernel-side demo. */
        if (loom_field_overcommitted(&g_field)) {
            g_video.stall_ticks++;
            stalls++;
            tick_prefix(tick); serial_puts("stall: field over capacity\n");
        } else {
            g_video.stall_ticks = 0;
            uint32_t f = get32(g_video.state.data) + 1;
            put32(g_video.state.data, f);
            put32(g_video.state.data + 4, f);
            g_video.frames++;
            frames = f;
        }
    }

    cell_destroy(&weaver);
    loom_continuity_stop(&g_field, &g_video);

    serial_puts("summary: "); serial_putu64(TICKS); serial_puts(" ticks, ");
    serial_putu64(frames); serial_puts(" frames, "); serial_putu64(stalls); serial_puts(" stalls, ");
    serial_putu64(transitions); serial_puts(" transitions, "); serial_putu64(refused); serial_puts(" refused, ");
    serial_putu64(contract); serial_puts(" contract refusal(s)\n");

    bool ok = weaver_alive && frames == TICKS && stalls == 0 && transitions == 4 && refused == 2 &&
              contract == 1 && pmm_free_count() == free0;
    return ok;
}
