/* Registry: the kernel-side table of Continuities and the Field they draw on. */
#include <stddef.h>
#include "registry.h"
#include <assert.h>

static loom_field_t *g_field;
static loom_continuity_t *g_conts[REG_MAX_CONTINUITIES];
static struct reg_record g_log[REG_LOG_MAX];
static uint32_t g_log_n;

void registry_bind_field(loom_field_t *f) { g_field = f; }

void registry_add(uint8_t index, loom_continuity_t *c) {
    assert(index < REG_MAX_CONTINUITIES);
    g_conts[index] = c;
}

loom_result_t registry_propose(uint8_t index, uint32_t target) {
    loom_continuity_t *c = (index < REG_MAX_CONTINUITIES) ? g_conts[index] : NULL;
    if (c == NULL || g_field == NULL) return LOOM_ERR_BAD_TARGET;
    uint32_t from = c->current;
    loom_result_t r = loom_transition(g_field, c, target);
    if (g_log_n < REG_LOG_MAX) {
        g_log[g_log_n++] = (struct reg_record){ index, from, target, r };
    }
    return r;
}

uint32_t registry_log_count(void) { return g_log_n; }
const struct reg_record *registry_log_at(uint32_t i) { return &g_log[i]; }
void registry_log_clear(void) { g_log_n = 0; }
