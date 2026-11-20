/* Resource Field accounting.
 *
 * Why every kind is checked: a demand fits only if every kind fits. The kinds are not
 * interchangeable, so spare CPU never pays for missing GPU. */
#include <assert.h>
#include <stdint.h>
#include "loom.h"

static void demand_zero(loom_demand_t *d) {
    for (int k = 0; k < LOOM_RES_COUNT; k++) d->amount[k] = 0;
}

void loom_field_init(loom_field_t *f, const loom_demand_t *capacity) {
    f->capacity = *capacity;
    demand_zero(&f->reserved);
}

bool loom_field_fits(const loom_field_t *f, const loom_demand_t *d) {
    for (int k = 0; k < LOOM_RES_COUNT; k++) {
        if ((uint64_t)f->reserved.amount[k] + d->amount[k] > f->capacity.amount[k]) return false;
    }
    return true;
}

/* Would the field fit if `old` were given back and `new_d` taken instead? Used by the Weaver to
 * ask about a change without making it. `old` is always part of `reserved`, so the subtraction
 * cannot underflow. */
bool loom_field_fits_swap(const loom_field_t *f, const loom_demand_t *old, const loom_demand_t *new_d) {
    for (int k = 0; k < LOOM_RES_COUNT; k++) {
        int64_t after = (int64_t)f->reserved.amount[k] - old->amount[k] + new_d->amount[k];
        if (after > (int64_t)f->capacity.amount[k]) return false;
    }
    return true;
}

/* True when some kind is over capacity, for example after a device is withdrawn. Existing
 * reservations are not revoked by the kernel; the Weaver must move Continuities out. */
bool loom_field_overcommitted(const loom_field_t *f) {
    for (int k = 0; k < LOOM_RES_COUNT; k++) {
        if (f->reserved.amount[k] > f->capacity.amount[k]) return true;
    }
    return false;
}

loom_result_t loom_field_reserve(loom_field_t *f, const loom_demand_t *d) {
    if (!loom_field_fits(f, d)) return LOOM_ERR_NO_RESOURCES;   /* checked first: all or nothing */
    for (int k = 0; k < LOOM_RES_COUNT; k++) f->reserved.amount[k] += d->amount[k];
    return LOOM_OK;
}

void loom_field_release(loom_field_t *f, const loom_demand_t *d) {
    for (int k = 0; k < LOOM_RES_COUNT; k++) {
        assert(f->reserved.amount[k] >= d->amount[k]);   /* releasing more than was held is a bug */
        f->reserved.amount[k] -= d->amount[k];
    }
}

/* Rollback puts back a reservation that was held a moment ago. It skips the admission check,
 * because the field may be over capacity (the reason for the transition in the first place). */
void loom_field_restore(loom_field_t *f, const loom_demand_t *d) {
    for (int k = 0; k < LOOM_RES_COUNT; k++) f->reserved.amount[k] += d->amount[k];
}
