/* scrollbar-unit.c — host checks for the shared scrollbar API (kernel/src/scrollbar.c).
 *
 * The API is pure arithmetic (rows in, pixels and moves out), so it can be tested
 * without a boot: thumb size and place, clamping, and what each part of a click does. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "scrollbar.h"

static int failures = 0;

static void check(bool ok, const char *what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

/* A pane showing 10 of 100 rows, in a 114 px bar with 14 px arrows. */
static scroll_state_t pane(uint32_t visible, uint32_t total) {
    scroll_state_t s;
    scroll_init(&s, visible);
    scroll_set_total(&s, total);
    return s;
}

static void test_geometry(void) {
    scroll_state_t s = pane(10, 100);
    scrollbar_geom_t g;
    scrollbar_geometry(&s, 100, 50, 14, 114, 14, &g);
    check(g.track_y == 64 && g.track_h == 86, "track sits between the two arrows");
    /* thumb height = track * visible / total = 86*10/100 = 8, raised to the minimum 14 */
    check(g.thumb_h == 14, "thumb is never smaller than the grabbable minimum");
    check(g.thumb_y == g.track_y, "a list at the top puts the thumb at the track start");

    s.top = s.total - s.visible;           /* scrolled to the end */
    scrollbar_geometry(&s, 100, 50, 14, 114, 14, &g);
    check(g.thumb_y + g.thumb_h == g.track_y + g.track_h, "the last row puts the thumb at the end");

    /* A short list fills the track and does not move. */
    s = pane(10, 4);
    scrollbar_geometry(&s, 100, 50, 14, 114, 14, &g);
    check(g.thumb_h == g.track_h, "a list that fits fills the track");
    check(g.thumb_y == g.track_y, "a list that fits starts at the top");
}

static void test_moves(void) {
    scroll_state_t s = pane(10, 100);
    check(scroll_rows(&s, 1) && s.top == 1, "one row down");
    check(scroll_rows(&s, -1) && s.top == 0, "one row up");
    check(!scroll_rows(&s, -1) && s.top == 0, "cannot scroll above the first row");
    s.top = 90;
    check(!scroll_rows(&s, 1) && s.top == 90, "cannot scroll past the last row");

    s = pane(10, 100);
    check(scroll_page(&s, 1) && s.top == 9, "a page down stops one row short of the end");
    check(scroll_page(&s, -1) && s.top == 0, "a page up returns to the top");

    s = pane(10, 100);
    s.top = 500;                            /* stale: the document shrank */
    scroll_set_total(&s, 12);
    check(s.top == 2, "a shrunken document clamps top to total-visible");

    s = pane(0, 100);                       /* a pane too small to show a row */
    scroll_set_total(&s, 100);
    check(s.top == 0, "an empty pane stays at the top");
}

static void test_click(void) {
    scroll_state_t s = pane(10, 100);
    scrollbar_geom_t g;
    scrollbar_geometry(&s, 100, 50, 14, 114, 14, &g);   /* track_y 64, track_h 86 */

    check(!scrollbar_click(&s, &g, 50, 50), "a point above the bar does nothing");
    check(!scrollbar_click(&s, &g, 90, 70), "a point left of the bar does nothing");
    check(!scrollbar_click(&s, &g, 105, g.y + 2) && s.top == 0,
          "the up arrow at the top does nothing");
    s.top = 3;
    scrollbar_geometry(&s, 100, 50, 14, 114, 14, &g);
    check(scrollbar_click(&s, &g, 105, g.y + 2) && s.top == 2, "the up arrow steps one row");
    check(scrollbar_click(&s, &g, 105, g.y + g.h - 2) && s.top == 3, "the down arrow steps back");

    s = pane(10, 100);
    s.top = 40;
    scrollbar_geometry(&s, 100, 50, 14, 114, 14, &g);
    check(scrollbar_click(&s, &g, 105, g.track_y + 2) && s.top == 31,
          "the track above the thumb pages up");
    s = pane(10, 100);
    s.top = 40;
    scrollbar_geometry(&s, 100, 50, 14, 114, 14, &g);
    check(scrollbar_click(&s, &g, 105, g.track_y + g.track_h - 2) && s.top == 49,
          "the track below the thumb pages down");
    s = pane(10, 100);
    s.top = 90;
    scrollbar_geometry(&s, 100, 50, 14, 114, 14, &g);
    scrollbar_click(&s, &g, 105, g.track_y + g.track_h - 2);
    check(s.top == 90, "the track below the thumb stops at the end");

    s = pane(10, 100);
    scrollbar_geometry(&s, 100, 50, 14, 114, 14, &g);
    check(!scrollbar_click(&s, &g, 105, g.thumb_y + 2), "clicking the thumb itself does not jump");
}

int main(void) {
    test_geometry();
    test_moves();
    test_click();
    printf(failures == 0 ? "\nAll scrollbar checks passed\n" : "\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
