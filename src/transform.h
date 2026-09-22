#ifndef GGAZE_TRANSFORM_H
#define GGAZE_TRANSFORM_H

/*:*
 * ggaze — the composed geometric transform (plain C, no GTK, no GEGL)
 *
 * The non-destructive rotate-90 / straighten / crop state the `[` `]`, `R`
 * and `c` tools edit and the enhancer turns into GEGL ops, in the compose
 * order of decision #35: load -> enhance (colour presets) -> rotate 90 ->
 * straighten (+ auto-crop of the rotated corners, default on) -> crop ->
 * export. Angles are in the user's terms -- DEGREES CLOCKWISE on screen --
 * and the enhancer negates them for GEGL, whose positive rotation is
 * counter-clockwise in image (y-down) coordinates (measured, see enhancer.c
 * _append_transform).
 *
 * Besides the state, this module holds the pure geometry every consumer
 * needs to agree on: the size an image has after each stage
 * (transform_output_size), so the tool that edits the crop rectangle, the
 * controller that turns it with the image, and the GEGL chain that finally
 * crops all use the same numbers; the largest axis-aligned rectangle inside
 * a rotated image (auto-crop); and the angle that levels a horizon line the
 * user dragged. Unit-tested without a display (tests/test_transform.c).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <glib.h>

#include "croprect.h"

G_BEGIN_DECLS

/* The straighten tool's nudge step (h/l/+/-) and its range. Half a degree is
 * fine enough to level any horizon; beyond 45 degrees a quarter turn plus a
 * small angle is the right tool. */
#define TRANSFORM_ANGLE_STEP 0.5
#define TRANSFORM_ANGLE_MAX 45.0

/* Pixels trimmed from EACH side of the straighten's auto-crop. The largest
 * inscribed rectangle touches the rotated image's edges at its corners, and
 * gegl:rotate's default (linear) sampler blends the outermost pixel with the
 * transparent abyss, so a crop of exactly that size keeps partially
 * transparent corner pixels (measured on gegl 0.4: alpha down to 145 at the
 * corners, up to 32 such pixels on 400x300; a JPEG export then differs from
 * the preview there). One pixel per side is the smallest inset that left
 * every kept pixel fully opaque at 0.5..45 degrees on 33x47 .. 4000x3000
 * (tests/test_enhancer.c pins it).
 *
 * The guarantee needs room for the inset: the inscribed rectangle must be
 * at least 2 * inset + 1 = 3 px on each side. Below that
 * transform_straighten_size floors the output at 1x1 INSIDE the blend
 * margin, so straightening an image that small (a 3x2 at 45 degrees) may
 * keep translucent pixels -- and is not a meaningful operation anyway;
 * tests/test_transform.c pins the boundary rather than refusing it. */
#define TRANSFORM_AUTOCROP_INSET 1.0

typedef struct {
   gint i_quarter;      /* clockwise quarter turns, 0..3 (] adds, [ subtracts)
                         */
   gdouble  d_degrees;  /* straighten angle, clockwise, -45..45, 0.5 steps */
   gboolean b_autocrop; /* straighten: crop the rotated corners away (default
                         * on, decision #35); off keeps the whole rotated
                         * image with transparent/black corners */
   gboolean b_crop;     /* t_crop is applied */
   CropRect t_crop;     /* the crop, in pixels of the image as it is AFTER
                         * rotate + straighten (the "base" image the crop
                         * tool shows) */
} Transform;

/* The identity: no turn, no angle, no crop, auto-crop on. */
void transform_init(Transform *p_t);

/* TRUE iff applying p_t changes nothing (a full-image crop counts as none
 * only via the enhancer's clamp; here b_crop is taken at its word). */
gboolean transform_is_identity(const Transform *p_t);

/* TRUE iff both describe the same transform. */
gboolean transform_equal(const Transform *p_a, const Transform *p_b);

/* `]` (i_dir > 0) / `[` (i_dir < 0): one more quarter turn, wrapping so four
 * of them are the identity again. An active crop is turned with the image
 * (croprect_rotate_quarter) so it keeps covering the same pixels; that needs
 * the base (pre-crop) image size before the turn, d_base_w x d_base_h. A
 * crop cannot be located on an empty base (a dimension <= 0), so it is
 * dropped (b_crop cleared) rather than turned into negative coordinates. */
void transform_rotate_quarter(Transform *p_t, gint i_dir, gdouble d_base_w,
                              gdouble d_base_h);

/* Snap d_deg to the nearest TRANSFORM_ANGLE_STEP and clamp it to
 * +-TRANSFORM_ANGLE_MAX. A non-finite angle is 0 (NaN never propagates into
 * the state or the GEGL graph). */
gdouble transform_clamp_angle(gdouble d_deg);

/* Straighten by d_delta more degrees clockwise (negative = counter-
 * clockwise), snapped and clamped. A non-finite delta changes nothing. */
void transform_nudge_angle(Transform *p_t, gdouble d_delta);

/* The clockwise angle (snapped, clamped) that levels the line from (d_x0,
 * d_y0) to (d_x1, d_y1) in image coordinates (y down): a line sloping down
 * to the right needs a counter-clockwise turn, so this is negative for it.
 * The direction the line was dragged in does not matter (a line and its
 * reverse level the same way). A zero-length line, or one with a non-finite
 * coordinate, gives 0. */
gdouble transform_horizon_degrees(gdouble d_x0, gdouble d_y0, gdouble d_x1,
                                  gdouble d_y1);

/* The largest axis-aligned rectangle that fits inside a d_w x d_h image
 * rotated by d_deg degrees (either direction), i.e. the auto-crop size. At
 * 0 degrees it is the image itself. Results are whole pixels (floored). */
void transform_autocrop_size(gdouble d_w, gdouble d_h, gdouble d_deg,
                             gdouble *p_w, gdouble *p_h);

/* The bounding box of a d_w x d_h image rotated by d_deg degrees, whole
 * pixels (ceiled): the size a straighten WITHOUT auto-crop produces. */
void transform_rotated_size(gdouble d_w, gdouble d_h, gdouble d_deg,
                            gdouble *p_w, gdouble *p_h);

/* The size the straighten stage outputs for a d_w x d_h input at d_deg: the
 * auto-crop rectangle inset by TRANSFORM_AUTOCROP_INSET on every side (never
 * below 1x1) when b_autocrop, else the rotated bounding box. This -- not
 * transform_autocrop_size -- is what the enhancer crops to and what
 * transform_base_size reports, so the two cannot drift apart. 0 degrees is
 * the input itself. */
void transform_straighten_size(gdouble d_w, gdouble d_h, gdouble d_deg,
                               gboolean b_autocrop, gdouble *p_w, gdouble *p_h);

/* The size of a d_w x d_h original after p_t's turn and straighten, i.e.
 * the base image the crop rectangle refers to (b_crop is ignored). This is
 * THE definition of that size: the enhancer builds its GEGL chain to produce
 * exactly it, so the crop tool can lay out a rectangle before the preview
 * has even rendered. */
void transform_base_size(const Transform *p_t, gdouble d_w, gdouble d_h,
                         gdouble *p_w, gdouble *p_h);

/* The size after the whole transform (base size, then the crop clamped
 * into it). */
void transform_output_size(const Transform *p_t, gdouble d_w, gdouble d_h,
                           gdouble *p_w, gdouble *p_h);

/* The crop as the enhancer applies it: p_t->t_crop clamped into the base
 * size and snapped to whole pixels. FALSE when there is no crop or it is
 * empty (entirely outside the base). */
gboolean transform_effective_crop(const Transform *p_t, gdouble d_base_w,
                                  gdouble d_base_h, CropRect *p_out);

/* Keep p_t's crop on the same image content after its base changed size
 * under it: the base of a d_orig_w x d_orig_h original was transform_base_size
 * of *p_old and is now that of *p_t (a straighten angle or auto-crop change;
 * a quarter turn is handled exactly by transform_rotate_quarter instead). The
 * straighten turns and the auto-crop shrinks about the centre, so the crop
 * is anchored on the centre -- the same offset from it as before. The
 * rectangle is kept WHOLE (shifted, never cut down): what the chain crops is
 * transform_effective_crop, its intersection with the base, judged at render
 * and export time, so a crop reaching past a shrunken base grows back when
 * the angle comes back and the same angles give back the same rectangle
 * exactly. Returns FALSE, with b_crop cleared, when the effective crop on the
 * new base is empty (nothing of it is left); TRUE (also with no crop at all)
 * otherwise. The title only ever says "crop" for a crop that is really
 * applied. */
gboolean transform_rebase_crop(Transform *p_t, const Transform *p_old,
                               gdouble d_orig_w, gdouble d_orig_h);

/* A short human summary for the window title, e.g. "90° CW",
 * "180°", "straighten 1.5° CCW", "crop", joined by ", ";
 * NULL for the identity. Caller frees. */
char *transform_describe(const Transform *p_t);

G_END_DECLS

#endif /* GGAZE_TRANSFORM_H */
