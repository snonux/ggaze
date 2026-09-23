/*:*
 * ggaze — the composed geometric transform
 *
 * See transform.h. The state is a value type the controllers copy around;
 * the geometry helpers below are what keeps the crop tool, the controller
 * and the GEGL chain agreeing on image sizes.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "transform.h"

#include <math.h>
#include <string.h>

#include <glib.h>

#include "croprect.h"

/* --- state ---------------------------------------------------------------- */

void
transform_init(Transform *p_t) {
   g_return_if_fail(p_t != NULL);
   memset(p_t, 0, sizeof(*p_t));
   p_t->b_autocrop = TRUE; /* decision #35: auto-crop default on */
}

gboolean
transform_is_identity(const Transform *p_t) {
   g_return_val_if_fail(p_t != NULL, TRUE);
   return (p_t->i_quarter == 0 && p_t->d_degrees == 0.0 && !p_t->b_crop);
}

gboolean
transform_equal(const Transform *p_a, const Transform *p_b) {
   g_return_val_if_fail(p_a != NULL && p_b != NULL, FALSE);
   if (p_a->i_quarter != p_b->i_quarter ||
       fabs(p_a->d_degrees - p_b->d_degrees) > 1e-9 ||
       p_a->b_crop != p_b->b_crop) {
      return (FALSE);
   }
   /* Auto-crop only has an effect while there is an angle: comparing it at
    * 0 degrees would make a flag toggle look like a new preview (and mark a
    * saved one unsaved) although nothing on screen changes. */
   if (p_a->d_degrees != 0.0 && p_a->b_autocrop != p_b->b_autocrop) {
      return (FALSE);
   }
   return (!p_a->b_crop || croprect_equal(&p_a->t_crop, &p_b->t_crop));
}

void
transform_rotate_quarter(Transform *p_t, gint i_dir, gdouble d_base_w,
                         gdouble d_base_h) {
   g_return_if_fail(p_t != NULL);
   if (p_t->b_crop && (d_base_w <= 0.0 || d_base_h <= 0.0)) {
      p_t->b_crop = FALSE; /* no pixels to keep covering: see transform.h */
   } else if (p_t->b_crop) {
      croprect_rotate_quarter(&p_t->t_crop, i_dir, d_base_w, d_base_h);
   }
   p_t->i_quarter = (p_t->i_quarter + (i_dir > 0 ? 1 : 3)) % 4;
}

/* --- angles --------------------------------------------------------------- */

gdouble
transform_clamp_angle(gdouble d_deg) {
   if (!isfinite(d_deg)) {
      return (0.0); /* NaN/inf: round() would keep it and CLAMP pass it on */
   }
   gdouble d_snapped =
      round(d_deg / TRANSFORM_ANGLE_STEP) * TRANSFORM_ANGLE_STEP;
   d_snapped = CLAMP(d_snapped, -TRANSFORM_ANGLE_MAX, TRANSFORM_ANGLE_MAX);
   return (d_snapped == 0.0 ? 0.0 : d_snapped); /* no negative zero */
}

void
transform_nudge_angle(Transform *p_t, gdouble d_delta) {
   g_return_if_fail(p_t != NULL);
   if (!isfinite(d_delta)) {
      return; /* a NaN nudge is no nudge (clamping the sum would zero it) */
   }
   p_t->d_degrees = transform_clamp_angle(p_t->d_degrees + d_delta);
}

gdouble
transform_horizon_degrees(gdouble d_x0, gdouble d_y0, gdouble d_x1,
                          gdouble d_y1) {
   gdouble d_dx = d_x1 - d_x0;
   gdouble d_dy = d_y1 - d_y0;
   if (!isfinite(d_dx) || !isfinite(d_dy) ||
       (fabs(d_dx) < 1e-9 && fabs(d_dy) < 1e-9)) {
      return (0.0); /* nothing to level, or no coordinates to level by */
   }
   /* The line's on-screen angle, clockwise-positive because y points down.
    * Fold it into (-90, 90] so the drag direction does not matter. */
   gdouble d_theta = atan2(d_dy, d_dx) * 180.0 / G_PI;
   if (d_theta > 90.0) {
      d_theta -= 180.0;
   } else if (d_theta <= -90.0) {
      d_theta += 180.0;
   }
   /* A line sloping d_theta clockwise is levelled by turning the image the
    * same amount counter-clockwise, i.e. -d_theta clockwise. */
   return (transform_clamp_angle(-d_theta));
}

/* --- sizes ---------------------------------------------------------------- */

void
transform_autocrop_size(gdouble d_w, gdouble d_h, gdouble d_deg, gdouble *p_w,
                        gdouble *p_h) {
   g_return_if_fail(p_w != NULL && p_h != NULL);
   if (d_w <= 0.0 || d_h <= 0.0) {
      *p_w = 0.0;
      *p_h = 0.0;
      return;
   }
   gdouble d_a = fabs(d_deg) * G_PI / 180.0; /* symmetric in the sign */
   gdouble d_s = fabs(sin(d_a));
   gdouble d_c = fabs(cos(d_a));
   if (d_s < 1e-12) {
      *p_w = floor(d_w);
      *p_h = floor(d_h);
      return;
   }
   /* The classic largest-inscribed-rectangle construction: once the short
    * side is fully spanned by the rotated corners (or at 45 degrees) the
    * answer is the half-short-side scaled by the sines; otherwise both
    * sides come from the cos(2a) formula. */
   gboolean b_wide  = d_w >= d_h;
   gdouble  d_long  = b_wide ? d_w : d_h;
   gdouble  d_short = b_wide ? d_h : d_w;
   gdouble  d_rw, d_rh;
   if (d_short <= 2.0 * d_s * d_c * d_long || fabs(d_s - d_c) < 1e-10) {
      gdouble d_x = 0.5 * d_short;
      d_rw        = b_wide ? d_x / d_s : d_x / d_c;
      d_rh        = b_wide ? d_x / d_c : d_x / d_s;
   } else {
      gdouble d_cos2 = d_c * d_c - d_s * d_s;
      d_rw           = (d_w * d_c - d_h * d_s) / d_cos2;
      d_rh           = (d_h * d_c - d_w * d_s) / d_cos2;
   }
   /* The epsilon keeps an exact answer exact: at 90 degrees the maths gives
    * 49.999... for 50 and floor would lose a pixel. */
   *p_w = MAX(1.0, floor(d_rw + 1e-9));
   *p_h = MAX(1.0, floor(d_rh + 1e-9));
}

void
transform_rotated_size(gdouble d_w, gdouble d_h, gdouble d_deg, gdouble *p_w,
                       gdouble *p_h) {
   g_return_if_fail(p_w != NULL && p_h != NULL);
   gdouble d_a = fabs(d_deg) * G_PI / 180.0;
   gdouble d_s = fabs(sin(d_a));
   gdouble d_c = fabs(cos(d_a));
   *p_w        = ceil(d_w * d_c + d_h * d_s - 1e-9);
   *p_h        = ceil(d_w * d_s + d_h * d_c - 1e-9);
}

void
transform_straighten_size(gdouble d_w, gdouble d_h, gdouble d_deg,
                          gboolean b_autocrop, gdouble *p_w, gdouble *p_h) {
   g_return_if_fail(p_w != NULL && p_h != NULL);
   if (d_deg == 0.0) {
      *p_w = d_w;
      *p_h = d_h;
      return;
   }
   if (!b_autocrop) {
      transform_rotated_size(d_w, d_h, d_deg, p_w, p_h);
      return;
   }
   /* The inscribed rectangle minus the sampler's blend margin on every side
    * (TRANSFORM_AUTOCROP_INSET), floored at a pixel so a tiny image still
    * yields something the chain can crop to -- below 3 px per side that
    * floor lies inside the margin and the opacity guarantee is off (see the
    * define). */
   transform_autocrop_size(d_w, d_h, d_deg, p_w, p_h);
   *p_w = MAX(1.0, *p_w - 2.0 * TRANSFORM_AUTOCROP_INSET);
   *p_h = MAX(1.0, *p_h - 2.0 * TRANSFORM_AUTOCROP_INSET);
}

void
transform_base_size(const Transform *p_t, gdouble d_w, gdouble d_h,
                    gdouble *p_w, gdouble *p_h) {
   g_return_if_fail(p_t != NULL && p_w != NULL && p_h != NULL);
   gdouble d_bw = d_w;
   gdouble d_bh = d_h;
   if (p_t->i_quarter % 2 != 0) {
      d_bw = d_h;
      d_bh = d_w;
   }
   transform_straighten_size(d_bw, d_bh, p_t->d_degrees, p_t->b_autocrop, p_w,
                             p_h);
}

gboolean
transform_effective_crop(const Transform *p_t, gdouble d_base_w,
                         gdouble d_base_h, CropRect *p_out) {
   g_return_val_if_fail(p_t != NULL && p_out != NULL, FALSE);
   if (!p_t->b_crop || d_base_w < 1.0 || d_base_h < 1.0) {
      return (FALSE);
   }
   /* An intersection, not croprect_clamp: the tool's minimum size is a UI
    * rule, and growing a rectangle here would crop pixels the user never
    * chose (it also broke a 4x2 unit fixture, where the minimum is the
    * whole image). */
   *p_out = p_t->t_crop;
   croprect_intersect(p_out, d_base_w, d_base_h);
   if (p_out->d_w < 0.5 || p_out->d_h < 0.5) {
      return (FALSE);
   }
   croprect_round(p_out);
   return (p_out->d_w >= 1.0 && p_out->d_h >= 1.0);
}

gboolean
transform_rebase_crop(Transform *p_t, const Transform *p_old, gdouble d_orig_w,
                      gdouble d_orig_h) {
   g_return_val_if_fail(p_t != NULL && p_old != NULL, FALSE);
   if (!p_t->b_crop) {
      return (TRUE);
   }
   gdouble d_ow, d_oh, d_nw, d_nh;
   transform_base_size(p_old, d_orig_w, d_orig_h, &d_ow, &d_oh);
   transform_base_size(p_t, d_orig_w, d_orig_h, &d_nw, &d_nh);
   /* Same offset from the centre as before: the straighten rotates and the
    * auto-crop shrinks about the centre, so this keeps the rectangle over
    * the content it framed (the content under it turns, the frame does not
    * -- the closest thing to "the same pixels" a rotation allows). Only the
    * shift is stored: the stored rectangle stays whole and the cutting is
    * transform_effective_crop's job at render / export time. Storing the
    * cut rectangle eroded a border-touching crop for good on the first
    * nudge ({0,0,100,100} on 400x300, 5 degrees and back, came out as
    * {12,17,88,83}); a shift is exactly undone by the opposite shift. The
    * same goes for a rectangle pushed entirely outside: clearing b_crop
    * (an earlier version did) made "nudge away and back" lose it, so it is
    * only reported, and the chain crops nothing while it is out there. */
   p_t->t_crop.d_x += (d_nw - d_ow) / 2.0;
   p_t->t_crop.d_y += (d_nh - d_oh) / 2.0;
   CropRect t_eff;
   return (transform_effective_crop(p_t, d_nw, d_nh, &t_eff));
}

gboolean
transform_crop_is_outside(const Transform *p_t, gdouble d_orig_w,
                          gdouble d_orig_h) {
   g_return_val_if_fail(p_t != NULL, FALSE);
   if (!p_t->b_crop || d_orig_w <= 0.0 || d_orig_h <= 0.0) {
      return (FALSE);
   }
   gdouble  d_bw, d_bh;
   CropRect t_eff;
   transform_base_size(p_t, d_orig_w, d_orig_h, &d_bw, &d_bh);
   return (!transform_effective_crop(p_t, d_bw, d_bh, &t_eff));
}

void
transform_output_size(const Transform *p_t, gdouble d_w, gdouble d_h,
                      gdouble *p_w, gdouble *p_h) {
   g_return_if_fail(p_t != NULL && p_w != NULL && p_h != NULL);
   transform_base_size(p_t, d_w, d_h, p_w, p_h);
   CropRect t_crop;
   if (transform_effective_crop(p_t, *p_w, *p_h, &t_crop)) {
      *p_w = t_crop.d_w;
      *p_h = t_crop.d_h;
   }
}

/* --- description ---------------------------------------------------------- */

static void
_append_part(GString *p_str, const char *c_part) {
   if (p_str->len > 0) {
      g_string_append(p_str, ", ");
   }
   g_string_append(p_str, c_part);
}

char *
transform_describe(const Transform *p_t, gdouble d_orig_w, gdouble d_orig_h) {
   g_return_val_if_fail(p_t != NULL, NULL);
   if (transform_is_identity(p_t)) {
      return (NULL);
   }
   GString *p_str = g_string_new(NULL);
   switch (p_t->i_quarter) {
   case 1:
      _append_part(p_str, "90° CW");
      break;
   case 2:
      _append_part(p_str, "180°");
      break;
   case 3:
      _append_part(p_str, "90° CCW");
      break;
   default:
      break;
   }
   if (p_t->d_degrees != 0.0) {
      /* g_ascii_formatd: a decimal point regardless of locale. */
      char c_num[G_ASCII_DTOSTR_BUF_SIZE];
      g_ascii_formatd(c_num, sizeof(c_num), "%.1f", fabs(p_t->d_degrees));
      char *c_part = g_strdup_printf("straighten %s° %s", c_num,
                                     p_t->d_degrees > 0.0 ? "CW" : "CCW");
      _append_part(p_str, c_part);
      g_free(c_part);
   }
   if (p_t->b_crop) {
      /* Truthful about a crop the chain is not applying right now. */
      _append_part(p_str, transform_crop_is_outside(p_t, d_orig_w, d_orig_h)
                             ? "crop (outside view)"
                             : "crop");
   }
   return (g_string_free(p_str, FALSE));
}
