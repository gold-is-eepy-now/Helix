/* The video-playback Continuity: contract and the four Morphologies. Shared by the kernel and the
 * Weaver Cell, so both reason about the same choices. The migration function is kernel-only and is
 * set by the kernel after this call. */
#ifndef LOOM_VIDEO_H
#define LOOM_VIDEO_H

#include "loom.h"

#define LOOM_VIDEO_CAP_GPU_DECODE  (1u << 0)

static inline void loom_video_define(loom_continuity_t *c) {
    c->name = "video-playback";
    c->contract.correctness = LOOM_CONTRACT_DEGRADABLE;
    c->contract.min_quality = 50;
    c->contract.max_stall_ticks = 0;
    c->cap_authority = LOOM_VIDEO_CAP_GPU_DECODE;

    loom_morph_t *a = &c->morphs[0];
    a->name = "A hw-decode"; a->quality = 100; a->state_schema = 2;
    a->required_caps = LOOM_VIDEO_CAP_GPU_DECODE; a->cell_count = 2;
    a->cells[0] = (loom_cell_t){ "hw-decoder", {{ [LOOM_RES_CPU] = 200, [LOOM_RES_GPU] = 600, [LOOM_RES_MEM] = 65536, [LOOM_RES_POWER] = 3000 }} };
    a->cells[1] = (loom_cell_t){ "compositor", {{ [LOOM_RES_CPU] = 100, [LOOM_RES_POWER] = 1000 }} };

    loom_morph_t *b = &c->morphs[1];
    b->name = "B sw-decode"; b->quality = 100; b->state_schema = 1; b->cell_count = 2;
    b->cells[0] = (loom_cell_t){ "sw-decoder", {{ [LOOM_RES_CPU] = 1500, [LOOM_RES_MEM] = 65536, [LOOM_RES_POWER] = 7000 }} };
    b->cells[1] = (loom_cell_t){ "output", {{ [LOOM_RES_CPU] = 200, [LOOM_RES_MEM_BW] = 4096, [LOOM_RES_POWER] = 1000 }} };

    loom_morph_t *cc = &c->morphs[2];
    cc->name = "C half-res-sw"; cc->quality = 60; cc->state_schema = 3; cc->cell_count = 2;
    cc->cells[0] = (loom_cell_t){ "reduced-decoder", {{ [LOOM_RES_CPU] = 600, [LOOM_RES_MEM] = 32768, [LOOM_RES_POWER] = 4000 }} };
    cc->cells[1] = (loom_cell_t){ "output", {{ [LOOM_RES_CPU] = 100, [LOOM_RES_MEM_BW] = 2048, [LOOM_RES_POWER] = 1000 }} };

    loom_morph_t *d = &c->morphs[3];
    d->name = "D audio-only-stub"; d->quality = 30; d->state_schema = 1; d->cell_count = 1;
    d->cells[0] = (loom_cell_t){ "stub", {{ [LOOM_RES_CPU] = 50 }} };

    c->morph_count = 4;
    for (uint32_t i = 0; i < c->morph_count; i++) loom_morph_finalize(&c->morphs[i]);
}

#endif
