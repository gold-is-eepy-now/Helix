/* The simulator's scenario (src/sim_video.c in the host build), run inside the kernel. Same
 * Morphologies, same pressure schedule, same injected failure and rogue proposal. The output
 * goes to serial instead of printf. The rules come from the shared library under src/ and
 * include/, unchanged. */
#include <stdint.h>
#include <string.h>
#include "loom.h"
#include "loom_kernel.h"
#include <assert.h>

#define TICKS           32
#define CAP_GPU_DECODE  (1u << 0)
#define STATE_BASE      8

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void two_digits(uint32_t v) {
    serial_putc((char)('0' + (v / 10) % 10));
    serial_putc((char)('0' + v % 10));
}

static void tick_prefix(uint32_t tick) {
    serial_puts("t=");
    two_digits(tick);
    serial_puts("  ");
}

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

static void build_video(loom_continuity_t *c, int *fail_once) {
    memset(c, 0, sizeof *c);
    c->name = "video-playback";
    c->contract.correctness = LOOM_CONTRACT_DEGRADABLE;
    c->contract.min_quality = 50;
    c->contract.max_stall_ticks = 0;
    c->cap_authority = CAP_GPU_DECODE;

    loom_morph_t *a = &c->morphs[0];
    a->name = "A hw-decode"; a->quality = 100; a->state_schema = 2;
    a->required_caps = CAP_GPU_DECODE; a->cell_count = 2;
    a->cells[0] = (loom_cell_t){ "hw-decoder", {{ [LOOM_RES_CPU] = 200, [LOOM_RES_GPU] = 600, [LOOM_RES_MEM] = 65536, [LOOM_RES_POWER] = 3000 }} };
    a->cells[1] = (loom_cell_t){ "compositor", {{ [LOOM_RES_CPU] = 100, [LOOM_RES_POWER] = 1000 }} };

    loom_morph_t *b = &c->morphs[1];
    b->name = "B sw-decode"; b->quality = 100; b->state_schema = 1; b->cell_count = 2;
    b->cells[0] = (loom_cell_t){ "sw-decoder", {{ [LOOM_RES_CPU] = 1500, [LOOM_RES_MEM] = 65536, [LOOM_RES_POWER] = 7000 }} };
    b->cells[1] = (loom_cell_t){ "output", {{ [LOOM_RES_CPU] = 200, [LOOM_RES_MEM_BW] = 4096, [LOOM_RES_POWER] = 1000 }} };

    loom_morph_t *cc = &c->morphs[2];
    cc->name = "C half-res-sw"; cc->quality = 60; cc->state_schema = 3; cc->cell_count = 2;
    cc->cells[0] = (loom_cell_t){ "reduced-decoder", {{ [LOOM_RES_CPU] = 600, [LOOM_RES_MEM] = 32768, [LOOM_RES_POWER] = 4000 }} };
    cc->cells[1] = (loom_cell_t){ "output", {{ [LOOM_RES_CPU] = 100, [LOOM_RES_MEM_BW] = 2048, [LOOM_RES_POWER] = 1000 }} };

    loom_morph_t *d = &c->morphs[3];
    d->name = "D audio-only-stub"; d->quality = 30; d->state_schema = 1; d->cell_count = 1;
    d->cells[0] = (loom_cell_t){ "stub", {{ [LOOM_RES_CPU] = 50 }} };

    c->morph_count = 4;
    for (uint32_t i = 0; i < c->morph_count; i++) loom_morph_finalize(&c->morphs[i]);

    c->migrate = video_migrate;
    c->migrate_ctx = fail_once;
    c->state.schema = 1;
    c->state.len = STATE_BASE;
    memset(c->state.data, 0, sizeof c->state.data);
}

static const char *morph_name(const loom_continuity_t *c) { return c->morphs[c->current].name; }

static loom_result_t do_transition(loom_field_t *f, loom_continuity_t *c, uint32_t target, uint32_t tick, const char *why) {
    uint32_t frames0 = get32(c->state.data), ref0 = get32(c->state.data + 4);
    const char *from = morph_name(c);
    loom_result_t r = loom_transition(f, c, target);
    tick_prefix(tick);
    if (r == LOOM_OK) {
        assert(get32(c->state.data) == frames0 && get32(c->state.data + 4) == ref0);
        serial_puts("transition "); serial_puts(from); serial_puts(" -> "); serial_puts(morph_name(c));
        serial_puts("  ["); serial_puts(why); serial_puts("]  state kept: frames=");
        serial_putu64(frames0); serial_puts(" ref="); serial_putu64(ref0); serial_puts("\n");
    } else {
        serial_puts("transition refused: "); serial_puts(loom_result_str(r));
        serial_puts("  ["); serial_puts(why); serial_puts("]  still "); serial_puts(morph_name(c)); serial_puts("\n");
    }
    return r;
}

static const struct { uint32_t tick; loom_res_t res; uint32_t value; const char *why; } g_events[] = {
    { 8,  LOOM_RES_GPU, 0,    "GPU withdrawn by its driver (simulated fault)" },
    { 14, LOOM_RES_CPU, 1500, "thermal throttle: CPU budget drops to 1500 mCPU" },
    { 18, LOOM_RES_CPU, 4000, "throttle cleared" },
    { 26, LOOM_RES_GPU, 1000, "GPU returned" },
};

/* Returns true when the scenario ends the way the simulator's does. */
bool loom_demo_run(void) {
    loom_demand_t cap;
    memset(&cap, 0, sizeof cap);
    cap.amount[LOOM_RES_CPU] = 4000;
    cap.amount[LOOM_RES_GPU] = 1000;
    cap.amount[LOOM_RES_MEM] = 1048576;
    cap.amount[LOOM_RES_MEM_BW] = 2000000;
    cap.amount[LOOM_RES_POWER] = 15000;

    loom_field_t field;
    loom_field_init(&field, &cap);

    int fail_once = 0;
    loom_continuity_t video;
    build_video(&video, &fail_once);

    int pick = loom_weaver_propose(&field, &video);
    assert(pick >= 0);
    assert(loom_continuity_start(&field, &video, (uint32_t)pick) == LOOM_OK);
    serial_puts("t=00  start "); serial_puts(video.name); serial_puts(" in "); serial_puts(morph_name(&video)); serial_puts("\n");

    uint32_t stalls = 0, refused = 0, transitions = 0;
    int contract_refusals = 0;
    size_t next_event = 0;

    for (uint32_t tick = 1; tick <= TICKS; tick++) {
        while (next_event < sizeof g_events / sizeof g_events[0] && g_events[next_event].tick == tick) {
            field.capacity.amount[g_events[next_event].res] = g_events[next_event].value;
            tick_prefix(tick); serial_puts("pressure: "); serial_puts(g_events[next_event].why); serial_puts("\n");
            next_event++;
        }
        if (tick == 18) {
            fail_once = 1;
            tick_prefix(tick); serial_puts("injecting one state-migration failure\n");
        }

        int proposal = loom_weaver_propose(&field, &video);
        if (proposal >= 0 && (uint32_t)proposal != video.current) {
            const char *why = loom_field_overcommitted(&field) ? "repair after pressure" : "use returned resources";
            loom_result_t r = do_transition(&field, &video, (uint32_t)proposal, tick, why);
            if (r == LOOM_OK) transitions++; else refused++;
        }

        if (tick == 24) {
            loom_result_t r = loom_transition(&field, &video, 3);   /* morph D: below the contract minimum */
            if (r == LOOM_ERR_CONTRACT) contract_refusals++;
            tick_prefix(tick); serial_puts("rogue proposal: morph D (quality 30) -> ");
            serial_puts(loom_result_str(r)); serial_puts("\n");
            refused++;
        }

        if (loom_field_overcommitted(&field)) {
            video.stall_ticks++;
            stalls++;
            tick_prefix(tick); serial_puts("stall: field over capacity\n");
        } else {
            video.stall_ticks = 0;
            uint32_t frames = get32(video.state.data) + 1;
            put32(video.state.data, frames);
            put32(video.state.data + 4, frames);
            video.frames++;
            assert(video.frames == frames);
        }
    }

    serial_puts("summary: "); serial_putu64(TICKS); serial_puts(" ticks, ");
    serial_putu64(video.frames); serial_puts(" frames, "); serial_putu64(stalls); serial_puts(" stalls, ");
    serial_putu64(transitions); serial_puts(" transitions, "); serial_putu64(refused); serial_puts(" refused, ");
    serial_putu64((uint64_t)contract_refusals); serial_puts(" contract refusal(s)\n");

    bool ok = (video.frames == TICKS) && stalls == 0 && contract_refusals == 1;
    loom_continuity_stop(&field, &video);
    return ok;
}
