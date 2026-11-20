/* The Weaver's proposal rule.
 *
 * This is read-only. It looks at the Field and the Continuity and names the best Morphology it
 * could run right now. It cannot change anything: loom_transition() makes the decision and applies
 * the same checks to the proposal. A bad proposal is refused, not obeyed.
 *
 * "Best" means highest quality, then lowest CPU demand, then the earliest declared. The rule is
 * deliberately simple; it is a policy, and policy is what the Weaver is meant to be free to change. */
#include "loom.h"

/* Works for a Continuity that is not running yet: it then holds no reservation, so the
 * swap check is simply "does the new demand fit now?". That is how the initial choice is made. */
int loom_weaver_propose(const loom_field_t *f, const loom_continuity_t *c) {
    int best = -1;
    for (uint32_t i = 0; i < c->morph_count; i++) {
        const loom_morph_t *m = &c->morphs[i];
        if (!loom_contract_allows(&c->contract, m->quality)) continue;
        if (m->required_caps & ~c->cap_authority) continue;
        if (!loom_field_fits_swap(f, &c->reserved, &m->demand)) continue;
        if (best < 0) { best = (int)i; continue; }
        const loom_morph_t *b = &c->morphs[best];
        if (m->quality > b->quality) {
            best = (int)i;
        } else if (m->quality == b->quality &&
                   m->demand.amount[LOOM_RES_CPU] < b->demand.amount[LOOM_RES_CPU]) {
            best = (int)i;
        }
    }
    return best;
}
