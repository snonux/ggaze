/*:*
 * ggaze — preview-scale unit tests (plain C, no display)
 *
 * The arithmetic of the scaled-down live enhance preview (8l2, decision
 * #53): the source scale a viewport needs (fit x device scale x
 * oversample, never up, within the minimum and maximum long side and a
 * caller's cap, a default viewport while none is known), the rounded
 * source size, when a source still serves a view, the card thumbnail
 * scale, and which GEGL properties are pixel lengths.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "preview-scale.h"

#include <glib.h>
#include <math.h>

/* A 64 MP camera photo in a 1000x740 view at 1x: fit is 740 / 6936, the
 * source 1.5 times that -- ~1480x1110, far below the image. At 2x the
 * source doubles; a view larger than the image gives the image itself
 * (never upscaled). */
static void
test_scale_follows_the_view(void) {
   PreviewView t_v = {1000, 740, 1, 0};
   gdouble     d_s = preview_scale_for_view(9248, 6936, &t_v);
   g_assert_cmpfloat_with_epsilon(d_s, 740.0 / 6936 * PREVIEW_SCALE_OVERSAMPLE,
                                  1e-12);
   gint i_w, i_h;
   preview_scale_size(9248, 6936, d_s, &i_w, &i_h);
   g_assert_cmpint(i_h, ==, 1110);
   g_assert_cmpint(i_w, ==, 1480);
   t_v.i_device_scale = 2;
   g_assert_cmpfloat_with_epsilon(preview_scale_for_view(9248, 6936, &t_v),
                                  2.0 * d_s, 1e-12);
   PreviewView t_big = {3000, 2000, 1, 0};
   g_assert_cmpfloat(preview_scale_for_view(400, 300, &t_big), ==, 1.0);
}

/* No view size yet: the default viewport stands in; a device scale <= 0
 * is 1. */
static void
test_unknown_view_uses_the_default(void) {
   PreviewView t_none = {0, -1, 0, 0};
   PreviewView t_def  = {PREVIEW_SCALE_DEFAULT_VIEW_W,
                         PREVIEW_SCALE_DEFAULT_VIEW_H, 1, 0};
   g_assert_cmpfloat(preview_scale_for_view(9248, 6936, &t_none), ==,
                     preview_scale_for_view(9248, 6936, &t_def));
   g_assert_cmpfloat(preview_scale_for_view(0, 10, &t_def), ==, 1.0);
}

/* The long side stays within PREVIEW_SCALE_MIN_SIDE .. MAX_SIDE, and a
 * caller's cap wins over both (the test seam's small source); the scale
 * never drops below one pixel's worth. */
static void
test_scale_is_bounded(void) {
   PreviewView t_tiny = {50, 40, 1, 0};
   gdouble     d_s    = preview_scale_for_view(8000, 6000, &t_tiny);
   g_assert_cmpfloat_with_epsilon(d_s * 8000, PREVIEW_SCALE_MIN_SIDE, 1e-9);
   PreviewView t_huge = {5120, 2880, 2, 0};
   d_s                = preview_scale_for_view(20000, 15000, &t_huge);
   g_assert_cmpfloat_with_epsilon(d_s * 20000, PREVIEW_SCALE_MAX_SIDE, 1e-9);
   PreviewView t_cap = {1000, 740, 1, 100};
   d_s               = preview_scale_for_view(400, 300, &t_cap);
   g_assert_cmpfloat_with_epsilon(d_s, 0.25, 1e-12);
   PreviewView t_one = {1000, 740, 1, 1};
   d_s               = preview_scale_for_view(9000, 3, &t_one);
   g_assert_cmpfloat_with_epsilon(d_s, 1.0 / 9000, 1e-12);
   gint i_w, i_h;
   preview_scale_size(9000, 3, d_s, &i_w, &i_h);
   g_assert_cmpint(i_w, ==, 1);
   g_assert_cmpint(i_h, ==, 1); /* never 0 */
}

/* A source serves a view that wants no finer, one that wants up to the
 * oversample more (it is still screen-sharp at fit: the head room), and
 * any view when it is the whole image; a view that would magnify it at
 * fit does not. */
static void
test_covers(void) {
   g_assert_true(preview_scale_covers(0.2, 0.2));
   g_assert_true(preview_scale_covers(0.2, 0.19));
   g_assert_true(preview_scale_covers(0.2, 0.25));
   g_assert_true(preview_scale_covers(0.2, 0.2 * PREVIEW_SCALE_OVERSAMPLE));
   g_assert_false(
      preview_scale_covers(0.2, 0.2 * PREVIEW_SCALE_OVERSAMPLE + 0.01));
   g_assert_true(preview_scale_covers(1.0, 3.0));
}

/* The card thumbnails: PREVIEW_SCALE_THUMB_SIDE on the long side, never
 * up. */
static void
test_thumb_scale(void) {
   g_assert_cmpfloat(preview_scale_thumb(1280, 960), ==,
                     (gdouble)PREVIEW_SCALE_THUMB_SIDE / 1280);
   g_assert_cmpfloat(preview_scale_thumb(300, 1000), ==,
                     (gdouble)PREVIEW_SCALE_THUMB_SIDE / 1000);
   g_assert_cmpfloat(preview_scale_thumb(100, 60), ==, 1.0);
}

/* The pixel lengths: Sharpen's std-dev and the blurs' radii are; its
 * strength, the noise reduction's iterations, vignette's relative radius
 * and an unknown op are not. A length scales with the image, never below
 * the property's minimum. */
static void
test_lengths(void) {
   g_assert_true(preview_scale_is_length("gegl:unsharp-mask", "std-dev"));
   g_assert_true(preview_scale_is_length("gegl:gaussian-blur", "std-dev-y"));
   g_assert_true(preview_scale_is_length("gegl:box-blur", "radius"));
   g_assert_true(preview_scale_is_length("gegl:dropshadow", "x"));
   g_assert_false(preview_scale_is_length("gegl:unsharp-mask", "scale"));
   g_assert_false(
      preview_scale_is_length("gegl:noise-reduction", "iterations"));
   g_assert_false(preview_scale_is_length("gegl:vignette", "radius"));
   g_assert_false(preview_scale_is_length("gegl:nope", "radius"));
   g_assert_false(preview_scale_is_length(NULL, "radius"));
   g_assert_false(preview_scale_is_length("gegl:box-blur", NULL));
   g_assert_cmpfloat_with_epsilon(preview_scale_length(3.0, 0.2, 0.0), 0.6,
                                  1e-12);
   g_assert_cmpfloat(preview_scale_length(4.0, 0.1, 1.0), ==, 1.0);
   g_assert_cmpfloat_with_epsilon(preview_scale_length(-20.0, 0.5, -1e300),
                                  -10.0, 1e-12);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
   g_test_add_func("/preview_scale/scale_follows_the_view",
                   test_scale_follows_the_view);
   g_test_add_func("/preview_scale/unknown_view_uses_the_default",
                   test_unknown_view_uses_the_default);
   g_test_add_func("/preview_scale/scale_is_bounded", test_scale_is_bounded);
   g_test_add_func("/preview_scale/covers", test_covers);
   g_test_add_func("/preview_scale/thumb_scale", test_thumb_scale);
   g_test_add_func("/preview_scale/lengths", test_lengths);
   return (g_test_run());
}
