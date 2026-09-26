/*:*
 * ggaze — composed transform unit test (no display)
 *
 * Pins transform.h: the identity, quarter turns wrapping and carrying the
 * crop with them, the 0.5-degree snap and +-45 clamp, the horizon-levelling
 * sign convention (clockwise-positive, direction of the drag irrelevant),
 * the auto-crop / bounding-box sizes, the base/output size the enhancer
 * and the tools share (with the straighten's sampler inset and its 3 px
 * minimum), a crop that follows its base without erosion and is kept even
 * when pushed outside it (cropping nothing, said in the title), the title
 * description, and a crop through a quarter turn plus a straighten and
 * back. Negative rules: NaN angles and coordinates never propagate, a crop
 * entirely outside its base crops nothing, a quarter turn on an empty base
 * drops the crop rather than turning it negative.
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
   g_assert_null(transform_describe(&t, 400, 300));
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
   char *c     = transform_describe(&t, 400, 300);
   g_assert_cmpstr(c, ==, "90° CW");
   g_free(c);
   t.i_quarter = 2;
   c           = transform_describe(&t, 400, 300);
   g_assert_cmpstr(c, ==, "180°");
   g_free(c);
   t.i_quarter = 3;
   t.d_degrees = -1.5;
   t.b_crop    = TRUE;
   t.t_crop    = (CropRect){10, 10, 50, 50};
   c           = transform_describe(&t, 400, 300);
   g_assert_cmpstr(c, ==, "90° CCW, straighten 1.5° CCW, crop");
   g_free(c);
   transform_init(&t);
   t.d_degrees = 2.0;
   c           = transform_describe(&t, 400, 300);
   g_assert_cmpstr(c, ==, "straighten 2.0° CW");
   g_free(c);
   /* A crop of which nothing is inside the base is named as such; with the
    * original's size unknown it is taken at its word. */
   t.b_crop = TRUE;
   t.t_crop = (CropRect){1000, 1000, 10, 10};
   g_assert_true(transform_crop_is_outside(&t, 400, 300));
   g_assert_false(transform_crop_is_outside(&t, 0, 0));
   c = transform_describe(&t, 400, 300);
   g_assert_cmpstr(c, ==, "straighten 2.0° CW, crop (outside view)");
   g_free(c);
   c = transform_describe(&t, 0, 0);
   g_assert_cmpstr(c, ==, "straighten 2.0° CW, crop");
   g_free(c);
   t.t_crop = (CropRect){10, 10, 10, 10};
   g_assert_false(transform_crop_is_outside(&t, 400, 300));
   t.b_crop = FALSE;
   g_assert_false(transform_crop_is_outside(&t, 400, 300));
}

/* NaN never reaches the state or the GEGL graph: a NaN angle clamps to 0,
 * a NaN nudge leaves the angle alone, and a horizon with a NaN end levels
 * by nothing. */
static void
test_nan_angles_and_coordinates_are_rejected(void) {
   g_assert_cmpfloat(transform_clamp_angle(NAN), ==, 0.0);
   g_assert_cmpfloat(transform_clamp_angle(INFINITY), ==, 0.0);
   g_assert_cmpfloat(transform_clamp_angle(-INFINITY), ==, 0.0);
   Transform t;
   transform_init(&t);
   t.d_degrees = 3.0;
   transform_nudge_angle(&t, NAN);
   g_assert_cmpfloat(t.d_degrees, ==, 3.0);
   g_assert_cmpfloat(transform_horizon_degrees(0, 0, NAN, 10), ==, 0.0);
   g_assert_cmpfloat(transform_horizon_degrees(NAN, NAN, NAN, NAN), ==, 0.0);
   g_assert_cmpfloat(transform_horizon_degrees(0, 0, INFINITY, 10), ==, 0.0);
}

/* A crop entirely outside its base is no crop (FALSE, nothing to apply),
 * and so is one on a base that has no size. */
static void
test_effective_crop_outside_is_false(void) {
   Transform t;
   transform_init(&t);
   t.b_crop = TRUE;
   t.t_crop = (CropRect){500, 500, 10, 10};
   CropRect c;
   g_assert_false(transform_effective_crop(&t, 400, 300, &c));
   t.t_crop = (CropRect){-20, -20, 10, 10}; /* off the top-left */
   g_assert_false(transform_effective_crop(&t, 400, 300, &c));
   t.t_crop = (CropRect){399.9, 0, 10, 10}; /* a sliver: under half a px */
   g_assert_false(transform_effective_crop(&t, 400, 300, &c));
   t.t_crop = (CropRect){10, 10, 0, 0}; /* empty by construction */
   g_assert_false(transform_effective_crop(&t, 400, 300, &c));
   /* The output size then falls back to the base. */
   gdouble w, h;
   transform_output_size(&t, 400, 300, &w, &h);
   g_assert_cmpfloat(w, ==, 400);
   g_assert_cmpfloat(h, ==, 300);
}

/* A quarter turn with a crop but no base to turn it on (0x0: nothing has
 * been decoded) drops the crop instead of producing negative coordinates;
 * the turn itself still counts. */
static void
test_rotate_quarter_on_empty_base_drops_the_crop(void) {
   Transform t;
   transform_init(&t);
   t.b_crop = TRUE;
   t.t_crop = (CropRect){10, 10, 20, 20};
   transform_rotate_quarter(&t, 1, 0, 0);
   g_assert_cmpint(t.i_quarter, ==, 1);
   g_assert_false(t.b_crop);
   g_assert_cmpfloat(t.t_crop.d_x, >=, 0.0);
   t.b_crop = TRUE;
   transform_rotate_quarter(&t, -1, 0, 300); /* one empty side is enough */
   g_assert_false(t.b_crop);
   /* A real base keeps it (the turned rectangle is inside the base). */
   t.b_crop = TRUE;
   t.t_crop = (CropRect){10, 10, 20, 20};
   transform_rotate_quarter(&t, 1, 400, 300);
   g_assert_true(t.b_crop);
   g_assert_cmpfloat(t.t_crop.d_x, >=, 0.0);
   g_assert_cmpfloat(t.t_crop.d_y, >=, 0.0);
}

/* The straighten stage's size is the auto-crop inset by
 * TRANSFORM_AUTOCROP_INSET on every side (the sampler's blend margin), and
 * transform_base_size reports exactly that, so the enhancer's crop and the
 * crop tool's layout cannot disagree. 0 degrees and auto-crop off are
 * untouched. */
static void
test_straighten_size_insets_the_autocrop(void) {
   gdouble w, h, aw, ah;
   transform_autocrop_size(400, 300, 5.0, &aw, &ah);
   transform_straighten_size(400, 300, 5.0, TRUE, &w, &h);
   g_assert_cmpfloat(w, ==, aw - 2 * TRANSFORM_AUTOCROP_INSET);
   g_assert_cmpfloat(h, ==, ah - 2 * TRANSFORM_AUTOCROP_INSET);
   Transform t;
   transform_init(&t);
   t.d_degrees = 5.0;
   gdouble bw, bh;
   transform_base_size(&t, 400, 300, &bw, &bh);
   g_assert_cmpfloat(bw, ==, w);
   g_assert_cmpfloat(bh, ==, h);
   transform_straighten_size(400, 300, 0.0, TRUE, &w, &h);
   g_assert_cmpfloat(w, ==, 400);
   g_assert_cmpfloat(h, ==, 300);
   transform_straighten_size(400, 300, 5.0, FALSE, &w, &h);
   transform_rotated_size(400, 300, 5.0, &aw, &ah);
   g_assert_cmpfloat(w, ==, aw);
   g_assert_cmpfloat(h, ==, ah);
   transform_straighten_size(3, 2, 45.0, TRUE, &w, &h); /* never below 1 */
   g_assert_cmpfloat(w, >=, 1.0);
   g_assert_cmpfloat(h, >=, 1.0);
}

/* A crop follows its base when a straighten changes it: anchored on the
 * centre (a centred crop stays centred), and the STORED rectangle keeps
 * its size -- what the chain applies is the EFFECTIVE crop, the part
 * inside the new base. No crop, or the same base, changes nothing. */
static void
test_rebase_crop_follows_the_centre(void) {
   Transform t_old, t;
   transform_init(&t_old);
   t_old.b_crop = TRUE;
   t_old.t_crop = (CropRect){150, 100, 100, 100}; /* centred on (200,150) */
   t            = t_old;
   t.d_degrees  = 10.0;
   g_assert_true(transform_rebase_crop(&t, &t_old, 400, 300));
   gdouble bw, bh;
   transform_base_size(&t, 400, 300, &bw, &bh);
   g_assert_cmpfloat(t.t_crop.d_x + t.t_crop.d_w / 2, ==, bw / 2);
   g_assert_cmpfloat(t.t_crop.d_y + t.t_crop.d_h / 2, ==, bh / 2);
   g_assert_cmpfloat(t.t_crop.d_w, ==, 100);
   /* The right third: partly survives a 30-degree base. */
   t_old.t_crop = (CropRect){267, 0, 133, 300};
   t            = t_old;
   t.d_degrees  = 30.0;
   g_assert_true(transform_rebase_crop(&t, &t_old, 400, 300));
   transform_base_size(&t, 400, 300, &bw, &bh);
   g_assert_true(t.b_crop);
   g_assert_cmpfloat(t.t_crop.d_w, ==, 133);
   g_assert_cmpfloat(t.t_crop.d_h, ==, 300);
   CropRect eff;
   g_assert_true(transform_effective_crop(&t, bw, bh, &eff));
   g_assert_cmpfloat(eff.d_x + eff.d_w, ==, bw);
   g_assert_cmpfloat(eff.d_h, ==, bh);
   g_assert_cmpfloat(eff.d_w, >, 0);
   g_assert_cmpfloat(eff.d_w, <, 133);
   /* No crop: nothing to do, TRUE. Same base: unchanged. */
   transform_init(&t_old);
   t = t_old;
   g_assert_true(transform_rebase_crop(&t, &t_old, 400, 300));
   t_old.b_crop = TRUE;
   t_old.t_crop = (CropRect){10, 20, 30, 40};
   t            = t_old;
   g_assert_true(transform_rebase_crop(&t, &t_old, 400, 300));
   g_assert_true(croprect_equal(&t.t_crop, &t_old.t_crop));
}

/* A crop of which nothing is inside the new base is reported (FALSE) but
 * KEPT: b_crop stays set, the chain has no effective crop to apply, the
 * title says "crop (outside view)", and the angle coming back gives the
 * very rectangle back, applied again. An earlier round cleared b_crop
 * here, which lost the rectangle for good one nudge too far. */
static void
test_rebase_crop_outside_is_kept_and_comes_back(void) {
   Transform t_old, t;
   transform_init(&t_old);
   t_old.b_crop = TRUE;
   t_old.t_crop = (CropRect){392, 0, 8, 300}; /* a sliver at the far right */
   t            = t_old;
   t.d_degrees  = 10.0;
   g_assert_false(transform_rebase_crop(&t, &t_old, 400, 300));
   g_assert_true(t.b_crop);
   g_assert_true(transform_crop_is_outside(&t, 400, 300));
   gdouble  bw, bh;
   CropRect eff;
   transform_base_size(&t, 400, 300, &bw, &bh);
   g_assert_false(transform_effective_crop(&t, bw, bh, &eff));
   char *c_desc = transform_describe(&t, 400, 300);
   g_assert_cmpstr(c_desc, ==, "straighten 10.0° CW, crop (outside view)");
   g_free(c_desc);
   /* Back to 0 degrees: the very sliver again, applied again. */
   Transform t_back = t;
   t_back.d_degrees = 0.0;
   g_assert_true(transform_rebase_crop(&t_back, &t, 400, 300));
   g_assert_true(croprect_equal(&t_back.t_crop, &t_old.t_crop));
   g_assert_false(transform_crop_is_outside(&t_back, 400, 300));
   g_assert_true(transform_effective_crop(&t_back, 400, 300, &eff));
   g_assert_cmpfloat(eff.d_w, ==, 8);
}

/* A crop that touches the base's border is not eroded by a straighten and
 * back: the stored rectangle is only shifted (the cutting happens in
 * transform_effective_crop at render time), so 0 -> 5 -> 0 degrees gives
 * back exactly the rectangle that was committed. Before, the intersected
 * rectangle was stored, and {0,0,100,100} on 400x300 came back as
 * {12,17,88,83} -- shrunk for good on the first nudge. A centred crop
 * round-trips too. */
static void
test_rebase_crop_round_trips_a_border_crop(void) {
   Transform t_zero, t_five, t_back;
   transform_init(&t_zero);
   t_zero.b_crop    = TRUE;
   t_zero.t_crop    = (CropRect){0, 0, 100, 100};
   t_five           = t_zero;
   t_five.d_degrees = 5.0;
   g_assert_true(transform_rebase_crop(&t_five, &t_zero, 400, 300));
   /* At 5 degrees the base is smaller, so the shifted rectangle pokes out
    * of it; the effective crop is the part inside, the stored one whole. */
   g_assert_cmpfloat(t_five.t_crop.d_x, <, 0.0);
   g_assert_cmpfloat(t_five.t_crop.d_w, ==, 100);
   gdouble  bw, bh;
   CropRect eff;
   transform_base_size(&t_five, 400, 300, &bw, &bh);
   g_assert_true(transform_effective_crop(&t_five, bw, bh, &eff));
   g_assert_cmpfloat(eff.d_x, ==, 0);
   g_assert_cmpfloat(eff.d_w, <, 100);
   t_back           = t_five;
   t_back.d_degrees = 0.0;
   g_assert_true(transform_rebase_crop(&t_back, &t_five, 400, 300));
   g_assert_true(croprect_equal(&t_back.t_crop, &t_zero.t_crop));
   g_assert_true(transform_equal(&t_back, &t_zero));
   /* A centred crop: the same, and it never leaves the base. */
   t_zero.t_crop    = (CropRect){150, 100, 100, 100};
   t_five           = t_zero;
   t_five.d_degrees = 5.0;
   g_assert_true(transform_rebase_crop(&t_five, &t_zero, 400, 300));
   g_assert_true(transform_effective_crop(&t_five, bw, bh, &eff));
   g_assert_true(croprect_equal(&eff, &t_five.t_crop));
   t_back           = t_five;
   t_back.d_degrees = 0.0;
   g_assert_true(transform_rebase_crop(&t_back, &t_five, 400, 300));
   g_assert_true(croprect_equal(&t_back.t_crop, &t_zero.t_crop));
}

/* Crop + quarter turn + straighten compose: the turn carries the top-left
 * square exactly to the top-right of the 300x400 turned base
 * (transform_rotate_quarter), the 5-degree base shifts it about the
 * centre and cuts it at the turned edge, and the angle coming back gives
 * back the turned rectangle -- the whole transform equal to the turned
 * one. */
static void
test_rebase_crop_composes_with_a_quarter_turn(void) {
   Transform t_zero;
   transform_init(&t_zero);
   t_zero.b_crop      = TRUE;
   t_zero.t_crop      = (CropRect){0, 0, 100, 100};
   Transform t_turned = t_zero;
   transform_rotate_quarter(&t_turned, 1, 400, 300);
   g_assert_cmpint(t_turned.i_quarter, ==, 1);
   g_assert_cmpfloat(t_turned.t_crop.d_x, ==, 200);
   g_assert_cmpfloat(t_turned.t_crop.d_y, ==, 0);
   Transform t_five = t_turned;
   t_five.d_degrees = 5.0;
   g_assert_true(transform_rebase_crop(&t_five, &t_turned, 400, 300));
   gdouble  bw, bh;
   CropRect eff;
   transform_base_size(&t_five, 400, 300, &bw, &bh);
   g_assert_cmpfloat(bw, <, 300);
   g_assert_cmpfloat(t_five.t_crop.d_x + t_five.t_crop.d_w, >, bw);
   g_assert_true(transform_effective_crop(&t_five, bw, bh, &eff));
   g_assert_cmpfloat(eff.d_x + eff.d_w, ==, bw); /* cut at the turned edge */
   g_assert_cmpfloat(eff.d_w, <, 100);
   Transform t_back = t_five;
   t_back.d_degrees = 0.0;
   g_assert_true(transform_rebase_crop(&t_back, &t_five, 400, 300));
   g_assert_true(transform_equal(&t_back, &t_turned));
   g_assert_true(croprect_equal(&t_back.t_crop, &t_turned.t_crop));
   char *c_desc = transform_describe(&t_back, 400, 300);
   g_assert_cmpstr(c_desc, ==, "90° CW, crop");
   g_free(c_desc);
}

/* The opacity guarantee of the auto-crop inset needs an inscribed
 * rectangle of at least 2 * TRANSFORM_AUTOCROP_INSET + 1 = 3 px per side:
 * from there on the straighten size is exactly the inscribed rectangle
 * minus the inset; below it the 1x1 floor takes over and lies inside the
 * blend margin (documented in transform.h and the user docs rather than
 * refused -- straightening a 3x2 image is not meaningful anyway). */
static void
test_straighten_inset_needs_three_px_per_side(void) {
   gdouble w, h, aw, ah;
   gdouble d_min = 2.0 * TRANSFORM_AUTOCROP_INSET + 1.0;
   g_assert_cmpfloat(d_min, ==, 3.0);
   /* 5x5 at 45 degrees: the inscribed square is 3 px -> the inset holds. */
   transform_autocrop_size(5, 5, 45.0, &aw, &ah);
   g_assert_cmpfloat(aw, ==, 3.0);
   transform_straighten_size(5, 5, 45.0, TRUE, &w, &h);
   g_assert_cmpfloat(w, ==, aw - 2.0 * TRANSFORM_AUTOCROP_INSET);
   g_assert_cmpfloat(h, ==, ah - 2.0 * TRANSFORM_AUTOCROP_INSET);
   /* 3x2 at 45 degrees: the inscribed rectangle is 1x1, under the minimum,
    * so the floor -- not the inset -- decides and the guarantee is off. */
   transform_autocrop_size(3, 2, 45.0, &aw, &ah);
   g_assert_cmpfloat(aw, <, d_min);
   transform_straighten_size(3, 2, 45.0, TRUE, &w, &h);
   g_assert_cmpfloat(w, ==, 1.0);
   g_assert_cmpfloat(h, ==, 1.0);
   g_assert_cmpfloat(w, >, aw - 2.0 * TRANSFORM_AUTOCROP_INSET);
}

/* transform_scale (8l2): the preview renders a scaled-down source, so its
 * crop must cover the same part of the picture there. Without a crop the
 * transform is copied as it is; with one, the rectangle scales from the
 * original's base to the source's, per axis -- also after a quarter turn
 * swapped the sides -- and the scaled output is the original's output
 * scaled; a crop on an empty base is dropped. */
static void
test_scale_carries_the_crop_to_a_smaller_source(void) {
   Transform t_xf, t_out;
   transform_init(&t_xf);
   t_xf.d_degrees = 2.5;
   transform_scale(&t_xf, 4000, 3000, 1000, 750, &t_out);
   g_assert_true(transform_equal(&t_out, &t_xf)); /* nothing in pixels */

   transform_init(&t_xf);
   t_xf.b_crop = TRUE;
   t_xf.t_crop = (CropRect){400, 300, 2000, 1500};
   transform_scale(&t_xf, 4000, 3000, 1000, 750, &t_out);
   g_assert_true(t_out.b_crop);
   g_assert_cmpfloat_with_epsilon(t_out.t_crop.d_x, 100, 1e-9);
   g_assert_cmpfloat_with_epsilon(t_out.t_crop.d_y, 75, 1e-9);
   g_assert_cmpfloat_with_epsilon(t_out.t_crop.d_w, 500, 1e-9);
   g_assert_cmpfloat_with_epsilon(t_out.t_crop.d_h, 375, 1e-9);
   gdouble d_w, d_h, d_sw, d_sh;
   transform_output_size(&t_xf, 4000, 3000, &d_w, &d_h);
   transform_output_size(&t_out, 1000, 750, &d_sw, &d_sh);
   g_assert_cmpfloat(d_sw, ==, d_w / 4.0);
   g_assert_cmpfloat(d_sh, ==, d_h / 4.0);

   /* A quarter turn: the base is 3000 wide, the source's 750. */
   t_xf.i_quarter = 1;
   t_xf.t_crop    = (CropRect){0, 1000, 3000, 2000};
   transform_scale(&t_xf, 4000, 3000, 1000, 750, &t_out);
   g_assert_cmpfloat_with_epsilon(t_out.t_crop.d_y, 250, 1e-9);
   g_assert_cmpfloat_with_epsilon(t_out.t_crop.d_w, 750, 1e-9);
   g_assert_cmpfloat_with_epsilon(t_out.t_crop.d_h, 500, 1e-9);

   /* An empty original: nowhere to put the crop. */
   transform_scale(&t_xf, 0, 0, 10, 10, &t_out);
   g_assert_false(t_out.b_crop);
}

/* Straightened and cropped, the scaled output matches the original's to
 * the whole-pixel rounding of the smaller image (the auto-crop and the
 * crop snap to whole source pixels). */
static void
test_scale_straightened_crop_matches_to_a_pixel(void) {
   Transform t_xf, t_out;
   transform_init(&t_xf);
   t_xf.d_degrees = -4.0;
   t_xf.b_crop    = TRUE;
   t_xf.t_crop    = (CropRect){500, 400, 2400, 1600};
   transform_scale(&t_xf, 4000, 3000, 1333, 1000, &t_out);
   gdouble d_w, d_h, d_sw, d_sh;
   transform_output_size(&t_xf, 4000, 3000, &d_w, &d_h);
   transform_output_size(&t_out, 1333, 1000, &d_sw, &d_sh);
   g_assert_cmpfloat(fabs(d_sw - d_w * 1333.0 / 4000.0), <=, 1.5);
   g_assert_cmpfloat(fabs(d_sh - d_h * 1000.0 / 3000.0), <=, 1.5);
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
   g_test_add_func("/transform/nan_angles_and_coordinates_are_rejected",
                   test_nan_angles_and_coordinates_are_rejected);
   g_test_add_func("/transform/effective_crop_outside_is_false",
                   test_effective_crop_outside_is_false);
   g_test_add_func("/transform/rotate_quarter_on_empty_base_drops_the_crop",
                   test_rotate_quarter_on_empty_base_drops_the_crop);
   g_test_add_func("/transform/straighten_size_insets_the_autocrop",
                   test_straighten_size_insets_the_autocrop);
   g_test_add_func("/transform/rebase_crop_follows_the_centre",
                   test_rebase_crop_follows_the_centre);
   g_test_add_func("/transform/rebase_crop_outside_is_kept_and_comes_back",
                   test_rebase_crop_outside_is_kept_and_comes_back);
   g_test_add_func("/transform/rebase_crop_round_trips_a_border_crop",
                   test_rebase_crop_round_trips_a_border_crop);
   g_test_add_func("/transform/rebase_crop_composes_with_a_quarter_turn",
                   test_rebase_crop_composes_with_a_quarter_turn);
   g_test_add_func("/transform/straighten_inset_needs_three_px_per_side",
                   test_straighten_inset_needs_three_px_per_side);
   g_test_add_func("/transform/scale_carries_the_crop_to_a_smaller_source",
                   test_scale_carries_the_crop_to_a_smaller_source);
   g_test_add_func("/transform/scale_straightened_crop_matches_to_a_pixel",
                   test_scale_straightened_crop_matches_to_a_pixel);
   return (g_test_run());
}
