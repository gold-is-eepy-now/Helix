/* Correctness contract rule.
 *
 * EXACT output must equal the reference (quality 100). DEGRADABLE output may fall, but never
 * below the declared minimum. The kernel checks this on every transition, so a Weaver proposal
 * cannot silently break a contract. */
#include "loom.h"

bool loom_contract_allows(const loom_contract_t *c, uint32_t quality) {
    if (quality > 100) return false;
    if (c->correctness == LOOM_CONTRACT_EXACT) return quality == 100;
    return quality >= c->min_quality;
}
