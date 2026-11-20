/* Continuity lifecycle and Morphology transitions.
 *
 * A transition runs these checks in order. The first failure refuses the transition and leaves
 * the Continuity, its state and the Field as they were:
 *   1. the target exists and is not the current Morphology
 *   2. the correctness contract allows the target's quality
 *   3. the Continuity holds every capability the target needs
 *   4. the Field admits the target's demand (old reservation given back first)
 *   5. the state migrates to the target's schema (on a copy)
 * Only then does the Continuity switch. Steps 4 and 5 roll back on failure. */
#include <assert.h>
#include <string.h>
#include "loom.h"

static loom_result_t migrate_state(loom_continuity_t *c, uint32_t to) {
    if (c->state.schema == to) return LOOM_OK;
    if (c->migrate == NULL) return LOOM_ERR_MIGRATION;
    loom_state_t scratch = c->state;                 /* migrate a copy; the live state stays put on failure */
    if (!c->migrate(c->migrate_ctx, c->state.schema, to, &scratch)) return LOOM_ERR_MIGRATION;
    scratch.schema = to;
    c->state = scratch;
    return LOOM_OK;
}

loom_result_t loom_continuity_start(loom_field_t *f, loom_continuity_t *c, uint32_t initial) {
    if (c->running) return LOOM_ERR_ALREADY_RUNNING;
    if (initial >= c->morph_count) return LOOM_ERR_BAD_TARGET;
    const loom_morph_t *m = &c->morphs[initial];
    if (!loom_contract_allows(&c->contract, m->quality)) return LOOM_ERR_CONTRACT;
    if (m->required_caps & ~c->cap_authority) return LOOM_ERR_CAPABILITY;
    loom_result_t r = loom_field_reserve(f, &m->demand);
    if (r != LOOM_OK) return r;
    loom_result_t mg = migrate_state(c, m->state_schema);
    if (mg != LOOM_OK) {
        loom_field_release(f, &m->demand);
        return mg;
    }
    c->reserved = m->demand;
    c->current = initial;
    c->running = true;
    c->stall_ticks = 0;
    return LOOM_OK;
}

void loom_continuity_stop(loom_field_t *f, loom_continuity_t *c) {
    if (!c->running) return;
    loom_field_release(f, &c->reserved);
    memset(&c->reserved, 0, sizeof c->reserved);
    c->running = false;
}

loom_result_t loom_transition(loom_field_t *f, loom_continuity_t *c, uint32_t target) {
    if (!c->running) return LOOM_ERR_NOT_RUNNING;
    if (target >= c->morph_count) return LOOM_ERR_BAD_TARGET;
    if (target == c->current) return LOOM_ERR_SAME;

    const loom_morph_t *to = &c->morphs[target];
    if (!loom_contract_allows(&c->contract, to->quality)) return LOOM_ERR_CONTRACT;
    if (to->required_caps & ~c->cap_authority) return LOOM_ERR_CAPABILITY;

    /* Admission: give back the old reservation, take the new one. If that fails, restore the
     * old one exactly as it was. */
    loom_field_release(f, &c->reserved);
    if (loom_field_reserve(f, &to->demand) != LOOM_OK) {
        loom_field_restore(f, &c->reserved);
        return LOOM_ERR_NO_RESOURCES;
    }

    loom_result_t mg = migrate_state(c, to->state_schema);
    if (mg != LOOM_OK) {
        loom_field_release(f, &to->demand);
        loom_field_restore(f, &c->reserved);
        return mg;
    }

    c->reserved = to->demand;
    c->current = target;
    return LOOM_OK;
}
