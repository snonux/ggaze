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
 * the base (pre-crop) image size before the turn, d_base_w x d_base_h. */
void transform_rotate_quarter(Transform *p_t, gint i_dir, gdouble d_base_w,
                              gdouble d_base_h);

/* Snap d_deg to the nearest TRANSFORM_ANGLE_STEP and clamp it to
 * +-TRANSFORM_ANGLE_MAX. */
gdouble transform_clamp_angle(gdouble d_deg);

/* Straighten by d_delta more degrees clockwise (negative = counter-
 * clockwise), snapped and clamped. */
void transform_nudge_angle(Transform *p_t, gdouble d_delta);

/* The clockwise angle (snapped, clamped) that levels the line from (d_x0,
 * d_y0) to (d_x1, d_y1) in image coordinates (y down): a line sloping down
 * to the right needs a counter-clockwise turn, so this is negative for it.
 * The direction the line was dragged in does not matter (a line and its
 * reverse level the same way). A zero-length line gives 0. */
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
 * empty. */
gboolean transform_effective_crop(const Transform *p_t, gdouble d_base_w,
                                  gdouble d_base_h, CropRect *p_out);

/* A short human summary for the window title, e.g. "90° CW",
 * "180°", "straighten 1.5° CCW", "crop", joined by ", ";
 * NULL for the identity. Caller frees. */
char *transform_describe(const Transform *p_t);

G_END_DECLS

#endif /* GGAZE_TRANSFORM_H */
