/* LOOM host tests. Each check prints PASS or FAIL. The exit status is 0 only if every check passed. */
#include <stdio.h>
#include <string.h>
#include "loom.h"

static int g_checks, g_failed;

#define CHECK(cond, msg) do {                                   \
        g_checks++;                                             \
        if (cond) printf("PASS: %s\n", msg);                    \
        else { g_failed++; printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
    } while (0)

#define CAP_FAST (1u << 0)

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* Migration for the test: schema 1 is 8 bytes, schema 2 is 12 bytes with an extra word.
 * ctx is a flag; when set, the next migration fails once. */
static bool test_migrate(void *ctx, uint32_t from, uint32_t to, loom_state_t *s) {
    int *fail = ctx;
    (void)from;
    if (*fail) { *fail = 0; return false; }
    s->len = (to == 1) ? 8u : 12u;
    if (to == 2) put32(s->data + 8, 0x2222);
    return true;
}

/* A two-Morphology Continuity: "fast" needs CAP_FAST and a GPU; "slow" needs only CPU. */
static void make_cont(loom_continuity_t *c, uint32_t caps, int *fail) {
    memset(c, 0, sizeof *c);
    c->name = "test";
    c->contract.correctness = LOOM_CONTRACT_DEGRADABLE;
    c->contract.min_quality = 50;
    c->cap_authority = caps;
    c->morph_count = 2;

    loom_morph_t *fast = &c->morphs[0];
    fast->name = "fast"; fast->quality = 100; fast->state_schema = 2; fast->required_caps = CAP_FAST;
    fast->cell_count = 1;
    fast->cells[0] = (loom_cell_t){ "gpu-path", {{ [LOOM_RES_CPU] = 300, [LOOM_RES_GPU] = 600 }} };
    loom_morph_finalize(fast);

    loom_morph_t *slow = &c->morphs[1];
    slow->name = "slow"; slow->quality = 60; slow->state_schema = 1;
    slow->cell_count = 1;
    slow->cells[0] = (loom_cell_t){ "cpu-path", {{ [LOOM_RES_CPU] = 1500 }} };
    loom_morph_finalize(slow);

    c->migrate = test_migrate;
    c->migrate_ctx = fail;
    c->state.schema = 2;
    c->state.len = 12;
    memset(c->state.data, 0, sizeof c->state.data);
    put32(c->state.data, 41);        /* frames */
    put32(c->state.data + 4, 40);    /* reference frame */
    put32(c->state.data + 8, 0x2222);
}

static loom_field_t make_field(uint32_t cpu, uint32_t gpu) {
    loom_demand_t cap = { { 0 } };
    cap.amount[LOOM_RES_CPU] = cpu;
    cap.amount[LOOM_RES_GPU] = gpu;
    loom_field_t f;
    loom_field_init(&f, &cap);
    return f;
}

static void test_field_all_or_nothing(void) {
    loom_field_t f = make_field(1000, 0);
    loom_demand_t d = { { 0 } };
    d.amount[LOOM_RES_CPU] = 600;
    CHECK(loom_field_reserve(&f, &d) == LOOM_OK, "reserve 600 of 1000 CPU");
    CHECK(loom_field_reserve(&f, &d) == LOOM_ERR_NO_RESOURCES, "second 600 CPU refused (only 400 left)");
    CHECK(f.reserved.amount[LOOM_RES_CPU] == 600, "refused reserve changes nothing");
}

static void test_kinds_not_interchangeable(void) {
    loom_field_t f = make_field(4000, 0);             /* plenty of CPU, no GPU */
    loom_demand_t d = { { 0 } };
    d.amount[LOOM_RES_GPU] = 600;
    CHECK(!loom_field_fits(&f, &d), "GPU demand not satisfied by spare CPU");
}

static void test_overcommit_detected(void) {
    loom_field_t f = make_field(4000, 1000);
    loom_demand_t d = { { 0 } };
    d.amount[LOOM_RES_GPU] = 600;
    loom_field_reserve(&f, &d);
    CHECK(!loom_field_overcommitted(&f), "no overcommit at full capacity");
    f.capacity.amount[LOOM_RES_GPU] = 0;
    CHECK(loom_field_overcommitted(&f), "overcommit detected after GPU is withdrawn");
}

static void test_contract_rule(void) {
    loom_contract_t deg = { LOOM_CONTRACT_DEGRADABLE, 50, 0 };
    loom_contract_t exact = { LOOM_CONTRACT_EXACT, 0, 0 };
    CHECK(loom_contract_allows(&deg, 60), "degradable: 60 allowed when minimum is 50");
    CHECK(!loom_contract_allows(&deg, 30), "degradable: 30 refused when minimum is 50");
    CHECK(!loom_contract_allows(&exact, 99), "exact: 99 refused");
    CHECK(loom_contract_allows(&exact, 100), "exact: 100 allowed");
}

static void test_start_and_transition_keeps_state(void) {
    int fail = 0;
    loom_continuity_t c;
    make_cont(&c, CAP_FAST, &fail);
    loom_field_t f = make_field(4000, 1000);

    CHECK(loom_continuity_start(&f, &c, 0) == LOOM_OK, "start in fast morphology");
    CHECK(f.reserved.amount[LOOM_RES_GPU] == 600, "fast holds 600 GPU");
    CHECK(loom_transition(&f, &c, 1) == LOOM_OK, "transition fast -> slow succeeds");
    CHECK(f.reserved.amount[LOOM_RES_GPU] == 0 && f.reserved.amount[LOOM_RES_CPU] == 1500,
          "after transition: GPU released, CPU 1500 held");
    CHECK(c.state.schema == 1 && c.state.len == 8, "state migrated to schema 1 (shorter)");
    CHECK(get32(c.state.data) == 41 && get32(c.state.data + 4) == 40,
          "frames and reference frame survive the transition");
    loom_continuity_stop(&f, &c);
    CHECK(f.reserved.amount[LOOM_RES_CPU] == 0 && f.reserved.amount[LOOM_RES_GPU] == 0,
          "stop releases every reservation");
}

/* The required demonstration of a rejected transition. */
static void test_invalid_transition_rejected(void) {
    int fail = 0;
    loom_continuity_t c;
    make_cont(&c, CAP_FAST, &fail);
    /* Add a third Morphology that breaks the contract: quality 30, minimum is 50. */
    loom_morph_t *bad = &c.morphs[2];
    bad->name = "bad"; bad->quality = 30; bad->state_schema = 1; bad->cell_count = 1;
    bad->cells[0] = (loom_cell_t){ "stub", {{ [LOOM_RES_CPU] = 50 }} };
    loom_morph_finalize(bad);
    c.morph_count = 3;

    loom_field_t f = make_field(4000, 1000);
    loom_continuity_start(&f, &c, 0);
    uint32_t cur = c.current, cpu = f.reserved.amount[LOOM_RES_CPU], gpu = f.reserved.amount[LOOM_RES_GPU];
    loom_state_t before = c.state;

    loom_result_t r = loom_transition(&f, &c, 2);
    CHECK(r == LOOM_ERR_CONTRACT, "invalid transition to the bad morphology is rejected (contract)");
    CHECK(c.current == cur, "rejected transition: current morphology unchanged");
    CHECK(f.reserved.amount[LOOM_RES_CPU] == cpu && f.reserved.amount[LOOM_RES_GPU] == gpu,
          "rejected transition: reservations unchanged");
    CHECK(c.state.schema == before.schema && c.state.len == before.len &&
          memcmp(c.state.data, before.data, sizeof before.data) == 0,
          "rejected transition: state bytes unchanged");
    CHECK(loom_transition(&f, &c, 9) == LOOM_ERR_BAD_TARGET, "transition to a nonexistent morphology is rejected");
    CHECK(loom_transition(&f, &c, cur) == LOOM_ERR_SAME, "transition to the current morphology is rejected");
    loom_continuity_stop(&f, &c);
}

static void test_capability_rejected(void) {
    int fail = 0;
    loom_continuity_t c;
    make_cont(&c, 0 /* no CAP_FAST */, &fail);
    loom_field_t f = make_field(4000, 1000);
    CHECK(loom_continuity_start(&f, &c, 0) == LOOM_ERR_CAPABILITY, "start in fast refused without CAP_FAST");
    CHECK(loom_continuity_start(&f, &c, 1) == LOOM_OK, "start in slow allowed");
    /* Now the transition up to fast should be refused for lacking the capability. */
    CHECK(loom_transition(&f, &c, 0) == LOOM_ERR_CAPABILITY, "transition to fast refused without CAP_FAST");
    loom_continuity_stop(&f, &c);
}

static void test_admission_failure_rolls_back(void) {
    int fail = 0;
    loom_continuity_t c;
    make_cont(&c, CAP_FAST, &fail);
    /* GPU is 1000 and fast needs 600; the second Continuity-sized GPU hog leaves only 300 GPU. */
    loom_field_t f = make_field(4000, 1000);
    loom_continuity_start(&f, &c, 1);                     /* start slow: CPU 1500, no GPU */
    loom_demand_t hog = { { 0 } };
    hog.amount[LOOM_RES_GPU] = 700;
    loom_field_reserve(&f, &hog);                         /* another user takes 700 of 1000 GPU */
    uint32_t cpu = f.reserved.amount[LOOM_RES_CPU], gpu = f.reserved.amount[LOOM_RES_GPU];

    CHECK(loom_transition(&f, &c, 0) == LOOM_ERR_NO_RESOURCES,
          "transition to fast refused when the GPU is short (600 needed, 300 left)");
    CHECK(c.current == 1, "after admission failure: still in slow");
    CHECK(f.reserved.amount[LOOM_RES_CPU] == cpu && f.reserved.amount[LOOM_RES_GPU] == gpu,
          "after admission failure: reservations restored exactly");
    loom_continuity_stop(&f, &c);
}

static void test_migration_failure_rolls_back_then_retries(void) {
    int fail = 0;
    loom_continuity_t c;
    make_cont(&c, CAP_FAST, &fail);
    loom_field_t f = make_field(4000, 1000);
    loom_continuity_start(&f, &c, 0);                     /* fast, schema 2 */
    loom_state_t before = c.state;
    uint32_t cpu = f.reserved.amount[LOOM_RES_CPU];

    fail = 1;                                             /* the next migration will fail */
    CHECK(loom_transition(&f, &c, 1) == LOOM_ERR_MIGRATION, "migration failure refuses the transition");
    CHECK(c.current == 0, "after migration failure: still in fast");
    CHECK(c.state.schema == before.schema && c.state.len == before.len &&
          memcmp(c.state.data, before.data, sizeof before.data) == 0,
          "after migration failure: state bytes unchanged");
    CHECK(f.reserved.amount[LOOM_RES_CPU] == cpu && f.reserved.amount[LOOM_RES_GPU] == 600,
          "after migration failure: reservations unchanged");
    CHECK(loom_transition(&f, &c, 1) == LOOM_OK, "retry after the failure succeeds");
    loom_continuity_stop(&f, &c);
}

static void test_weaver_proposals(void) {
    int fail = 0;
    loom_continuity_t c;
    make_cont(&c, CAP_FAST, &fail);

    loom_field_t f = make_field(4000, 1000);
    CHECK(loom_weaver_propose(&f, &c) == 0, "weaver picks fast when GPU and CPU are free");

    loom_continuity_start(&f, &c, 0);
    f.capacity.amount[LOOM_RES_GPU] = 0;                  /* the GPU is withdrawn */
    CHECK(loom_weaver_propose(&f, &c) == 1, "weaver moves to slow when the GPU is withdrawn");
    loom_continuity_stop(&f, &c);

    loom_field_t f2 = make_field(1000, 1000);             /* CPU too small for slow (1500) */
    CHECK(loom_weaver_propose(&f2, &c) == 0, "weaver keeps fast when slow does not fit");

    loom_field_t f3 = make_field(100, 0);                  /* nothing fits */
    CHECK(loom_weaver_propose(&f3, &c) == -1, "weaver proposes nothing when nothing fits");
}

static void test_weaver_never_proposes_contract_breaker(void) {
    int fail = 0;
    loom_continuity_t c;
    make_cont(&c, CAP_FAST, &fail);
    loom_morph_t *bad = &c.morphs[2];
    bad->name = "bad"; bad->quality = 30; bad->state_schema = 1; bad->cell_count = 1;
    bad->cells[0] = (loom_cell_t){ "stub", {{ [LOOM_RES_CPU] = 1 }} };
    loom_morph_finalize(bad);
    c.morph_count = 3;
    loom_field_t f = make_field(100, 0);                  /* only the 1-CPU "bad" morph fits */
    CHECK(loom_weaver_propose(&f, &c) == -1, "weaver never proposes a morphology that breaks the contract");
}

int main(void) {
    test_field_all_or_nothing();
    test_kinds_not_interchangeable();
    test_overcommit_detected();
    test_contract_rule();
    test_start_and_transition_keeps_state();
    test_invalid_transition_rejected();
    test_capability_rejected();
    test_admission_failure_rolls_back();
    test_migration_failure_rolls_back_then_retries();
    test_weaver_proposals();
    test_weaver_never_proposes_contract_breaker();

    printf("\nloom-test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
