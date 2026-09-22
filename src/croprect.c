/*:*
 * ggaze — crop rectangle geometry
 *
 * See croprect.h. Every editing entry point ends in croprect_clamp, so the
 * rectangle is always inside its image and at least CROPRECT_MIN_SIZE on a
 * side, whatever sequence of nudges and drags produced it.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "croprect.h"

#include <math.h>

#include <glib.h>

/* Which point of the rectangle stays put along one axis while a resize
 * changes the size on that axis. */
typedef enum {
   _ANCHOR_LOW,   /* the low edge (left / top) is fixed */
   _ANCHOR_HIGH,  /* the high edge (right / bottom) is fixed */
   _ANCHOR_CENTRE /* the centre is fixed (the edge being dragged is on the
                   * other axis, so this axis follows symmetrically) */
} _Anchor;

/* --- hit-kind predicates -------------------------------------------------- */

static gboolean
_touches_left(CropRectHit e) {
   return (e == CROPRECT_HIT_LEFT || e == CROPRECT_HIT_TOP_LEFT ||
           e == CROPRECT_HIT_BOTTOM_LEFT);
}

static gboolean
_touches_right(CropRectHit e) {
   return (e == CROPRECT_HIT_RIGHT || e == CROPRECT_HIT_TOP_RIGHT ||
           e == CROPRECT_HIT_BOTTOM_RIGHT);
}

static gboolean
_touches_top(CropRectHit e) {
   return (e == CROPRECT_HIT_TOP || e == CROPRECT_HIT_TOP_LEFT ||
           e == CROPRECT_HIT_TOP_RIGHT);
}

static gboolean
_touches_bottom(CropRectHit e) {
   return (e == CROPRECT_HIT_BOTTOM || e == CROPRECT_HIT_BOTTOM_LEFT ||
           e == CROPRECT_HIT_BOTTOM_RIGHT);
}

/* --- basics --------------------------------------------------------------- */

void
croprect_init_full(CropRect *p_r, gdouble d_w, gdouble d_h) {
   g_return_if_fail(p_r != NULL);
   p_r->d_x = 0.0;
   p_r->d_y = 0.0;
   p_r->d_w = MAX(d_w, 0.0);
   p_r->d_h = MAX(d_h, 0.0);
}

gboolean
croprect_is_full(const CropRect *p_r, gdouble d_w, gdouble d_h) {
   g_return_val_if_fail(p_r != NULL, FALSE);
   return (fabs(p_r->d_x) < 0.5 && fabs(p_r->d_y) < 0.5 &&
           fabs(p_r->d_w - d_w) < 0.5 && fabs(p_r->d_h - d_h) < 0.5);
}

gboolean
croprect_equal(const CropRect *p_a, const CropRect *p_b) {
   g_return_val_if_fail(p_a != NULL && p_b != NULL, FALSE);
   return (
      fabs(p_a->d_x - p_b->d_x) < 1e-6 && fabs(p_a->d_y - p_b->d_y) < 1e-6 &&
      fabs(p_a->d_w - p_b->d_w) < 1e-6 && fabs(p_a->d_h - p_b->d_h) < 1e-6);
}

void
croprect_clamp(CropRect *p_r, gdouble d_w, gdouble d_h) {
   g_return_if_fail(p_r != NULL);
   d_w = MAX(d_w, 0.0);
   d_h = MAX(d_h, 0.0);
   /* Intersect with the image first (an edge past the border is pulled
    * back to it; the rest of the rectangle stays where it was), THEN grow
    * to the minimum and slide back in if that overshot. The floor is the
    * image itself when that is smaller than the minimum: a 6x3 fixture must
    * still yield a rectangle, just the whole image. */
   gdouble d_min_w = MIN(CROPRECT_MIN_SIZE, d_w);
   gdouble d_min_h = MIN(CROPRECT_MIN_SIZE, d_h);
   p_r->d_x        = CLAMP(p_r->d_x, 0.0, d_w);
   p_r->d_y        = CLAMP(p_r->d_y, 0.0, d_h);
   p_r->d_w        = MAX(MIN(p_r->d_w, d_w - p_r->d_x), d_min_w);
   p_r->d_h        = MAX(MIN(p_r->d_h, d_h - p_r->d_y), d_min_h);
   p_r->d_x        = CLAMP(p_r->d_x, 0.0, d_w - p_r->d_w);
   p_r->d_y        = CLAMP(p_r->d_y, 0.0, d_h - p_r->d_h);
}

void
croprect_intersect(CropRect *p_r, gdouble d_w, gdouble d_h) {
   g_return_if_fail(p_r != NULL);
   gdouble d_l = CLAMP(p_r->d_x, 0.0, MAX(d_w, 0.0));
   gdouble d_t = CLAMP(p_r->d_y, 0.0, MAX(d_h, 0.0));
   gdouble d_r = CLAMP(p_r->d_x + p_r->d_w, 0.0, MAX(d_w, 0.0));
   gdouble d_b = CLAMP(p_r->d_y + p_r->d_h, 0.0, MAX(d_h, 0.0));
   p_r->d_x    = d_l;
   p_r->d_y    = d_t;
   p_r->d_w    = MAX(d_r - d_l, 0.0);
   p_r->d_h    = MAX(d_b - d_t, 0.0);
}

void
croprect_move(CropRect *p_r, gdouble d_dx, gdouble d_dy, gdouble d_w,
              gdouble d_h) {
   g_return_if_fail(p_r != NULL);
   /* Clamp the POSITION, not the rectangle: a move keeps its size and stops
    * at the border (croprect_clamp alone would shrink a rectangle pushed
    * past it). */
   p_r->d_x += d_dx;
   p_r->d_y += d_dy;
   p_r->d_x = CLAMP(p_r->d_x, 0.0, MAX(0.0, d_w - p_r->d_w));
   p_r->d_y = CLAMP(p_r->d_y, 0.0, MAX(0.0, d_h - p_r->d_h));
   croprect_clamp(p_r, d_w, d_h);
}

/* --- resize --------------------------------------------------------------- */

/* Move the edge(s) e names by the deltas, never past the opposite edge
 * minus the minimum size (so a left-edge drag stops at right - min rather
 * than inverting the rectangle). */
static void
_move_edges(CropRect *p_r, CropRectHit e, gdouble d_dx, gdouble d_dy,
            gdouble d_min_w, gdouble d_min_h) {
   gdouble d_right  = p_r->d_x + p_r->d_w;
   gdouble d_bottom = p_r->d_y + p_r->d_h;
   if (_touches_left(e)) {
      p_r->d_x = MIN(p_r->d_x + d_dx, d_right - d_min_w);
      p_r->d_w = d_right - p_r->d_x;
   } else if (_touches_right(e)) {
      p_r->d_w = MAX(p_r->d_w + d_dx, d_min_w);
   }
   if (_touches_top(e)) {
      p_r->d_y = MIN(p_r->d_y + d_dy, d_bottom - d_min_h);
      p_r->d_h = d_bottom - p_r->d_y;
   } else if (_touches_bottom(e)) {
      p_r->d_h = MAX(p_r->d_h + d_dy, d_min_h);
   }
}

/* The anchor on the x axis for a drag of e: dragging the left edge keeps
 * the right one, and vice versa; an edge on the other axis keeps the
 * centre. */
static _Anchor
_anchor_x(CropRectHit e) {
   if (_touches_left(e)) {
      return (_ANCHOR_HIGH);
   }
   if (_touches_right(e)) {
      return (_ANCHOR_LOW);
   }
   return (_ANCHOR_CENTRE);
}

static _Anchor
_anchor_y(CropRectHit e) {
   if (_touches_top(e)) {
      return (_ANCHOR_HIGH);
   }
   if (_touches_bottom(e)) {
      return (_ANCHOR_LOW);
   }
   return (_ANCHOR_CENTRE);
}

/* The largest size along one axis that keeps the anchor where it is and
 * stays inside [0, d_bound]. d_lo/d_size describe the current extent. */
static gdouble
_room(_Anchor e_anchor, gdouble d_lo, gdouble d_size, gdouble d_bound) {
   gdouble d_centre = d_lo + d_size / 2.0;
   switch (e_anchor) {
   case _ANCHOR_LOW:
      return (d_bound - d_lo);
   case _ANCHOR_HIGH:
      return (d_lo + d_size);
   case _ANCHOR_CENTRE:
   default:
      return (2.0 * MIN(d_centre, d_bound - d_centre));
   }
}

/* Re-place the low edge so that resizing from d_old to d_new keeps the
 * anchor point fixed. */
static void
_place(_Anchor e_anchor, gdouble *p_lo, gdouble d_old, gdouble d_new) {
   switch (e_anchor) {
   case _ANCHOR_HIGH:
      *p_lo += d_old - d_new;
      break;
   case _ANCHOR_CENTRE:
      *p_lo += (d_old - d_new) / 2.0;
      break;
   case _ANCHOR_LOW:
   default:
      break;
   }
}

/* Give p_r the aspect d_aspect after a drag of e: the dragged dimension
 * leads (only a pure top/bottom drag lets the height lead), the other
 * follows, and if the shape then does not fit the room its anchors leave
 * it, both shrink together so the shape survives and the rectangle stays
 * inside the image. */
static void
_fit_aspect(CropRect *p_r, CropRectHit e, gdouble d_aspect, gdouble d_w,
            gdouble d_h) {
   gboolean b_height_leads =
      (e == CROPRECT_HIT_TOP || e == CROPRECT_HIT_BOTTOM);
   _Anchor e_ax     = _anchor_x(e);
   _Anchor e_ay     = _anchor_y(e);
   gdouble d_room_w = _room(e_ax, p_r->d_x, p_r->d_w, d_w);
   gdouble d_room_h = _room(e_ay, p_r->d_y, p_r->d_h, d_h);
   gdouble d_nw, d_nh;
   if (b_height_leads) {
      d_nh = p_r->d_h;
      d_nw = d_nh * d_aspect;
   } else {
      d_nw = p_r->d_w;
      d_nh = d_nw / d_aspect;
   }
   if (d_nw > d_room_w) {
      d_nw = d_room_w;
      d_nh = d_nw / d_aspect;
   }
   if (d_nh > d_room_h) {
      d_nh = d_room_h;
      d_nw = d_nh * d_aspect;
   }
   _place(e_ax, &p_r->d_x, p_r->d_w, d_nw);
   _place(e_ay, &p_r->d_y, p_r->d_h, d_nh);
   p_r->d_w = d_nw;
   p_r->d_h = d_nh;
}

void
croprect_resize(CropRect *p_r, CropRectHit e_edge, gdouble d_dx, gdouble d_dy,
                gdouble d_aspect, gdouble d_w, gdouble d_h) {
   g_return_if_fail(p_r != NULL);
   if (e_edge == CROPRECT_HIT_NONE || e_edge == CROPRECT_HIT_INSIDE) {
      return;
   }
   _move_edges(p_r, e_edge, d_dx, d_dy, MIN(CROPRECT_MIN_SIZE, d_w),
               MIN(CROPRECT_MIN_SIZE, d_h));
   if (d_aspect > 0.0) {
      _fit_aspect(p_r, e_edge, d_aspect, d_w, d_h);
   }
   croprect_clamp(p_r, d_w, d_h);
}

void
croprect_set_aspect(CropRect *p_r, gdouble d_aspect, gdouble d_w, gdouble d_h) {
   g_return_if_fail(p_r != NULL);
   if (d_aspect <= 0.0) {
      return; /* free: keep whatever shape the user made */
   }
   /* The largest rectangle of that shape inside the current one, centred
    * on it, so switching 3:2 -> 1:1 -> 3:2 stays put rather than drifting. */
   gdouble d_nw = p_r->d_w;
   gdouble d_nh = d_nw / d_aspect;
   if (d_nh > p_r->d_h) {
      d_nh = p_r->d_h;
      d_nw = d_nh * d_aspect;
   }
   p_r->d_x += (p_r->d_w - d_nw) / 2.0;
   p_r->d_y += (p_r->d_h - d_nh) / 2.0;
   p_r->d_w = d_nw;
   p_r->d_h = d_nh;
   croprect_clamp(p_r, d_w, d_h);
}

/* --- pointer -------------------------------------------------------------- */

CropRectHit
croprect_hit(const CropRect *p_r, gdouble d_x, gdouble d_y, gdouble d_tol) {
   g_return_val_if_fail(p_r != NULL, CROPRECT_HIT_NONE);
   gdouble  d_l    = p_r->d_x;
   gdouble  d_r    = p_r->d_x + p_r->d_w;
   gdouble  d_t    = p_r->d_y;
   gdouble  d_b    = p_r->d_y + p_r->d_h;
   gboolean b_nl   = fabs(d_x - d_l) <= d_tol;
   gboolean b_nr   = fabs(d_x - d_r) <= d_tol;
   gboolean b_nt   = fabs(d_y - d_t) <= d_tol;
   gboolean b_nb   = fabs(d_y - d_b) <= d_tol;
   gboolean b_in_x = d_x >= d_l - d_tol && d_x <= d_r + d_tol;
   gboolean b_in_y = d_y >= d_t - d_tol && d_y <= d_b + d_tol;
   if (b_nl && b_nt) {
      return (CROPRECT_HIT_TOP_LEFT);
   }
   if (b_nr && b_nt) {
      return (CROPRECT_HIT_TOP_RIGHT);
   }
   if (b_nl && b_nb) {
      return (CROPRECT_HIT_BOTTOM_LEFT);
   }
   if (b_nr && b_nb) {
      return (CROPRECT_HIT_BOTTOM_RIGHT);
   }
   if (b_nl && b_in_y) {
      return (CROPRECT_HIT_LEFT);
   }
   if (b_nr && b_in_y) {
      return (CROPRECT_HIT_RIGHT);
   }
   if (b_nt && b_in_x) {
      return (CROPRECT_HIT_TOP);
   }
   if (b_nb && b_in_x) {
      return (CROPRECT_HIT_BOTTOM);
   }
   if (d_x > d_l && d_x < d_r && d_y > d_t && d_y < d_b) {
      return (CROPRECT_HIT_INSIDE);
   }
   return (CROPRECT_HIT_NONE);
}

void
croprect_drag(CropRect *p_r, const CropRect *p_start, CropRectHit e_hit,
              gdouble d_dx, gdouble d_dy, gdouble d_aspect, gdouble d_w,
              gdouble d_h) {
   g_return_if_fail(p_r != NULL && p_start != NULL);
   *p_r = *p_start;
   switch (e_hit) {
   case CROPRECT_HIT_NONE:
      return;
   case CROPRECT_HIT_INSIDE:
      croprect_move(p_r, d_dx, d_dy, d_w, d_h);
      return;
   default:
      croprect_resize(p_r, e_hit, d_dx, d_dy, d_aspect, d_w, d_h);
      return;
   }
}

/* --- with the image ------------------------------------------------------- */

void
croprect_rotate_quarter(CropRect *p_r, gint i_dir, gdouble d_w, gdouble d_h) {
   g_return_if_fail(p_r != NULL);
   CropRect t_old = *p_r;
   if (i_dir > 0) {
      /* Clockwise: (x, y) -> (h - y, x), so the old bottom edge becomes
       * the new left edge (measured on the gegl:rotate probe in
       * enhancer.c's _append_transform comment). */
      p_r->d_x = d_h - (t_old.d_y + t_old.d_h);
      p_r->d_y = t_old.d_x;
   } else {
      /* Counter-clockwise: (x, y) -> (y, w - x). */
      p_r->d_x = t_old.d_y;
      p_r->d_y = d_w - (t_old.d_x + t_old.d_w);
   }
   p_r->d_w = t_old.d_h;
   p_r->d_h = t_old.d_w;
}

void
croprect_round(CropRect *p_r) {
   g_return_if_fail(p_r != NULL);
   gdouble d_l = round(p_r->d_x);
   gdouble d_t = round(p_r->d_y);
   gdouble d_r = round(p_r->d_x + p_r->d_w);
   gdouble d_b = round(p_r->d_y + p_r->d_h);
   /* Rounding both edges can collapse a sub-pixel rectangle; keep a pixel. */
   if (d_r - d_l < 1.0) {
      d_r = d_l + 1.0;
   }
   if (d_b - d_t < 1.0) {
      d_b = d_t + 1.0;
   }
   p_r->d_x = d_l;
   p_r->d_y = d_t;
   p_r->d_w = d_r - d_l;
   p_r->d_h = d_b - d_t;
}
