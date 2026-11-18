/* scrollbar.h — one vertical scrollbar, shared by every window that scrolls.
 *
 * The API is rows, not pixels: a caller says how many rows exist and how many it can
 * show, and the scrollbar works out where the thumb belongs and what a click means.
 * Drawing stays with the caller, because only the caller knows the theme; this module
 * hands back the geometry it should draw. See kernel/src/scrollbar.c. */
#ifndef HELIX_SCROLLBAR_H
#define HELIX_SCROLLBAR_H

#include <stdbool.h>
#include <stdint.h>

/* How much of a list is showing, and where it starts. */
typedef struct {
    uint32_t top;       /* first row shown */
    uint32_t total;     /* rows the document has */
    uint32_t visible;   /* rows the pane can show at once */
} scroll_state_t;

/* Where the bar's parts sit on screen, in pixels. */
typedef struct {
    uint32_t x, y, w, h;         /* the whole bar, including its two arrows */
    uint32_t arrow_h;            /* height of each arrow button */
    uint32_t track_y, track_h;   /* the track between the arrows */
    uint32_t thumb_y, thumb_h;   /* the thumb inside the track */
} scrollbar_geom_t;

/* Start a pane that shows `visible` rows at a time. */
void scroll_init(scroll_state_t *s, uint32_t visible);

/* Record how many rows the document has and clamp `top` into range. */
void scroll_set_total(scroll_state_t *s, uint32_t total);

/* Keep top within 0 .. total-visible (nothing if the list fits). */
void scroll_clamp(scroll_state_t *s);

/* Move by `delta` rows (negative goes up). False when nothing moved. */
bool scroll_rows(scroll_state_t *s, int32_t delta);

/* Move by one paneful, negative for up. False when nothing moved. */
bool scroll_page(scroll_state_t *s, int32_t dir);

/* Work out where the bar's track and thumb go inside the box (x, y, w, h). */
void scrollbar_geometry(const scroll_state_t *s, uint32_t x, uint32_t y, uint32_t w,
                        uint32_t h, uint32_t arrow_h, scrollbar_geom_t *g);

/* A click at (cx, cy): an arrow steps one row, the track pages towards the click.
 * False when the point is outside the bar (nothing moved). */
bool scrollbar_click(scroll_state_t *s, const scrollbar_geom_t *g, uint32_t cx, uint32_t cy);

#endif /* HELIX_SCROLLBAR_H */
