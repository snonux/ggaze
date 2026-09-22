/*:*
 * ggaze — crop rectangle geometry unit test (no display)
 *
 * Pins the rules croprect.h promises: a rectangle never leaves its image or
 * shrinks below the minimum, moves slide along the edge, resizes anchor on
 * the opposite edge, the aspect lock keeps the shape while fitting the
 * room, hits prefer corners, a drag is computed from its start rectangle,
 * and a quarter turn keeps covering the same pixels. Negative rules: an
 * edge dragged past the border never moves the opposite edge, NaN deltas
 * change nothing, a 0x0 image and a rectangle entirely outside are safe,
 * and the aspect lock holds at the minimum size.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "croprect.h"

#include <math.h>

#include <glib.h>

#define W 400.0
#define H 300.0

static void
assert_rect(const CropRect *p_r, gdouble d_x, gdouble d_y, gdouble d_w,
            gdouble d_h) {
   g_assert_cmpfloat(fabs(p_r->d_x - d_x), <, 1e-6);
   g_assert_cmpfloat(fabs(p_r->d_y - d_y), <, 1e-6);
   g_assert_cmpfloat(fabs(p_r->d_w - d_w), <, 1e-6);
   g_assert_cmpfloat(fabs(p_r->d_h - d_h), <, 1e-6);
}

static void
test_init_full_and_is_full(void) {
   CropRect r;
   croprect_init_full(&r, W, H);
   assert_rect(&r, 0, 0, W, H);
   g_assert_true(croprect_is_full(&r, W, H));
   r.d_w -= 1.0;
   g_assert_false(croprect_is_full(&r, W, H));
   CropRect r2 = {0.2, -0.3, W + 0.1, H - 0.4};
   g_assert_true(croprect_is_full(&r2, W, H)); /* within half a pixel */
   g_assert_false(croprect_equal(&r, &r2));
   g_assert_true(croprect_equal(&r, &r));
}

static void
test_clamp_keeps_inside_and_min_size(void) {
   CropRect r = {-10, -10, 500, 500};
   croprect_clamp(&r, W, H);
   assert_rect(&r, 0, 0, W, H);
   CropRect s = {390, 295, 2, 2}; /* too small, and grown past the edge */
   croprect_clamp(&s, W, H);
   /* Grows to the minimum; x still fits (390 + 8 <= 400), y does not. */
   assert_rect(&s, 390, H - CROPRECT_MIN_SIZE, CROPRECT_MIN_SIZE,
               CROPRECT_MIN_SIZE);
   CropRect t = {0, 0, 1, 1}; /* an image smaller than the minimum */
   croprect_clamp(&t, 6, 3);
   assert_rect(&t, 0, 0, 6, 3);
}

static void
test_intersect_is_a_plain_cut(void) {
   CropRect r = {-10, 5, 30, 400}; /* hangs off the left and the bottom */
   croprect_intersect(&r, W, H);
   assert_rect(&r, 0, 5, 20, H - 5);
   CropRect s = {390, 295, 2, 2}; /* smaller than the minimum: kept as is */
   croprect_intersect(&s, W, H);
   assert_rect(&s, 390, 295, 2, 2);
   CropRect t = {500, 0, 10, 10}; /* entirely outside: empty, not moved */
   croprect_intersect(&t, W, H);
   g_assert_cmpfloat(t.d_w, ==, 0);
   g_assert_cmpfloat(t.d_x, ==, W);
}

static void
test_move_slides_along_the_edge(void) {
   CropRect r = {100, 100, 100, 50};
   croprect_move(&r, 10, -20, W, H);
   assert_rect(&r, 110, 80, 100, 50);
   croprect_move(&r, 1000, 1000, W, H); /* stops at the far edge */
   assert_rect(&r, W - 100, H - 50, 100, 50);
   croprect_move(&r, -1000, 0, W, H);
   assert_rect(&r, 0, H - 50, 100, 50);
}

static void
test_resize_edges_anchor_opposite_edge(void) {
   CropRect r = {100, 100, 100, 50};
   croprect_resize(&r, CROPRECT_HIT_RIGHT, 20, 999, 0.0, W, H);
   assert_rect(&r, 100, 100, 120, 50); /* d_dy ignored on a vertical edge */
   croprect_resize(&r, CROPRECT_HIT_LEFT, -20, 0, 0.0, W, H);
   assert_rect(&r, 80, 100, 140, 50);
   croprect_resize(&r, CROPRECT_HIT_TOP, 0, -10, 0.0, W, H);
   assert_rect(&r, 80, 90, 140, 60);
   croprect_resize(&r, CROPRECT_HIT_BOTTOM, 0, 10, 0.0, W, H);
   assert_rect(&r, 80, 90, 140, 70);
   croprect_resize(&r, CROPRECT_HIT_BOTTOM_RIGHT, 10, 10, 0.0, W, H);
   assert_rect(&r, 80, 90, 150, 80);
   croprect_resize(&r, CROPRECT_HIT_TOP_LEFT, 10, 10, 0.0, W, H);
   assert_rect(&r, 90, 100, 140, 70);
   /* INSIDE / NONE are not resizes. */
   croprect_resize(&r, CROPRECT_HIT_INSIDE, 10, 10, 0.0, W, H);
   croprect_resize(&r, CROPRECT_HIT_NONE, 10, 10, 0.0, W, H);
   assert_rect(&r, 90, 100, 140, 70);
}

static void
test_resize_never_inverts_or_leaves(void) {
   CropRect r = {100, 100, 100, 50};
   croprect_resize(&r, CROPRECT_HIT_LEFT, 500, 0, 0.0, W, H);
   assert_rect(&r, 200 - CROPRECT_MIN_SIZE, 100, CROPRECT_MIN_SIZE, 50);
   CropRect s = {100, 100, 100, 50};
   croprect_resize(&s, CROPRECT_HIT_BOTTOM, 0, -500, 0.0, W, H);
   assert_rect(&s, 100, 100, 100, CROPRECT_MIN_SIZE);
   CropRect t = {100, 100, 100, 50};
   croprect_resize(&t, CROPRECT_HIT_RIGHT, 5000, 0, 0.0, W, H);
   assert_rect(&t, 100, 100, W - 100, 50);
}

static void
test_resize_with_aspect_keeps_shape(void) {
   CropRect r = {100, 100, 100, 50};
   croprect_resize(&r, CROPRECT_HIT_RIGHT, 20, 0, 2.0, W, H);
   /* width leads: 120 wide, 60 high, height centred on the old centre */
   assert_rect(&r, 100, 95, 120, 60);
   croprect_resize(&r, CROPRECT_HIT_BOTTOM, 0, 20, 2.0, W, H);
   /* height leads: 80 high, 160 wide, width centred */
   assert_rect(&r, 80, 95, 160, 80);
   croprect_resize(&r, CROPRECT_HIT_BOTTOM_RIGHT, 1000, 1000, 2.0, W, H);
   /* shrinks to the room its anchors (left/top fixed) leave: 240 wide would
    * be 120 high, but only 205 high fit -> both cut back */
   g_assert_cmpfloat(fabs(r.d_w / r.d_h - 2.0), <, 1e-6);
   g_assert_cmpfloat(r.d_x, ==, 80);
   g_assert_cmpfloat(r.d_y, ==, 95);
   g_assert_cmpfloat(r.d_x + r.d_w, <=, W + 1e-6);
   g_assert_cmpfloat(r.d_y + r.d_h, <=, H + 1e-6);
}

static void
test_set_aspect_refits_around_centre(void) {
   CropRect r = {100, 100, 200, 100};
   croprect_set_aspect(&r, 1.0, W, H);
   assert_rect(&r, 150, 100, 100, 100);
   croprect_set_aspect(&r, 0.0, W, H); /* free: unchanged */
   assert_rect(&r, 150, 100, 100, 100);
   CropRect s = {0, 0, W, H};
   croprect_set_aspect(&s, 16.0 / 9.0, W, H);
   g_assert_cmpfloat(fabs(s.d_w / s.d_h - 16.0 / 9.0), <, 1e-6);
   g_assert_cmpfloat(s.d_w, ==, W);
   CropRect t = {0, 0, W, H};
   croprect_set_aspect(&t, 0.5, W, H); /* taller than wide: height-bound */
   assert_rect(&t, W / 2 - H / 4, 0, H / 2, H);
}

static void
test_hit_prefers_corners_then_edges(void) {
   CropRect r = {100, 100, 100, 50};
   g_assert_cmpint(croprect_hit(&r, 102, 98, 5), ==, CROPRECT_HIT_TOP_LEFT);
   g_assert_cmpint(croprect_hit(&r, 199, 101, 5), ==, CROPRECT_HIT_TOP_RIGHT);
   g_assert_cmpint(croprect_hit(&r, 100, 150, 5), ==, CROPRECT_HIT_BOTTOM_LEFT);
   g_assert_cmpint(croprect_hit(&r, 200, 150, 5), ==,
                   CROPRECT_HIT_BOTTOM_RIGHT);
   g_assert_cmpint(croprect_hit(&r, 101, 125, 5), ==, CROPRECT_HIT_LEFT);
   g_assert_cmpint(croprect_hit(&r, 203, 125, 5), ==, CROPRECT_HIT_RIGHT);
   g_assert_cmpint(croprect_hit(&r, 150, 97, 5), ==, CROPRECT_HIT_TOP);
   g_assert_cmpint(croprect_hit(&r, 150, 152, 5), ==, CROPRECT_HIT_BOTTOM);
   g_assert_cmpint(croprect_hit(&r, 150, 125, 5), ==, CROPRECT_HIT_INSIDE);
   g_assert_cmpint(croprect_hit(&r, 50, 50, 5), ==, CROPRECT_HIT_NONE);
   g_assert_cmpint(croprect_hit(&r, 150, 90, 5), ==, CROPRECT_HIT_NONE);
}

static void
test_drag_is_computed_from_start(void) {
   CropRect start = {100, 100, 100, 50};
   CropRect r;
   croprect_drag(&r, &start, CROPRECT_HIT_INSIDE, 30, 10, 0.0, W, H);
   assert_rect(&r, 130, 110, 100, 50);
   croprect_drag(&r, &start, CROPRECT_HIT_INSIDE, 5, 5, 0.0, W, H);
   assert_rect(&r, 105, 105, 100, 50); /* not 135/115: not path-dependent */
   croprect_drag(&r, &start, CROPRECT_HIT_RIGHT, 40, 0, 0.0, W, H);
   assert_rect(&r, 100, 100, 140, 50);
   croprect_drag(&r, &start, CROPRECT_HIT_NONE, 40, 40, 0.0, W, H);
   assert_rect(&r, 100, 100, 100, 50);
   g_assert_true(croprect_equal(&r, &start));
}

static void
test_rotate_quarter_keeps_the_same_pixels(void) {
   /* A 400x300 image; the rect covers x 50..150, y 200..250. Clockwise the
    * old bottom (y=250..300 below it, 50 px) becomes the new left edge. */
   CropRect r = {50, 200, 100, 50};
   croprect_rotate_quarter(&r, 1, W, H);
   assert_rect(&r, H - 250, 50, 50, 100);
   croprect_rotate_quarter(&r, -1, H, W); /* the image is now 300x400 */
   assert_rect(&r, 50, 200, 100, 50);
   /* Four clockwise turns are the identity. */
   croprect_rotate_quarter(&r, 1, W, H);
   croprect_rotate_quarter(&r, 1, H, W);
   croprect_rotate_quarter(&r, 1, W, H);
   croprect_rotate_quarter(&r, 1, H, W);
   assert_rect(&r, 50, 200, 100, 50);
}

static void
test_round_snaps_edges(void) {
   CropRect r = {10.4, 20.6, 100.2, 50.9};
   croprect_round(&r);
   assert_rect(&r, 10, 21, 101, 51); /* right 110.6 -> 111, bottom 71.5 -> 72 */
   CropRect s = {10.4, 10.4, 0.2, 0.2};
   croprect_round(&s);
   assert_rect(&s, 10, 10, 1, 1); /* never collapses to nothing */
}

/* Dragging the LEFT / TOP edge (or the top-left corner) past the border is
 * an intersection: the edge stops at 0 and the OPPOSITE edge stays where it
 * was. The old clamp zeroed x and then re-clamped the width, which stretched
 * {50,50,20,20} into {0,50,100,20} -- the right edge jumped from 70 to 100.
 * Holds with and without the aspect lock. */
static void
test_overshoot_keeps_the_opposite_edge(void) {
   CropRect r = {50, 50, 20, 20};
   croprect_resize(&r, CROPRECT_HIT_LEFT, -100, 0, 0.0, 100, 100);
   assert_rect(&r, 0, 50, 70, 20);
   r = (CropRect){50, 50, 20, 20};
   croprect_resize(&r, CROPRECT_HIT_TOP, 0, -100, 0.0, 100, 100);
   assert_rect(&r, 50, 0, 20, 70);
   r = (CropRect){50, 50, 20, 20};
   croprect_resize(&r, CROPRECT_HIT_TOP_LEFT, -100, -100, 0.0, 100, 100);
   assert_rect(&r, 0, 0, 70, 70);
   /* Aspect 1:1, left edge: the width is the room the right edge leaves
    * (70), the height follows about the old centre. */
   r = (CropRect){50, 50, 20, 20};
   croprect_resize(&r, CROPRECT_HIT_LEFT, -100, 0, 1.0, 100, 100);
   assert_rect(&r, 0, 25, 70, 70);
   r = (CropRect){50, 50, 20, 20};
   croprect_resize(&r, CROPRECT_HIT_TOP_LEFT, -100, -100, 1.0, 100, 100);
   assert_rect(&r, 0, 0, 70, 70);
   /* The same through the drag entry point (what the pointer path calls). */
   CropRect start = {50, 50, 20, 20};
   croprect_drag(&r, &start, CROPRECT_HIT_LEFT, -100, 0, 0.0, 100, 100);
   assert_rect(&r, 0, 50, 70, 20);
   /* The clamp itself, on a rectangle hanging off the left/top. */
   r = (CropRect){-30, -30, 60, 60};
   croprect_clamp(&r, 100, 100);
   assert_rect(&r, 0, 0, 30, 30);
}

/* A NaN delta (a pointer event without coordinates) must never reach the
 * rectangle: move / resize / drag by NaN are no-ops, and a rectangle that
 * somehow holds NaN is repaired by the clamp rather than passed on. */
static void
test_nan_never_propagates(void) {
   CropRect r = {100, 100, 100, 50};
   croprect_move(&r, NAN, NAN, W, H);
   assert_rect(&r, 100, 100, 100, 50);
   croprect_resize(&r, CROPRECT_HIT_BOTTOM_RIGHT, NAN, NAN, 0.0, W, H);
   assert_rect(&r, 100, 100, 100, 50);
   croprect_resize(&r, CROPRECT_HIT_RIGHT, NAN, 0, 2.0, W, H);
   assert_rect(&r, 100, 100, 100, 50);
   croprect_set_aspect(&r, NAN, W, H); /* NaN aspect reads as "free" */
   assert_rect(&r, 100, 100, 100, 50);
   CropRect start = r;
   croprect_drag(&r, &start, CROPRECT_HIT_INSIDE, NAN, 5, 0.0, W, H);
   assert_rect(&r, 100, 105, 100, 50);
   CropRect bad = {NAN, 10, NAN, 20};
   croprect_clamp(&bad, W, H);
   g_assert_true(isfinite(bad.d_x) && isfinite(bad.d_w));
   assert_rect(&bad, 0, 10, W, 20);
   croprect_clamp(&r, NAN, NAN); /* a NaN image is an empty one */
   g_assert_true(isfinite(r.d_x) && isfinite(r.d_w));
}

/* A 0x0 image (nothing decoded yet) yields an empty rectangle that every
 * operation leaves empty and finite; a rectangle entirely outside, at
 * negative coordinates, is pulled back to the nearest corner at the minimum
 * size rather than left where the chain could not crop. */
static void
test_zero_image_and_outside_rect(void) {
   CropRect r;
   croprect_init_full(&r, 0, 0);
   assert_rect(&r, 0, 0, 0, 0);
   g_assert_true(croprect_is_full(&r, 0, 0));
   croprect_move(&r, 10, 10, 0, 0);
   croprect_resize(&r, CROPRECT_HIT_RIGHT, 10, 0, 1.0, 0, 0);
   croprect_set_aspect(&r, 1.0, 0, 0);
   assert_rect(&r, 0, 0, 0, 0);
   g_assert_cmpint(croprect_hit(&r, 0, 0, 5), ==, CROPRECT_HIT_TOP_LEFT);
   croprect_init_full(&r, -5, -5); /* negative sizes are empty too */
   assert_rect(&r, 0, 0, 0, 0);
   CropRect out = {-50, -50, 10, 10};
   croprect_clamp(&out, W, H);
   assert_rect(&out, 0, 0, CROPRECT_MIN_SIZE, CROPRECT_MIN_SIZE);
   out = (CropRect){-50, -50, 10, 10};
   croprect_move(&out, -1, -1, W, H); /* a move slides it in at its size */
   assert_rect(&out, 0, 0, 10, 10);
   out = (CropRect){W + 10, H + 10, 10, 10};
   croprect_clamp(&out, W, H);
   assert_rect(&out, W - CROPRECT_MIN_SIZE, H - CROPRECT_MIN_SIZE,
               CROPRECT_MIN_SIZE, CROPRECT_MIN_SIZE);
}

/* The aspect lock holds at the minimum size: a 2:1 lock on the 8x8 minimum
 * grows the width to 16 rather than clamping the height back up to 8 and
 * ending at 8x8. Same for a resize that hits the minimum. */
static void
test_aspect_lock_holds_at_minimum_size(void) {
   CropRect r = {100, 100, CROPRECT_MIN_SIZE, CROPRECT_MIN_SIZE};
   croprect_set_aspect(&r, 2.0, W, H);
   assert_rect(&r, 96, 100, 2 * CROPRECT_MIN_SIZE, CROPRECT_MIN_SIZE);
   r = (CropRect){100, 100, CROPRECT_MIN_SIZE, CROPRECT_MIN_SIZE};
   croprect_set_aspect(&r, 0.5, W, H); /* taller: the height grows */
   assert_rect(&r, 100, 96, CROPRECT_MIN_SIZE, 2 * CROPRECT_MIN_SIZE);
   /* A right-edge drag to nothing with a 2:1 lock: 16x8, not 8x8. */
   r = (CropRect){100, 100, 100, 50};
   croprect_resize(&r, CROPRECT_HIT_RIGHT, -500, 0, 2.0, W, H);
   g_assert_cmpfloat(r.d_w, ==, 2 * CROPRECT_MIN_SIZE);
   g_assert_cmpfloat(r.d_h, ==, CROPRECT_MIN_SIZE);
   /* And a bottom-edge drag with a 1:2 lock: 8x16. */
   r = (CropRect){100, 100, 100, 50};
   croprect_resize(&r, CROPRECT_HIT_BOTTOM, 0, -500, 0.5, W, H);
   g_assert_cmpfloat(r.d_w, ==, CROPRECT_MIN_SIZE);
   g_assert_cmpfloat(r.d_h, ==, 2 * CROPRECT_MIN_SIZE);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
   g_test_add_func("/croprect/init_full_and_is_full",
                   test_init_full_and_is_full);
   g_test_add_func("/croprect/clamp_keeps_inside_and_min_size",
                   test_clamp_keeps_inside_and_min_size);
   g_test_add_func("/croprect/intersect_is_a_plain_cut",
                   test_intersect_is_a_plain_cut);
   g_test_add_func("/croprect/move_slides_along_the_edge",
                   test_move_slides_along_the_edge);
   g_test_add_func("/croprect/resize_edges_anchor_opposite_edge",
                   test_resize_edges_anchor_opposite_edge);
   g_test_add_func("/croprect/resize_never_inverts_or_leaves",
                   test_resize_never_inverts_or_leaves);
   g_test_add_func("/croprect/resize_with_aspect_keeps_shape",
                   test_resize_with_aspect_keeps_shape);
   g_test_add_func("/croprect/set_aspect_refits_around_centre",
                   test_set_aspect_refits_around_centre);
   g_test_add_func("/croprect/hit_prefers_corners_then_edges",
                   test_hit_prefers_corners_then_edges);
   g_test_add_func("/croprect/drag_is_computed_from_start",
                   test_drag_is_computed_from_start);
   g_test_add_func("/croprect/rotate_quarter_keeps_the_same_pixels",
                   test_rotate_quarter_keeps_the_same_pixels);
   g_test_add_func("/croprect/round_snaps_edges", test_round_snaps_edges);
   g_test_add_func("/croprect/overshoot_keeps_the_opposite_edge",
                   test_overshoot_keeps_the_opposite_edge);
   g_test_add_func("/croprect/nan_never_propagates", test_nan_never_propagates);
   g_test_add_func("/croprect/zero_image_and_outside_rect",
                   test_zero_image_and_outside_rect);
   g_test_add_func("/croprect/aspect_lock_holds_at_minimum_size",
                   test_aspect_lock_holds_at_minimum_size);
   return (g_test_run());
}
