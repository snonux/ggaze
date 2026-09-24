/*:*
 * ggaze — zoom and touch-gesture math (plain C, no GTK)
 *
 * See gesture-math.h. Every function here is pure; viewer.c owns the
 * widget state and only stores what these return.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "gesture-math.h"

#include <math.h>

gdouble
gesture_math_clamp_zoom(gdouble d_zoom, gdouble d_fit) {
   /* Both limits give way to the fit ratio, symmetrically. The ceiling is
    * the normal limit or the fit ratio, whichever is larger (jx0): when
    * fit-to-window already exceeds GGAZE_ZOOM_MAX, clamping to the bare
    * limit turned the first zoom-in into a shrink. The floor is the normal
    * limit or the fit ratio, whichever is SMALLER (zb2 fourth review): a
    * 32768 px panorama in a 600 px window fits at 1.83 %, below the 2 %
    * floor, and clamping to the bare floor turned every zoom-out from fit
    * -- `-`, the wheel, a pinch in -- into an enlargement (and made a
    * pinch jump ~9 % leaving the fit detent). With either limit moved to
    * fit, the far end is a no-op instead of a reversal. A non-finite or
    * non-positive fit (never produced by the viewer) is ignored rather
    * than allowed to move a limit to infinity or to zero. */
   gdouble d_min = GGAZE_ZOOM_MIN;
   gdouble d_max = GGAZE_ZOOM_MAX;
   if (isfinite(d_fit) && d_fit > d_max) {
      d_max = d_fit;
   }
   if (isfinite(d_fit) && d_fit > 0.0 && d_fit < d_min) {
      d_min = d_fit;
   }
   return (CLAMP(d_zoom, d_min, d_max));
}

/* TRUE when every number the zoom derives from is finite. CLAMP cannot
 * filter NaN -- both of its comparisons are false, so NaN passes straight
 * through -- hence the explicit check (hx0). */
static gboolean
_view_is_finite(const GestureView *p_view, gdouble d_cx, gdouble d_cy,
                gdouble d_zoom) {
   return (isfinite(d_cx) && isfinite(d_cy) && isfinite(d_zoom) &&
           isfinite(p_view->d_scale) && isfinite(p_view->d_x) &&
           isfinite(p_view->d_y) && isfinite(p_view->d_fit));
}

gboolean
gesture_math_zoom_about(const GestureView *p_view, gdouble d_cx, gdouble d_cy,
                        gdouble d_zoom, gdouble *p_zoom, gdouble *p_pan_x,
                        gdouble *p_pan_y) {
   g_return_val_if_fail(p_view != NULL, FALSE);
   if (!_view_is_finite(p_view, d_cx, d_cy, d_zoom)) {
      return (FALSE);
   }
   gdouble d_z = gesture_math_clamp_zoom(d_zoom, p_view->d_fit);
   /* The image pixel under the point now (0 for a view not drawn yet, so
    * a zoom there centres on the image's top-left rather than dividing by
    * zero) ... */
   gdouble d_s     = p_view->d_scale;
   gdouble d_img_x = (d_s > 0.0) ? (d_cx - p_view->d_x) / d_s : 0.0;
   gdouble d_img_y = (d_s > 0.0) ? (d_cy - p_view->d_y) / d_s : 0.0;
   /* ... goes back under it at the new zoom: that is where the top-left
    * must be, expressed as an offset from the centred position. The
    * viewer's geometry clamps it on the next draw. */
   gdouble d_want_x = d_cx - d_img_x * d_z;
   gdouble d_want_y = d_cy - d_img_y * d_z;
   *p_zoom          = d_z;
   *p_pan_x = d_want_x - ((gdouble)p_view->i_w - p_view->i_tex_w * d_z) / 2.0;
   *p_pan_y = d_want_y - ((gdouble)p_view->i_h - p_view->i_tex_h * d_z) / 2.0;
   return (TRUE);
}

gboolean
gesture_math_pinch_zoom(gdouble d_start_zoom, gdouble d_scale,
                        gdouble *p_zoom) {
   if (!isfinite(d_start_zoom) || !isfinite(d_scale) || d_start_zoom <= 0.0 ||
       d_scale <= 0.0) {
      return (FALSE);
   }
   *p_zoom = d_start_zoom * d_scale;
   return (isfinite(*p_zoom));
}

gdouble
gesture_math_detent_scale(gdouble d_scale) {
   const gdouble d_band = GESTURE_TAP_MAX_SCALE_DEV;
   if (!isfinite(d_scale)) {
      return (d_scale);
   }
   if (d_scale > 1.0 + d_band) {
      return (d_scale / (1.0 + d_band));
   }
   if (d_scale < 1.0 - d_band) {
      return (d_scale / (1.0 - d_band));
   }
   return (1.0); /* inside the detent: fit */
}

gint
gesture_math_swipe_direction(gdouble d_dx, gdouble d_dy, gdouble d_vx,
                             gdouble d_vy) {
   if (!isfinite(d_dx) || !isfinite(d_dy) || !isfinite(d_vx) ||
       !isfinite(d_vy)) {
      return (0);
   }
   if (fabs(d_dx) < GESTURE_SWIPE_MIN_DISTANCE ||
       fabs(d_dy) > GESTURE_SWIPE_MAX_SLOPE * fabs(d_dx) ||
       fabs(d_vx) < GESTURE_SWIPE_MIN_VELOCITY) {
      return (0);
   }
   /* A drag that ends moving back against its own direction is someone
    * changing their mind, not a flick. */
   if ((d_dx > 0.0) != (d_vx > 0.0)) {
      return (0);
   }
   return ((d_dx < 0.0) ? 1 : -1);
}

gboolean
gesture_math_is_two_finger_tap(gint64 i_duration_us, gdouble d_max_move,
                               gdouble d_max_scale_dev) {
   if (i_duration_us < 0 || i_duration_us > GESTURE_TAP_MAX_US) {
      return (FALSE);
   }
   if (!isfinite(d_max_move) || !isfinite(d_max_scale_dev) ||
       d_max_move < 0.0 || d_max_scale_dev < 0.0) {
      return (FALSE);
   }
   return (d_max_move <= GESTURE_TAP_MAX_MOVE &&
           d_max_scale_dev <= GESTURE_TAP_MAX_SCALE_DEV);
}
