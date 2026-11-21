/* The kernel's registry of Continuities and the Resource Field. The Field and the Continuities here
 * are the authority: a Cell can ask for a transition (SYS_PROPOSE), and loom_transition decides. */
#ifndef LOOM_REGISTRY_H
#define LOOM_REGISTRY_H

#include <stdint.h>
#include "loom.h"

#define REG_MAX_CONTINUITIES 4
#define REG_LOG_MAX 8

/* One proposal the kernel handled: what was asked, and what the rules did. */
struct reg_record {
    uint8_t  cont;
    uint32_t from;
    uint32_t to;
    loom_result_t result;
};

void registry_bind_field(loom_field_t *f);
void registry_add(uint8_t index, loom_continuity_t *c);
loom_result_t registry_propose(uint8_t index, uint32_t target);   /* called by SYS_PROPOSE */

/* Proposals handled since the last clear. The demo reads them after each tick. */
uint32_t registry_log_count(void);
const struct reg_record *registry_log_at(uint32_t i);
void registry_log_clear(void);

#endif
