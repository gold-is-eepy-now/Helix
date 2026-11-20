/* LOOM simulator: one video-playback Continuity under changing resource pressure.
 *
 * This is a model. The "frames" are counters, not decoded pictures, and the resources are numbers
 * in a table. It shows the decision flow: the Weaver proposes, the kernel rules check, the state
 * survives transitions, and a failed transition leaves everything as it was.
 *
 * Morphologies for the video Continuity (quality is a percent of the reference output):
 *   A  hw-decode        GPU decoder + compositor           quality 100, state schema 2, needs GPU_DECODE
 *   B  sw-decode        full CPU software decoder          quality 100, state schema 1
 *   C  half-res-sw      reduced-resolution CPU decoder     quality  60, state schema 3
 *   D  audio-only-stub  does not decode video at all       quality  30, state schema 1  (contract violation)
 *
 * Contract: DEGRADABLE, min quality 50, no stalls allowed (max_stall_ticks 0).
 * So C is a permitted degradation and D is an unacceptable failure. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "loom.h"

#define TICKS            32
#define CAP_GPU_DECODE   (1u << 0)

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* State layout. Bytes 0..7 are the same in every schema: frames delivered, and the reference
 * frame id. Later schemas add fields after them, so migrating keeps the playback position. */
#define STATE_BASE 8

/* Migration: schema 1 is base only; schema 2 adds a deblock flag; schema 3 adds a scale factor.
 * `ctx` points at a flag; when it is set, the next migration fails once (failure injection). */
static bool video_migrate(void *ctx, uint32_t from, uint32_t to, loom_state_t *s) {
    int *fail_once = ctx;
    (void)from;
    if (*fail_once) { *fail_once = 0; return false; }
    if (s->len < STATE_BASE) return false;
    s->len = STATE_BASE + (to == 1 ? 0u : 4u);
    if (to == 2) put32(s->data + STATE_BASE, 1);   /* deblock on */
    if (to == 3) put32(s->data + STATE_BASE, 2);   /* scale 1/2 */
    return true;
}

static void build_video(loom_continuity_t *c, int *fail_once) {
    memset(c, 0, sizeof *c);
    c->name = "video-playback";
    c->contract.correctness = LOOM_CONTRACT_DEGRADABLE;
    c->contract.min_quality = 50;
    c->contract.max_stall_ticks = 0;
    c->cap_authority = CAP_GPU_DECODE;   /* granted to this Continuity by the sim's policy */

    loom_morph_t *a = &c->morphs[0];
    a->name = "A hw-decode";
    a->quality = 100; a->state_schema = 2; a->required_caps = CAP_GPU_DECODE; a->cell_count = 2;
    a->cells[0] = (loom_cell_t){ "hw-decoder", {{ [LOOM_RES_CPU] = 200, [LOOM_RES_GPU] = 600, [LOOM_RES_MEM] = 65536, [LOOM_RES_POWER] = 3000 }} };
    a->cells[1] = (loom_cell_t){ "compositor", {{ [LOOM_RES_CPU] = 100, [LOOM_RES_POWER] = 1000 }} };

    loom_morph_t *b = &c->morphs[1];
    b->name = "B sw-decode";
    b->quality = 100; b->state_schema = 1; b->cell_count = 2;
    b->cells[0] = (loom_cell_t){ "sw-decoder", {{ [LOOM_RES_CPU] = 1500, [LOOM_RES_MEM] = 65536, [LOOM_RES_POWER] = 7000 }} };
    b->cells[1] = (loom_cell_t){ "output", {{ [LOOM_RES_CPU] = 200, [LOOM_RES_MEM_BW] = 4096, [LOOM_RES_POWER] = 1000 }} };

    loom_morph_t *cc = &c->morphs[2];
    cc->name = "C half-res-sw";
    cc->quality = 60; cc->state_schema = 3; cc->cell_count = 2;
    cc->cells[0] = (loom_cell_t){ "reduced-decoder", {{ [LOOM_RES_CPU] = 600, [LOOM_RES_MEM] = 32768, [LOOM_RES_POWER] = 4000 }} };
    cc->cells[1] = (loom_cell_t){ "output", {{ [LOOM_RES_CPU] = 100, [LOOM_RES_MEM_BW] = 2048, [LOOM_RES_POWER] = 1000 }} };

    loom_morph_t *d = &c->morphs[3];
    d->name = "D audio-only-stub";
    d->quality = 30; d->state_schema = 1; d->cell_count = 1;
    d->cells[0] = (loom_cell_t){ "stub", {{ [LOOM_RES_CPU] = 50 }} };

    c->morph_count = 4;
    for (uint32_t i = 0; i < c->morph_count; i++) loom_morph_finalize(&c->morphs[i]);

    c->migrate = video_migrate;
    c->migrate_ctx = fail_once;

    /* Initial state: schema 1, frames 0, reference frame 0. */
    c->state.schema = 1;
    c->state.len = STATE_BASE;
    memset(c->state.data, 0, sizeof c->state.data);
}

static const char *morph_name(const loom_continuity_t *c) { return c->morphs[c->current].name; }

static loom_result_t do_transition(loom_field_t *f, loom_continuity_t *c, uint32_t target, int tick, const char *why) {
    uint32_t frames0 = get32(c->state.data), ref0 = get32(c->state.data + 4);
    const char *from = morph_name(c);
    loom_result_t r = loom_transition(f, c, target);
    if (r == LOOM_OK) {
        /* The kernel's rule for state: a transition never resets the playback position. */
        assert(get32(c->state.data) == frames0 && get32(c->state.data + 4) == ref0);
        printf("t=%02d  transition %s -> %s  [%s]  state kept: frames=%u ref=%u schema=%u\n",
               tick, from, morph_name(c), why, frames0, ref0, c->state.schema);
    } else {
        /* A refused transition must not move the Continuity or change its state. */
        printf("t=%02d  transition refused: %s  [%s]  still %s\n",
               tick, loom_result_str(r), why, morph_name(c));
    }
    return r;
}

static const struct { uint32_t tick; loom_res_t res; uint32_t value; const char *why; } g_events[] = {
    { 8,  LOOM_RES_GPU, 0,    "GPU withdrawn by its driver (simulated fault)" },
    { 14, LOOM_RES_CPU, 1500, "thermal throttle: CPU budget drops to 1500 mCPU" },
    { 18, LOOM_RES_CPU, 4000, "throttle cleared" },
    { 26, LOOM_RES_GPU, 1000, "GPU returned" },
};

int main(void) {
    loom_demand_t cap = { { 0 } };
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

    /* Initial morphology: the Weaver proposes, the kernel starts it. */
    int pick = loom_weaver_propose(&field, &video);
    assert(pick >= 0);
    loom_result_t started = loom_continuity_start(&field, &video, (uint32_t)pick);
    assert(started == LOOM_OK);
    printf("t=00  start %s in %s\n", video.name, morph_name(&video));

    uint32_t stalls = 0, refused = 0, transitions = 0;
    int contract_violations = 0;
    size_t next_event = 0;

    for (uint32_t tick = 1; tick <= TICKS; tick++) {
        /* 1. Resource pressure arrives. */
        while (next_event < sizeof g_events / sizeof g_events[0] && g_events[next_event].tick == tick) {
            const char *what = g_events[next_event].why;
            field.capacity.amount[g_events[next_event].res] = g_events[next_event].value;
            printf("t=%02u  pressure: %s\n", tick, what);
            next_event++;
        }
        if (tick == 18) {
            fail_once = 1;
            printf("t=%02u  injecting one state-migration failure for the next transition\n", tick);
        }

        /* 2. The Weaver proposes; the kernel decides. Upgrades are proposed too, when resources
         *    return. Each proposal goes through loom_transition with the full checks. */
        int proposal = loom_weaver_propose(&field, &video);
        if (proposal >= 0 && (uint32_t)proposal != video.current) {
            const char *why = loom_field_overcommitted(&field) ? "repair after pressure" : "use returned resources";
            loom_result_t r = do_transition(&field, &video, (uint32_t)proposal, (int)tick, why);
            if (r == LOOM_OK) transitions++; else refused++;
        }

        /* 3. The Weaver also sends a bad proposal, as a misbehaving Weaver might. The kernel refuses it. */
        if (tick == 24) {
            uint32_t bad = 3;   /* morphology D: quality 30, below the contract minimum */
            loom_result_t r = loom_transition(&field, &video, bad);
            if (r == LOOM_ERR_CONTRACT) contract_violations++;
            printf("t=%02u  rogue proposal: morph D (quality 30) -> %s\n", tick, loom_result_str(r));
            refused++;
        }

        /* 4. The tick delivers a frame if the Continuity still holds its reservation; otherwise it stalls. */
        if (loom_field_overcommitted(&field)) {
            video.stall_ticks++;
            stalls++;
            printf("t=%02u  stall: field over capacity, no admissible morphology (%s)\n", tick, morph_name(&video));
            if (video.stall_ticks > video.contract.max_stall_ticks) {
                printf("t=%02u  TIMING CONTRACT VIOLATED: %u stall ticks in a row\n", tick, video.stall_ticks);
            }
        } else {
            video.stall_ticks = 0;
            uint32_t frames = get32(video.state.data) + 1;
            put32(video.state.data, frames);
            put32(video.state.data + 4, frames);
            video.frames++;
            assert(video.frames == frames);   /* the state's counter agrees with the kernel's */
        }
    }

    printf("\nsummary: %d ticks, %llu frames delivered, %u stalls, %u transitions, %u refused, %d contract refusal(s)\n",
           TICKS, (unsigned long long)video.frames, stalls, transitions, refused, contract_violations);
    printf("final: %s, reserved CPU %u mCPU, state frames=%u ref=%u schema=%u\n",
           morph_name(&video), video.reserved.amount[LOOM_RES_CPU],
           get32(video.state.data), get32(video.state.data + 4), video.state.schema);

    int ok = (video.frames == TICKS) && stalls == 0 && contract_violations == 1;
    printf("result: %s\n", ok ? "PASS" : "FAIL");
    loom_continuity_stop(&field, &video);
    return ok ? 0 : 1;
}
