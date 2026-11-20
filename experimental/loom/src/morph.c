/* Morphology helpers. */
#include <assert.h>
#include "loom.h"

/* A Morphology's demand is the sum of its Cells. Summing is only done inside one kind, so the
 * loop adds amounts of the same unit. */
void loom_morph_finalize(loom_morph_t *m) {
    assert(m->cell_count <= LOOM_MAX_CELLS);
    assert(m->quality <= 100);
    for (int k = 0; k < LOOM_RES_COUNT; k++) m->demand.amount[k] = 0;
    for (uint32_t i = 0; i < m->cell_count; i++) {
        for (int k = 0; k < LOOM_RES_COUNT; k++) m->demand.amount[k] += m->cells[i].demand.amount[k];
    }
}

const char *loom_result_str(loom_result_t r) {
    switch (r) {
    case LOOM_OK:                return "ok";
    case LOOM_ERR_BAD_TARGET:    return "no such morphology";
    case LOOM_ERR_SAME:          return "already running that morphology";
    case LOOM_ERR_NOT_RUNNING:   return "continuity is not running";
    case LOOM_ERR_ALREADY_RUNNING: return "continuity is already running";
    case LOOM_ERR_CONTRACT:      return "would break the correctness contract";
    case LOOM_ERR_CAPABILITY:    return "missing capability";
    case LOOM_ERR_NO_RESOURCES:  return "not admitted: resources unavailable";
    case LOOM_ERR_MIGRATION:     return "state could not be migrated";
    }
    return "unknown";
}
