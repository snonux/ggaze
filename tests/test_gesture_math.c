/*:*
 * ggaze — zoom / touch-gesture math unit test (no display)
 *
 * Pins the rules gesture-math.h promises (task zb2): zooming about a point
 * keeps that point's image pixel under it, the zoom clamp rises to the fit
 * ratio (jx0), a pinch multiplies its start zoom, a swipe needs distance,
 * speed and a mostly horizontal path, and a two-finger tap is short and
 * still. Negative rules: NaN / Inf anywhere (hx0) changes nothing, a zero
 * or negative pinch scale is refused, a zero-duration swipe (no velocity),
 * a vertical swipe, a tiny swipe and one that reverses at the end are not
 * swipes, and a long, moving or scaling two-touch gesture is not a tap.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "gesture-math.h"

#include <math.h>

#include <glib.h>

/* A 1200x800 image drawn at fit (0.5) in a 600x400 widget: top-left 0,0. */
static GestureView
fit_view(void) {
   GestureView t_v = {.i_w     = 600,
                      .i_h     = 400,
                      .i_tex_w = 1200,
                      .i_tex_h = 800,
                      .d_scale = 0.5,
                      .d_x     = 0.0,
                      .d_y     = 0.0,
                      .d_fit   = 0.5};
   return (t_v);
}

/* The image pixel under widget point (d_cx, d_cy) for a view drawn at
 * d_zoom with pan (d_px, d_py) -- the inverse the viewer's draw implies. */
static void
image_point(const GestureView *p_v, gdouble d_zoom, gdouble d_px, gdouble d_py,
            gdouble d_cx, gdouble d_cy, gdouble *p_ix, gdouble *p_iy) {
   gdouble d_x = (p_v->i_w - p_v->i_tex_w * d_zoom) / 2.0 + d_px;
   gdouble d_y = (p_v->i_h - p_v->i_tex_h * d_zoom) / 2.0 + d_py;
   *p_ix       = (d_cx - d_x) / d_zoom;
   *p_iy       = (d_cy - d_y) / d_zoom;
}

static void
test_zoom_about_keeps_the_point(void) {
   GestureView t_v = fit_view();
   gdouble     d_z, d_px, d_py;
   g_assert_true(
      gesture_math_zoom_about(&t_v, 150.0, 100.0, 1.0, &d_z, &d_px, &d_py));
   g_assert_cmpfloat(d_z, ==, 1.0);
   gdouble d_ix, d_iy;
   image_point(&t_v, d_z, d_px, d_py, 150.0, 100.0, &d_ix, &d_iy);
   /* Widget (150, 100) was image (300, 200) at 0.5 and still is. */
   g_assert_cmpfloat(fabs(d_ix - 300.0), <, 1e-9);
   g_assert_cmpfloat(fabs(d_iy - 200.0), <, 1e-9);
   /* About the centre: no pan at all. */
   g_assert_true(
      gesture_math_zoom_about(&t_v, 300.0, 200.0, 2.0, &d_z, &d_px, &d_py));
   g_assert_cmpfloat(fabs(d_px), <, 1e-9);
   g_assert_cmpfloat(fabs(d_py), <, 1e-9);
}

static void
test_zoom_about_clamps(void) {
   GestureView t_v = fit_view();
   gdouble     d_z, d_px, d_py;
   g_assert_true(
      gesture_math_zoom_about(&t_v, 0.0, 0.0, 1000.0, &d_z, &d_px, &d_py));
   g_assert_cmpfloat(d_z, ==, GGAZE_ZOOM_MAX);
   g_assert_true(
      gesture_math_zoom_about(&t_v, 0.0, 0.0, 1e-9, &d_z, &d_px, &d_py));
   g_assert_cmpfloat(d_z, ==, GGAZE_ZOOM_MIN);
   /* jx0: the ceiling rises to a fit ratio above it, never below it. */
   g_assert_cmpfloat(gesture_math_clamp_zoom(150.0, 100.0), ==, 100.0);
   g_assert_cmpfloat(gesture_math_clamp_zoom(150.0, 1.0), ==, GGAZE_ZOOM_MAX);
   g_assert_cmpfloat(gesture_math_clamp_zoom(150.0, INFINITY), ==,
                     GGAZE_ZOOM_MAX);
   g_assert_cmpfloat(gesture_math_clamp_zoom(150.0, NAN), ==, GGAZE_ZOOM_MAX);
}

/* A view not drawn yet (scale 0) zooms about the image's top-left instead
 * of dividing by zero. */
static void
test_zoom_about_undrawn_view(void) {
   GestureView t_v = fit_view();
   t_v.d_scale     = 0.0;
   gdouble d_z, d_px, d_py;
   g_assert_true(
      gesture_math_zoom_about(&t_v, 10.0, 20.0, 1.0, &d_z, &d_px, &d_py));
   g_assert_true(isfinite(d_px) && isfinite(d_py));
}

/* hx0: any non-finite input is refused and leaves the outputs alone. */
static void
test_zoom_about_rejects_non_finite(void) {
   const gdouble c_bad[] = {NAN, INFINITY, -INFINITY};
   for (guint u = 0; u < G_N_ELEMENTS(c_bad); u++) {
      GestureView t_v  = fit_view();
      gdouble     d_z  = -1.0;
      gdouble     d_px = -2.0;
      gdouble     d_py = -3.0;
      g_assert_false(
         gesture_math_zoom_about(&t_v, c_bad[u], 1.0, 1.0, &d_z, &d_px, &d_py));
      g_assert_false(
         gesture_math_zoom_about(&t_v, 1.0, c_bad[u], 1.0, &d_z, &d_px, &d_py));
      g_assert_false(
         gesture_math_zoom_about(&t_v, 1.0, 1.0, c_bad[u], &d_z, &d_px, &d_py));
      t_v.d_x = c_bad[u];
      g_assert_false(
         gesture_math_zoom_about(&t_v, 1.0, 1.0, 1.0, &d_z, &d_px, &d_py));
      t_v     = fit_view();
      t_v.d_y = c_bad[u];
      g_assert_false(
         gesture_math_zoom_about(&t_v, 1.0, 1.0, 1.0, &d_z, &d_px, &d_py));
      t_v         = fit_view();
      t_v.d_scale = c_bad[u];
      g_assert_false(
         gesture_math_zoom_about(&t_v, 1.0, 1.0, 1.0, &d_z, &d_px, &d_py));
      t_v       = fit_view();
      t_v.d_fit = c_bad[u];
      g_assert_false(
         gesture_math_zoom_about(&t_v, 1.0, 1.0, 1.0, &d_z, &d_px, &d_py));
      g_assert_cmpfloat(d_z, ==, -1.0);
      g_assert_cmpfloat(d_px, ==, -2.0);
      g_assert_cmpfloat(d_py, ==, -3.0);
   }
}

static void
test_pinch_zoom(void) {
   gdouble d_z = -1.0;
   g_assert_true(gesture_math_pinch_zoom(0.5, 2.0, &d_z));
   g_assert_cmpfloat(d_z, ==, 1.0);
   g_assert_true(gesture_math_pinch_zoom(0.5, 0.5, &d_z));
   g_assert_cmpfloat(d_z, ==, 0.25);
   /* Negative: zero / negative / non-finite scale or start, and an overflow
    * to infinity, are refused with p_zoom untouched. */
   d_z = -1.0;
   g_assert_false(gesture_math_pinch_zoom(0.5, 0.0, &d_z));
   g_assert_false(gesture_math_pinch_zoom(0.5, -1.0, &d_z));
   g_assert_false(gesture_math_pinch_zoom(0.5, NAN, &d_z));
   g_assert_false(gesture_math_pinch_zoom(0.5, INFINITY, &d_z));
   g_assert_false(gesture_math_pinch_zoom(0.0, 2.0, &d_z));
   g_assert_false(gesture_math_pinch_zoom(NAN, 2.0, &d_z));
   g_assert_cmpfloat(d_z, ==, -1.0);
   g_assert_false(gesture_math_pinch_zoom(G_MAXDOUBLE, 4.0, &d_z));
}

/* The fit detent's rebased scale: 1 inside the band, continuous at both
 * edges (no jump to 1.1x fit), proportional beyond; non-finite passes
 * through for pinch_zoom to refuse, zero / negative stay refusable. */
static void
test_detent_scale(void) {
   const gdouble d_b = GESTURE_TAP_MAX_SCALE_DEV;
   g_assert_cmpfloat(gesture_math_detent_scale(1.0), ==, 1.0);
   g_assert_cmpfloat(gesture_math_detent_scale(1.0 + d_b), ==, 1.0);
   g_assert_cmpfloat(gesture_math_detent_scale(1.0 - d_b), ==, 1.0);
   g_assert_cmpfloat(fabs(gesture_math_detent_scale(1.0 + d_b + 1e-9) - 1.0), <,
                     1e-8);
   g_assert_cmpfloat(fabs(gesture_math_detent_scale(1.0 - d_b - 1e-9) - 1.0), <,
                     1e-8);
   g_assert_cmpfloat(fabs(gesture_math_detent_scale(2.0 * (1.0 + d_b)) - 2.0),
                     <, 1e-12);
   g_assert_cmpfloat(fabs(gesture_math_detent_scale(0.5 * (1.0 - d_b)) - 0.5),
                     <, 1e-12);
   g_assert_true(isnan(gesture_math_detent_scale(NAN)));
   g_assert_true(isinf(gesture_math_detent_scale(INFINITY)));
   g_assert_cmpfloat(gesture_math_detent_scale(0.0), ==, 0.0);
   g_assert_cmpfloat(gesture_math_detent_scale(-1.0), <, 0.0);
}

static void
test_swipe_directions(void) {
   /* Leftward flick = next, rightward = previous. */
   g_assert_cmpint(gesture_math_swipe_direction(-200.0, 10.0, -900.0, 0.0), ==,
                   1);
   g_assert_cmpint(gesture_math_swipe_direction(200.0, -10.0, 900.0, 50.0), ==,
                   -1);
   /* Exactly at every threshold still counts. */
   g_assert_cmpint(gesture_math_swipe_direction(
                      -GESTURE_SWIPE_MIN_DISTANCE,
                      GESTURE_SWIPE_MAX_SLOPE * GESTURE_SWIPE_MIN_DISTANCE,
                      -GESTURE_SWIPE_MIN_VELOCITY, 0.0),
                   ==, 1);
}

static void
test_swipe_rejects(void) {
   /* Zero-duration: GTK reports no velocity for it. */
   g_assert_cmpint(gesture_math_swipe_direction(-200.0, 0.0, 0.0, 0.0), ==, 0);
   /* Vertical, and diagonal past the slope. */
   g_assert_cmpint(gesture_math_swipe_direction(0.0, 300.0, 0.0, 900.0), ==, 0);
   g_assert_cmpint(gesture_math_swipe_direction(-200.0, 150.0, -900.0, 0.0), ==,
                   0);
   /* Tiny, slow, and reversing at the end. */
   g_assert_cmpint(gesture_math_swipe_direction(-30.0, 0.0, -900.0, 0.0), ==,
                   0);
   g_assert_cmpint(gesture_math_swipe_direction(-200.0, 0.0, -100.0, 0.0), ==,
                   0);
   g_assert_cmpint(gesture_math_swipe_direction(-200.0, 0.0, 900.0, 0.0), ==,
                   0);
   /* Non-finite anywhere. */
   g_assert_cmpint(gesture_math_swipe_direction(NAN, 0.0, -900.0, 0.0), ==, 0);
   g_assert_cmpint(gesture_math_swipe_direction(-200.0, NAN, -900.0, 0.0), ==,
                   0);
   g_assert_cmpint(gesture_math_swipe_direction(-200.0, 0.0, -INFINITY, 0.0),
                   ==, 0);
   g_assert_cmpint(gesture_math_swipe_direction(-200.0, 0.0, -900.0, NAN), ==,
                   0);
}

static void
test_two_finger_tap(void) {
   g_assert_true(gesture_math_is_two_finger_tap(0, 0.0, 0.0));
   g_assert_true(gesture_math_is_two_finger_tap(
      GESTURE_TAP_MAX_US, GESTURE_TAP_MAX_MOVE, GESTURE_TAP_MAX_SCALE_DEV));
   /* Too long, moved, pinched, or nonsense. */
   g_assert_false(
      gesture_math_is_two_finger_tap(GESTURE_TAP_MAX_US + 1, 0.0, 0.0));
   g_assert_false(gesture_math_is_two_finger_tap(-1, 0.0, 0.0));
   g_assert_false(
      gesture_math_is_two_finger_tap(0, GESTURE_TAP_MAX_MOVE + 1.0, 0.0));
   g_assert_false(
      gesture_math_is_two_finger_tap(0, 0.0, GESTURE_TAP_MAX_SCALE_DEV + 0.01));
   g_assert_false(gesture_math_is_two_finger_tap(0, NAN, 0.0));
   g_assert_false(gesture_math_is_two_finger_tap(0, 0.0, INFINITY));
   g_assert_false(gesture_math_is_two_finger_tap(0, -1.0, 0.0));
   g_assert_false(gesture_math_is_two_finger_tap(0, 0.0, -1.0));
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/gesture_math/zoom_about_keeps_the_point",
                   test_zoom_about_keeps_the_point);
   g_test_add_func("/gesture_math/zoom_about_clamps", test_zoom_about_clamps);
   g_test_add_func("/gesture_math/zoom_about_undrawn_view",
                   test_zoom_about_undrawn_view);
   g_test_add_func("/gesture_math/zoom_about_rejects_non_finite",
                   test_zoom_about_rejects_non_finite);
   g_test_add_func("/gesture_math/pinch_zoom", test_pinch_zoom);
   g_test_add_func("/gesture_math/detent_scale", test_detent_scale);
   g_test_add_func("/gesture_math/swipe_directions", test_swipe_directions);
   g_test_add_func("/gesture_math/swipe_rejects", test_swipe_rejects);
   g_test_add_func("/gesture_math/two_finger_tap", test_two_finger_tap);
   return (g_test_run());
}
