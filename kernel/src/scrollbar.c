/* scrollbar.c — the arithmetic behind a vertical scrollbar (see scrollbar.h).
 *
 * The thumb's height is the fraction of the document that is on screen, and its
 * position is that of the first visible row within the rows that can scroll. Both are
 * integer maths over a track that is a few dozen pixels long, so the thumb is clamped to
 * a grabbable minimum rather than allowed to vanish. */
#include "scrollbar.h"

#define SB_MIN_THUMB 14u   /* never smaller: a thumb must stay grabbable */

void scroll_init(scroll_state_t *s, uint32_t visible) {
    s->top = 0;
    s->total = 0;
    s->visible = visible;
}

void scroll_clamp(scroll_state_t *s) {
    uint32_t max_top = s->total > s->visible ? s->total - s->visible : 0;
    if (s->top > max_top) s->top = max_top;
}

void scroll_set_total(scroll_state_t *s, uint32_t total) {
    s->total = total;
    scroll_clamp(s);
}

bool scroll_rows(scroll_state_t *s, int32_t delta) {
    int64_t next = (int64_t)s->top + delta;
    if (next < 0) next = 0;
    scroll_set_total(s, s->total);          /* clamps total against a stale pane height */
    uint32_t max_top = s->total > s->visible ? s->total - s->visible : 0;
    if (next > (int64_t)max_top) next = (int64_t)max_top;
    if ((uint32_t)next == s->top) return false;
    s->top = (uint32_t)next;
    return true;
}

bool scroll_page(scroll_state_t *s, int32_t dir) {
    int32_t step = s->visible > 1 ? (int32_t)s->visible - 1 : 1;
    return scroll_rows(s, dir >= 0 ? step : -step);
}

void scrollbar_geometry(const scroll_state_t *s, uint32_t x, uint32_t y, uint32_t w,
                        uint32_t h, uint32_t arrow_h, scrollbar_geom_t *g) {
    g->x = x;
    g->y = y;
    g->w = w;
    g->h = h;
    g->arrow_h = arrow_h < h / 2u ? arrow_h : h / 2u;
    g->track_y = y + g->arrow_h;
    g->track_h = h > 2u * g->arrow_h ? h - 2u * g->arrow_h : 0;

    if (s->total <= s->visible || g->track_h == 0) {
        g->thumb_y = g->track_y;              /* nothing to scroll: the thumb fills it */
        g->thumb_h = g->track_h;
        return;
    }
    uint32_t th = (g->track_h * s->visible) / s->total;
    if (th < SB_MIN_THUMB) th = SB_MIN_THUMB;
    if (th > g->track_h) th = g->track_h;
    g->thumb_h = th;

    uint32_t max_top = s->total - s->visible;
    uint32_t room = g->track_h - th;
    g->thumb_y = g->track_y + (max_top > 0 ? (room * s->top) / max_top : 0u);
}

bool scrollbar_click(scroll_state_t *s, const scrollbar_geom_t *g, uint32_t cx, uint32_t cy) {
    if (cx < g->x || cx >= g->x + g->w || cy < g->y || cy >= g->y + g->h) return false;
    if (cy < g->y + g->arrow_h) return scroll_rows(s, -1);
    if (cy >= g->y + g->h - g->arrow_h) return scroll_rows(s, 1);

    if (cy < g->thumb_y) return scroll_page(s, -1);
    if (cy >= g->thumb_y + g->thumb_h) return scroll_page(s, 1);

    /* On the thumb: a click there does not jump the list. Dragging it is not done. */
    return false;
}
