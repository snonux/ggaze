#ifndef GGAZE_CROPRECT_H
#define GGAZE_CROPRECT_H

/*:*
 * ggaze — crop rectangle geometry (plain C, no GTK, no GEGL)
 *
 * The rectangle the `c` crop tool edits, and every rule about how it moves:
 * keyboard nudges and mouse drags translate or resize it, an aspect-ratio
 * lock keeps its shape, and it can never leave the image or shrink below a
 * usable size. All coordinates are pixels of the image the rectangle sits
 * on (the composed, pre-crop preview), y down, origin top-left. The bounds
 * (d_w, d_h) are passed into every editing call rather than stored, because
 * the image they refer to can change under the tool (a 90-degree turn, a
 * straighten) and the caller is the one who knows the new size.
 *
 * Pure functions of their arguments; unit-tested without a display
 * (tests/test_croprect.c). The GtkWidget side -- drawing the overlay and
 * turning pointer events into these calls -- lives in tool-ctrl.c.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <glib.h>

G_BEGIN_DECLS

/* The smallest width/height a rectangle may be edited down to, in image
 * pixels. Below this a corner drag can invert the rectangle and the GEGL crop
 * would produce a sliver nobody wants; 8 px is still visibly a rectangle. */
#define CROPRECT_MIN_SIZE 8.0

typedef struct {
   gdouble d_x; /* left edge */
   gdouble d_y; /* top edge */
   gdouble d_w; /* width, > 0 */
   gdouble d_h; /* height, > 0 */
} CropRect;

/* What a pointer position is over: nothing, the interior (drag = move), one
 * edge (drag = resize that edge) or one corner (drag = resize two edges). */
typedef enum {
   CROPRECT_HIT_NONE = 0,
   CROPRECT_HIT_INSIDE,
   CROPRECT_HIT_LEFT,
   CROPRECT_HIT_RIGHT,
   CROPRECT_HIT_TOP,
   CROPRECT_HIT_BOTTOM,
   CROPRECT_HIT_TOP_LEFT,
   CROPRECT_HIT_TOP_RIGHT,
   CROPRECT_HIT_BOTTOM_LEFT,
   CROPRECT_HIT_BOTTOM_RIGHT
} CropRectHit;

/* Cover the whole d_w x d_h image. */
void croprect_init_full(CropRect *p_r, gdouble d_w, gdouble d_h);

/* TRUE iff p_r covers the whole d_w x d_h image (within half a pixel), i.e.
 * applying it as a crop would change nothing. */
gboolean croprect_is_full(const CropRect *p_r, gdouble d_w, gdouble d_h);

/* TRUE iff both rectangles have the same edges (within 1e-6). */
gboolean croprect_equal(const CropRect *p_a, const CropRect *p_b);

/* Force p_r inside the d_w x d_h image -- an edge past the border is pulled
 * back to it, the rest stays put (an intersection, not a slide) -- and at
 * least CROPRECT_MIN_SIZE on each side (the image itself may be smaller than
 * that: then the rectangle is the image). Every editing function ends with
 * this, so a rectangle is valid after any sequence of calls. */
void croprect_clamp(CropRect *p_r, gdouble d_w, gdouble d_h);

/* Cut p_r down to the part inside the d_w x d_h image -- a plain
 * intersection with no minimum size (what the GEGL crop wants: a rectangle
 * drawn on a base that has since shrunk keeps the pixels it still covers).
 * The result can be empty (d_w or d_h zero); the caller checks. */
void croprect_intersect(CropRect *p_r, gdouble d_w, gdouble d_h);

/* Translate by (d_dx, d_dy), sliding along the image edge rather than
 * leaving it (the size is preserved). */
void croprect_move(CropRect *p_r, gdouble d_dx, gdouble d_dy, gdouble d_w,
                   gdouble d_h);

/* Move the edge(s) named by e_edge by (d_dx, d_dy): a horizontal edge only
 * reads d_dx, a vertical one only d_dy, a corner both. d_aspect > 0 locks
 * width/height to that ratio (w/h): the dragged dimension leads and the
 * other follows, anchored on the opposite edge (or centred, for an edge
 * that does not touch that axis). Then clamped. CROPRECT_HIT_INSIDE and
 * _NONE are no-ops. */
void croprect_resize(CropRect *p_r, CropRectHit e_edge, gdouble d_dx,
                     gdouble d_dy, gdouble d_aspect, gdouble d_w, gdouble d_h);

/* Re-fit p_r to the aspect ratio d_aspect (w/h; 0 = free, a no-op) around
 * its own centre: the largest rectangle of that shape that fits inside the
 * current one, slid into the image if the centre was near an edge. */
void croprect_set_aspect(CropRect *p_r, gdouble d_aspect, gdouble d_w,
                         gdouble d_h);

/* Classify the point (d_x, d_y): a corner when within d_tol of both of its
 * edges, else an edge when within d_tol of it (and within the rectangle's
 * span along it, plus d_tol), else INSIDE when inside, else NONE. Corners win
 * over edges so a small rectangle can still be resized diagonally. */
CropRectHit croprect_hit(const CropRect *p_r, gdouble d_x, gdouble d_y,
                         gdouble d_tol);

/* Apply a drag that started with the rectangle at *p_start over the hit
 * e_hit and has moved (d_dx, d_dy) in total: INSIDE moves, an edge/corner
 * resizes (with the aspect lock), NONE does nothing. *p_r receives the
 * result -- always computed from p_start, so a drag is not path-dependent
 * and never accumulates clamping error. */
void croprect_drag(CropRect *p_r, const CropRect *p_start, CropRectHit e_hit,
                   gdouble d_dx, gdouble d_dy, gdouble d_aspect, gdouble d_w,
                   gdouble d_h);

/* Turn the rectangle with its image by a quarter turn: i_dir > 0 clockwise,
 * < 0 counter-clockwise. d_w/d_h are the image's dimensions BEFORE the turn
 * (they are swapped by it). The rectangle keeps covering the same pixels. */
void croprect_rotate_quarter(CropRect *p_r, gint i_dir, gdouble d_w,
                             gdouble d_h);

/* Snap the edges to whole pixels (the GEGL crop takes integers; rounding the
 * edges rather than the size keeps the rectangle where it was drawn). */
void croprect_round(CropRect *p_r);

G_END_DECLS

#endif /* GGAZE_CROPRECT_H */
