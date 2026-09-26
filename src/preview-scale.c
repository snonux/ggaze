/*:*
 * ggaze — how far the live enhance preview is scaled down
 *
 * See preview-scale.h.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "preview-scale.h"

#include <math.h>
#include <string.h>

#include <glib.h>

/* Two scales closer than this are the same (a rounded side's worth on any
 * image ggaze decodes: 1 px in 32768 is 3e-5). */
#define _SCALE_EPS 1e-4

/* The GEGL op properties that are lengths in image pixels (gegl 0.4.x
 * names, checked against the installed ops' property lists). A property
 * an op does not have is simply not touched by the enhancer, so an entry
 * here costs nothing on a GEGL without it. */
static const struct {
   const char *c_op;
   const char *c_prop;
} LENGTHS[] = {
   {"gegl:unsharp-mask", "std-dev"},
   {"gegl:gaussian-blur", "std-dev-x"},
   {"gegl:gaussian-blur", "std-dev-y"},
   {"gegl:high-pass", "std-dev"},
   {"gegl:difference-of-gaussians", "radius1"},
   {"gegl:difference-of-gaussians", "radius2"},
   {"gegl:box-blur", "radius"},
   {"gegl:median-blur", "radius"},
   {"gegl:snn-mean", "radius"},
   {"gegl:bilateral-filter", "blur-radius"},
   {"gegl:pixelize", "size-x"},
   {"gegl:pixelize", "size-y"},
   {"gegl:motion-blur-linear", "length"},
   {"gegl:lens-blur", "radius"},
   {"gegl:variable-blur", "radius"},
   {"gegl:bloom", "radius"},
   {"gegl:edge-neon", "radius"},
   {"gegl:local-threshold", "radius"},
   {"gegl:dropshadow", "x"},
   {"gegl:dropshadow", "y"},
   {"gegl:dropshadow", "radius"},
   {"gegl:dropshadow", "grow-radius"},
};

gdouble
preview_scale_for_view(gint i_img_w, gint i_img_h, const PreviewView *p_view) {
   g_return_val_if_fail(p_view != NULL, 1.0);
   if (i_img_w <= 0 || i_img_h <= 0) {
      return (1.0);
   }
   gint    i_vw  = p_view->i_w > 0 ? p_view->i_w : PREVIEW_SCALE_DEFAULT_VIEW_W;
   gint    i_vh  = p_view->i_h > 0 ? p_view->i_h : PREVIEW_SCALE_DEFAULT_VIEW_H;
   gint    i_dev = p_view->i_device_scale > 0 ? p_view->i_device_scale : 1;
   gdouble d_long = (gdouble)MAX(i_img_w, i_img_h);
   gdouble d_fit  = MIN((gdouble)i_vw / i_img_w, (gdouble)i_vh / i_img_h);
   gdouble d_s    = d_fit * i_dev * PREVIEW_SCALE_OVERSAMPLE;
   d_s            = MAX(d_s, PREVIEW_SCALE_MIN_SIDE / d_long);
   d_s            = MIN(d_s, PREVIEW_SCALE_MAX_SIDE / d_long);
   if (p_view->i_max_side > 0) {
      d_s = MIN(d_s, p_view->i_max_side / d_long);
   }
   return (CLAMP(d_s, 1.0 / d_long, 1.0));
}

void
preview_scale_size(gint i_img_w, gint i_img_h, gdouble d_scale, gint *p_w,
                   gint *p_h) {
   g_return_if_fail(p_w != NULL && p_h != NULL);
   *p_w = MAX(1, (gint)lround(i_img_w * d_scale));
   *p_h = MAX(1, (gint)lround(i_img_h * d_scale));
}

gboolean
preview_scale_covers(gdouble d_have, gdouble d_want) {
   return (d_have >= 1.0 - _SCALE_EPS ||
           d_have * PREVIEW_SCALE_OVERSAMPLE >= d_want - _SCALE_EPS);
}

gdouble
preview_scale_thumb(gint i_w, gint i_h) {
   gint i_long = MAX(i_w, i_h);
   if (i_long <= PREVIEW_SCALE_THUMB_SIDE) {
      return (1.0);
   }
   return ((gdouble)PREVIEW_SCALE_THUMB_SIDE / i_long);
}

gboolean
preview_scale_is_length(const char *c_op, const char *c_prop) {
   if (c_op == NULL || c_prop == NULL) {
      return (FALSE);
   }
   for (guint u = 0; u < G_N_ELEMENTS(LENGTHS); u++) {
      if (strcmp(LENGTHS[u].c_op, c_op) == 0 &&
          strcmp(LENGTHS[u].c_prop, c_prop) == 0) {
         return (TRUE);
      }
   }
   return (FALSE);
}

gdouble
preview_scale_length(gdouble d_value, gdouble d_scale, gdouble d_min) {
   return (MAX(d_value * d_scale, d_min));
}
