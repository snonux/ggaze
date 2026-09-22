/*:*
 * ggaze — composed transform unit test (no display)
 *
 * Pins transform.h: the identity, quarter turns wrapping and carrying the
 * crop with them, the 0.5-degree snap and +-45 clamp, the horizon-levelling
 * sign convention (clockwise-positive, direction of the drag irrelevant),
 * the auto-crop / bounding-box sizes, the base/output size the enhancer
 * and the tools share, and the title description.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "transform.h"

#include <math.h>

#include <glib.h>

static void
test_identity_and_equality(void) {
   Transform t;
   transform_init(&t);
   g_assert_true(transform_is_identity(&t));
   g_assert_true(t.b_autocrop); /* decision #35: default on */
   g_assert_null(transform_describe(&t));
   Transform u = t;
   g_assert_true(transform_equal(&t, &u));
   u.b_autocrop = FALSE; /* no angle: auto-crop has no effect yet */
   g_assert_true(transform_equal(&t, &u));
   u.d_degrees = 1.0;
   g_assert_false(transform_equal(&t, &u));
   g_assert_false(transform_is_identity(&u));
   t.d_degrees = 1.0; /* same angle, different auto-crop: now it matters */
   g_assert_false(transform_equal(&t, &u));
   transform_init(&t);
   t.b_crop = TRUE;
   croprect_init_full(&t.t_crop, 10, 10);
   g_assert_false(transform_is_identity(&t));
   u            = t;
   u.t_crop.d_w = 5;
   g_assert_false(transform_equal(&t, &u));
}

static void
test_rotate_quarter_wraps_and_turns_the_crop(void) {
   Transform t;
   transform_init(&t);
   transform_rotate_quarter(&t, 1, 400, 300);
   g_assert_cmpint(t.i_quarter, ==, 1);
   transform_rotate_quarter(&t, -1, 300, 400);
   g_assert_cmpint(t.i_quarter, ==, 0);
   transform_rotate_quarter(&t, -1, 400, 300);
   g_assert_cmpint(t.i_quarter, ==, 3);
   transform_rotate_quarter(&t, 1, 300, 400);
   g_assert_cmpint(t.i_quarter, ==, 0);
   g_assert_true(transform_is_identity(&t));
   /* With a crop, the crop turns with the image (croprect_rotate_quarter). */
   t.b_crop = TRUE;
   t.t_crop = (CropRect){50, 200, 100, 50};
   transform_rotate_quarter(&t, 1, 400, 300);
   g_assert_cmpfloat(t.t_crop.d_x, ==, 300 - 250);
   g_assert_cmpfloat(t.t_crop.d_y, ==, 50);
   g_assert_cmpfloat(t.t_crop.d_w, ==, 50);
   g_assert_cmpfloat(t.t_crop.d_h, ==, 100);
}

static void
test_angle_snaps_and_clamps(void) {
   g_assert_cmpfloat(transform_clamp_angle(0.26), ==, 0.5);
   g_assert_cmpfloat(transform_clamp_angle(0.24), ==, 0.0);
   g_assert_cmpfloat(transform_clamp_angle(-0.24), ==, 0.0);
   g_assert_true(signbit(transform_clamp_angle(-0.24)) == 0);
   g_assert_cmpfloat(transform_clamp_angle(90.0), ==, TRANSFORM_ANGLE_MAX);
   g_assert_cmpfloat(transform_clamp_angle(-90.0), ==, -TRANSFORM_ANGLE_MAX);
   Transform t;
   transform_init(&t);
   transform_nudge_angle(&t, TRANSFORM_ANGLE_STEP);
   transform_nudge_angle(&t, TRANSFORM_ANGLE_STEP);
   g_assert_cmpfloat(t.d_degrees, ==, 1.0);
   for (int i = 0; i < 200; i++) {
      transform_nudge_angle(&t, -TRANSFORM_ANGLE_STEP);
   }
   g_assert_cmpfloat(t.d_degrees, ==, -TRANSFORM_ANGLE_MAX);
}

static void
test_horizon_degrees_levels_the_line(void) {
   /* Level already: nothing to do. */
   g_assert_cmpfloat(transform_horizon_degrees(0, 0, 100, 0), ==, 0.0);
   g_assert_cmpfloat(transform_horizon_degrees(0, 0, 0, 0), ==, 0.0);
   /* Sloping down to the right (y down): the image must turn counter-
    * clockwise, i.e. a NEGATIVE clockwise angle. tan(10) * 100 = 17.63. */
   gdouble d = transform_horizon_degrees(0, 0, 100, 17.63);
   g_assert_cmpfloat(d, ==, -10.0);
   /* Dragged the other way along the same line: the same answer. */
   g_assert_cmpfloat(transform_horizon_degrees(100, 17.63, 0, 0), ==, -10.0);
   /* Sloping up to the right: clockwise. */
   g_assert_cmpfloat(transform_horizon_degrees(0, 17.63, 100, 0), ==, 10.0);
   /* A near-vertical line folds into the (-90, 90] range and clamps. */
   g_assert_cmpfloat(transform_horizon_degrees(0, 0, 1, 100), ==,
                     -TRANSFORM_ANGLE_MAX);
}

static void
test_autocrop_and_rotated_sizes(void) {
   gdouble w, h;
   transform_autocrop_size(400, 300, 0.0, &w, &h);
   g_assert_cmpfloat(w, ==, 400);
   g_assert_cmpfloat(h, ==, 300);
   /* A square at 45 degrees: the inscribed square has side s / sqrt(2). */
   transform_autocrop_size(100, 100, 45.0, &w, &h);
   g_assert_cmpfloat(w, ==, floor(100 / G_SQRT2));
   g_assert_cmpfloat(h, ==, floor(100 / G_SQRT2));
   /* 90 degrees is a plain swap. */
   transform_autocrop_size(100, 50, 90.0, &w, &h);
   g_assert_cmpfloat(w, ==, 50);
   g_assert_cmpfloat(h, ==, 100);
   /* The sign of the angle does not matter and the result fits inside. */
   gdouble w2, h2;
   transform_autocrop_size(400, 300, 5.0, &w, &h);
   transform_autocrop_size(400, 300, -5.0, &w2, &h2);
   g_assert_cmpfloat(w, ==, w2);
   g_assert_cmpfloat(h, ==, h2);
   g_assert_cmpfloat(w, <, 400);
   g_assert_cmpfloat(h, <, 300);
   g_assert_cmpfloat(w, >, 350);
   /* The bounding box grows instead. */
   transform_rotated_size(400, 300, 5.0, &w, &h);
   g_assert_cmpfloat(w, >, 400);
   g_assert_cmpfloat(h, >, 300);
   transform_rotated_size(400, 300, 0.0, &w, &h);
   g_assert_cmpfloat(w, ==, 400);
   g_assert_cmpfloat(h, ==, 300);
   transform_autocrop_size(0, 300, 5.0, &w, &h);
   g_assert_cmpfloat(w, ==, 0);
}

static void
test_base_and_output_sizes(void) {
   Transform t;
   transform_init(&t);
   gdouble w, h;
   transform_base_size(&t, 400, 300, &w, &h);
   g_assert_cmpfloat(w, ==, 400);
   g_assert_cmpfloat(h, ==, 300);
   t.i_quarter = 1;
   transform_base_size(&t, 400, 300, &w, &h);
   g_assert_cmpfloat(w, ==, 300);
   g_assert_cmpfloat(h, ==, 400);
   t.d_degrees = 10.0; /* auto-crop: smaller than the turned image */
   transform_base_size(&t, 400, 300, &w, &h);
   g_assert_cmpfloat(w, <, 300);
   g_assert_cmpfloat(h, <, 400);
   t.b_autocrop = FALSE; /* bounding box: larger */
   transform_base_size(&t, 400, 300, &w, &h);
   g_assert_cmpfloat(w, >, 300);
   g_assert_cmpfloat(h, >, 400);
   /* The crop is clamped into the base and rounded. */
   transform_init(&t);
   t.b_crop = TRUE;
   t.t_crop = (CropRect){10.4, 20.6, 1000, 50.2};
   CropRect c;
   g_assert_true(transform_effective_crop(&t, 400, 300, &c));
   g_assert_cmpfloat(c.d_x, ==, 10);
   g_assert_cmpfloat(c.d_y, ==, 21);
   g_assert_cmpfloat(c.d_w, ==, 390);
   g_assert_cmpfloat(c.d_h, ==, 50);
   transform_output_size(&t, 400, 300, &w, &h);
   g_assert_cmpfloat(w, ==, 390);
   g_assert_cmpfloat(h, ==, 50);
   t.b_crop = FALSE;
   g_assert_false(transform_effective_crop(&t, 400, 300, &c));
   t.b_crop = TRUE;
   g_assert_false(transform_effective_crop(&t, 0, 0, &c)); /* no base */
}

static void
test_describe(void) {
   Transform t;
   transform_init(&t);
   t.i_quarter = 1;
   char *c     = transform_describe(&t);
   g_assert_cmpstr(c, ==, "90° CW");
   g_free(c);
   t.i_quarter = 2;
   c           = transform_describe(&t);
   g_assert_cmpstr(c, ==, "180°");
   g_free(c);
   t.i_quarter = 3;
   t.d_degrees = -1.5;
   t.b_crop    = TRUE;
   c           = transform_describe(&t);
   g_assert_cmpstr(c, ==, "90° CCW, straighten 1.5° CCW, crop");
   g_free(c);
   transform_init(&t);
   t.d_degrees = 2.0;
   c           = transform_describe(&t);
   g_assert_cmpstr(c, ==, "straighten 2.0° CW");
   g_free(c);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
   g_test_add_func("/transform/identity_and_equality",
                   test_identity_and_equality);
   g_test_add_func("/transform/rotate_quarter_wraps_and_turns_the_crop",
                   test_rotate_quarter_wraps_and_turns_the_crop);
   g_test_add_func("/transform/angle_snaps_and_clamps",
                   test_angle_snaps_and_clamps);
   g_test_add_func("/transform/horizon_degrees_levels_the_line",
                   test_horizon_degrees_levels_the_line);
   g_test_add_func("/transform/autocrop_and_rotated_sizes",
                   test_autocrop_and_rotated_sizes);
   g_test_add_func("/transform/base_and_output_sizes",
                   test_base_and_output_sizes);
   g_test_add_func("/transform/describe", test_describe);
   return (g_test_run());
}
