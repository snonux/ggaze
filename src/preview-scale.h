#ifndef GGAZE_PREVIEW_SCALE_H
#define GGAZE_PREVIEW_SCALE_H

/*:*
 * ggaze — how far the live enhance preview is scaled down (plain C)
 *
 * The edit panel's live preview (8l2, decision #53) does not run the GEGL
 * chain on the full-resolution image: a 64 MP camera photo took ~10 s per
 * key press that way. It renders from a SOURCE -- the image decoded once
 * and scaled down to about what the screen can show -- and only the export
 * (`s`, the save gate's Save) runs at full resolution. This module is the
 * arithmetic that decision rests on, kept free of GTK and GEGL so it is
 * unit-tested in every lane (tests/test_preview_scale.c):
 *
 *   - the scale a source needs for a viewport: the image's fit-to-window
 *     ratio times the device scale times PREVIEW_SCALE_OVERSAMPLE (head
 *     room for a zoom step or a larger window), never above 1 (a source is
 *     never upscaled), its long side never below PREVIEW_SCALE_MIN_SIDE
 *     (the card thumbnails are cut from it) and never above a cap;
 *   - whether a source made at one scale still serves a viewport that
 *     wants another (a smaller window keeps it; a larger one rebuilds);
 *   - the thumbnail scale of the cards (PREVIEW_SCALE_THUMB_SIDE);
 *   - which GEGL op properties are LENGTHS in pixels, so a preset run on a
 *     source scaled by s gets them scaled by s too and looks on the
 *     preview as it will in the export (an unsharp mask's 3 px radius is
 *     0.6 px on a fifth-size source, not 3 px -- five times too wide).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <glib.h>

G_BEGIN_DECLS

/* How many source pixels per screen pixel at fit-to-window: 1.5 keeps a
 * zoom step (1.25x) and a moderately larger window sharp without a
 * rebuild, and keeps the chain's cost -- which grows with the pixel count
 * (a 4-iteration noise reduction: ~0.9 ms per 1000 px) -- near the
 * screen's. Measured on a 1280x800 window: a 64 MP photo's source is then
 * ~1400x1050. */
#define PREVIEW_SCALE_OVERSAMPLE 1.5

/* A source's long side is at least this (or the whole image when that is
 * smaller): a tiny window must not make the card thumbnails, which are cut
 * from the source, or a zoom step, look like mush. */
#define PREVIEW_SCALE_MIN_SIDE 512

/* ... and at most this, whatever the display: a render beyond it costs
 * seconds again (a 5K screen at 2x would otherwise ask for ~40 MP). */
#define PREVIEW_SCALE_MAX_SIDE 4096

/* The viewport assumed while the viewer has no size yet (a window not
 * presented, the grid in front). */
#define PREVIEW_SCALE_DEFAULT_VIEW_W 1920
#define PREVIEW_SCALE_DEFAULT_VIEW_H 1080

/* The long side of a card thumbnail's source (the cards are 60x40 logical
 * px: twice that, for a 2x display). */
#define PREVIEW_SCALE_THUMB_SIDE 128

/* The viewport a preview is for: the large view's size in logical pixels
 * (<= 0: not known yet, PREVIEW_SCALE_DEFAULT_VIEW_*), its device scale
 * factor (<= 0: 1), and an extra cap on the source's long side (0: none;
 * a test seam forces a small source with it). */
typedef struct {
   gint i_w;
   gint i_h;
   gint i_device_scale;
   gint i_max_side;
} PreviewView;

/* The scale (0, 1] a source of an i_img_w x i_img_h image needs for
 * p_view (see the header). 1 for an empty image. */
gdouble preview_scale_for_view(gint i_img_w, gint i_img_h,
                               const PreviewView *p_view);

/* The i_img_w x i_img_h image at d_scale: each side rounded, at least 1. */
void preview_scale_size(gint i_img_w, gint i_img_h, gdouble d_scale, gint *p_w,
                        gint *p_h);

/* TRUE iff a source made at d_have serves a view that wants d_want: it is
 * the whole image (1), or at least as fine (to a rounding's worth). A
 * source finer than wanted is kept -- rebuilding it smaller would only
 * cost a decode. */
gboolean preview_scale_covers(gdouble d_have, gdouble d_want);

/* The scale (0, 1] that brings an i_w x i_h source down to a card
 * thumbnail (PREVIEW_SCALE_THUMB_SIDE on its long side, never up). */
gdouble preview_scale_thumb(gint i_w, gint i_h);

/* TRUE iff property c_prop of the GEGL op c_op ("gegl:unsharp-mask",
 * "std-dev") is a length in image pixels -- a blur radius, a standard
 * deviation, a cell size, an offset -- which must scale with the image
 * for the preset to look the same on a scaled source. Counts and ratios
 * are not: noise-reduction's and mean-curvature-blur's iterations stay
 * (a documented deviation: a 3x3 kernel iterated n times has no scaled
 * equivalent), and so do vignette's radius (relative to the image) and
 * every colour or strength. */
gboolean preview_scale_is_length(const char *c_op, const char *c_prop);

/* d_value (a length, above) on an image scaled by d_scale, never below
 * d_min (the property's own minimum). */
gdouble preview_scale_length(gdouble d_value, gdouble d_scale, gdouble d_min);

G_END_DECLS

#endif /* GGAZE_PREVIEW_SCALE_H */
