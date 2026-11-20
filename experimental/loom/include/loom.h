/* LOOM core data structures and kernel-side rules.
 *
 * LOOM is an experiment, not part of Helix's kernel. This header describes the model:
 *   Continuity  a requested behaviour with a contract (what it must deliver, how well, how soon)
 *   Morphology  one way of implementing a Continuity: a set of Cells with a resource demand
 *   Cell        one isolated unit inside a Morphology, with its own demand
 *   Field       the machine's resources, one unit per kind, never converted between kinds
 *   Weaver      proposes a Morphology; the kernel rules below decide whether it happens
 *
 * Everything in this header is host-side C. Nothing here touches hardware.
 */
#ifndef LOOM_H
#define LOOM_H

#include <stdbool.h>
#include <stdint.h>

#define LOOM_MAX_MORPHS       4
#define LOOM_MAX_CELLS        4
#define LOOM_STATE_MAX        64      /* bytes of Continuity state carried across transitions */

/* Resource kinds. Each has its own unit. There is no conversion function between kinds, so a
 * Morphology that needs GPU time cannot be satisfied by spare CPU time. */
typedef enum {
    LOOM_RES_CPU = 0,     /* milli-CPUs: 1000 is one full CPU */
    LOOM_RES_GPU,         /* milli-GPUs: 1000 is one full GPU */
    LOOM_RES_MEM,         /* KiB held */
    LOOM_RES_MEM_BW,      /* KiB moved per tick */
    LOOM_RES_STORAGE_BW,  /* KiB per tick (not simulated yet) */
    LOOM_RES_NET_BW,      /* KiB per tick (not simulated yet) */
    LOOM_RES_DMA,         /* channels (not simulated yet) */
    LOOM_RES_POWER,       /* mW */
    LOOM_RES_THERMAL,     /* milli-degrees of headroom (not simulated yet) */
    LOOM_RES_COUNT
} loom_res_t;

typedef struct {
    uint32_t amount[LOOM_RES_COUNT];
} loom_demand_t;

typedef struct {
    loom_demand_t capacity;   /* what the machine can provide right now */
    loom_demand_t reserved;   /* what admitted Continuities hold */
} loom_field_t;

typedef struct {
    const char *name;
    loom_demand_t demand;
} loom_cell_t;

typedef struct {
    const char *name;
    uint32_t quality;            /* percent of the Continuity's reference output, 0..100 */
    uint32_t state_schema;       /* layout version of the state this Morphology reads and writes */
    uint32_t required_caps;      /* capabilities the Continuity must hold to use this Morphology */
    uint32_t cell_count;
    loom_cell_t cells[LOOM_MAX_CELLS];
    loom_demand_t demand;        /* sum of the cells; filled in by loom_morph_finalize() */
} loom_morph_t;

typedef enum {
    LOOM_CONTRACT_EXACT,         /* output must match the reference exactly (quality 100) */
    LOOM_CONTRACT_DEGRADABLE     /* quality may fall, but never below min_quality */
} loom_correctness_t;

typedef struct {
    loom_correctness_t correctness;
    uint32_t min_quality;        /* percent; used when DEGRADABLE */
    uint32_t max_stall_ticks;    /* timing: most ticks in a row with no delivered frame */
} loom_contract_t;

/* Continuity state. Its bytes are carried across Morphology transitions; the schema says how to
 * read them. A transition that cannot migrate the state is refused and the old state is kept. */
typedef struct {
    uint32_t schema;
    uint8_t data[LOOM_STATE_MAX];
    uint32_t len;
} loom_state_t;

/* Converts the state in place from one schema to another. Returns false if it cannot. The kernel
 * always passes a copy, so a false return leaves the live state untouched. */
typedef bool (*loom_migrate_fn)(void *ctx, uint32_t from, uint32_t to, loom_state_t *s);

typedef struct {
    const char *name;
    loom_contract_t contract;
    uint32_t cap_authority;      /* capabilities this Continuity holds */
    uint32_t morph_count;
    loom_morph_t morphs[LOOM_MAX_MORPHS];   /* the Morphologies this Continuity permits */

    /* Runtime state, owned by the kernel-side rules in continuity.c */
    bool running;
    uint32_t current;            /* index into morphs[] */
    loom_demand_t reserved;      /* what this Continuity holds in the field right now */
    loom_state_t state;
    loom_migrate_fn migrate;
    void *migrate_ctx;
    uint32_t stall_ticks;        /* consecutive ticks with no delivered frame */
    uint64_t frames;             /* frames delivered in total */
} loom_continuity_t;

typedef enum {
    LOOM_OK = 0,
    LOOM_ERR_BAD_TARGET,         /* no such Morphology in this Continuity */
    LOOM_ERR_SAME,               /* already running that Morphology */
    LOOM_ERR_NOT_RUNNING,
    LOOM_ERR_ALREADY_RUNNING,
    LOOM_ERR_CONTRACT,           /* Morphology would break the correctness contract */
    LOOM_ERR_CAPABILITY,         /* Continuity lacks a capability the Morphology needs */
    LOOM_ERR_NO_RESOURCES,       /* admission failed: the field cannot hold the new demand */
    LOOM_ERR_MIGRATION           /* state could not be carried to the new schema */
} loom_result_t;

const char *loom_result_str(loom_result_t r);

/* Morphology helpers (morph.c) */
void loom_morph_finalize(loom_morph_t *m);

/* Contract rule (contract.c) */
bool loom_contract_allows(const loom_contract_t *c, uint32_t quality);

/* Field accounting (field.c). Admission is all-or-nothing and checks every kind. */
void loom_field_init(loom_field_t *f, const loom_demand_t *capacity);
bool loom_field_fits(const loom_field_t *f, const loom_demand_t *d);
bool loom_field_fits_swap(const loom_field_t *f, const loom_demand_t *old, const loom_demand_t *new_d);
bool loom_field_overcommitted(const loom_field_t *f);
loom_result_t loom_field_reserve(loom_field_t *f, const loom_demand_t *d);
void loom_field_release(loom_field_t *f, const loom_demand_t *d);
void loom_field_restore(loom_field_t *f, const loom_demand_t *d);   /* rollback only */

/* Continuity lifecycle and transitions (continuity.c) */
loom_result_t loom_continuity_start(loom_field_t *f, loom_continuity_t *c, uint32_t initial);
void loom_continuity_stop(loom_field_t *f, loom_continuity_t *c);
loom_result_t loom_transition(loom_field_t *f, loom_continuity_t *c, uint32_t target);

/* Weaver (weaver.c). Read-only: it proposes, it cannot change anything. */
int loom_weaver_propose(const loom_field_t *f, const loom_continuity_t *c);

#endif
