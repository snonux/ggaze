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

/* A non-finite coordinate can only come from a NaN/inf pointer delta or a
 * corrupt rectangle; it must never propagate (a NaN edge fails every
 * comparison, so CLAMP would hand it straight through). Fall back to d_alt. */
static gdouble
_finite_or(gdouble d_v, gdouble d_alt) {
   return (isfinite(d_v) ? d_v : d_alt);
}

void
croprect_clamp(CropRect *p_r, gdouble d_w, gdouble d_h) {
   g_return_if_fail(p_r != NULL);
   d_w      = MAX(_finite_or(d_w, 0.0), 0.0);
   d_h      = MAX(_finite_or(d_h, 0.0), 0.0);
   p_r->d_x = _finite_or(p_r->d_x, 0.0);
   p_r->d_y = _finite_or(p_r->d_y, 0.0);
   p_r->d_w = _finite_or(p_r->d_w, d_w);
   p_r->d_h = _finite_or(p_r->d_h, d_h);
   /* Intersect with the image FIRST -- every edge past the border is pulled
    * back to it and the opposite edge stays put -- THEN grow to the minimum
    * and slide back in if that overshot. Clamping x and then the width (the
    * old order) turned a left edge dragged past the border into a wider
    * rectangle: {50,50,20,20} with the left edge at -50 became {0,50,100,20},
    * its right edge jumping from 70 to 100. The floor is the image itself
    * when that is smaller than the minimum: a 6x3 fixture must still yield
    * a rectangle, just the whole image. */
   gdouble d_min_w = MIN(CROPRECT_MIN_SIZE, d_w);
   gdouble d_min_h = MIN(CROPRECT_MIN_SIZE, d_h);
   croprect_intersect(p_r, d_w, d_h);
   p_r->d_w = MAX(p_r->d_w, d_min_w);
   p_r->d_h = MAX(p_r->d_h, d_min_h);
   p_r->d_x = CLAMP(p_r->d_x, 0.0, d_w - p_r->d_w);
   p_r->d_y = CLAMP(p_r->d_y, 0.0, d_h - p_r->d_h);
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
    * past it). A NaN delta (a pointer event without coordinates) moves
    * nothing rather than poisoning the rectangle. */
   p_r->d_x += _finite_or(d_dx, 0.0);
   p_r->d_y += _finite_or(d_dy, 0.0);
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

/* The aspect-locked size (d_nw, d_nh) with the minimum enforced by growing
 * the OTHER side rather than by the clamp's per-axis floor, which would
 * break the lock (8x8 is not 2:1). Shared by the resize and set_aspect
 * paths. The image itself can still be the floor when it is smaller than
 * the minimum times the aspect; then the clamp wins and the lock does not
 * hold, which is the honest answer for a rectangle that cannot exist. */
static void
_aspect_min(gdouble *p_nw, gdouble *p_nh, gdouble d_aspect, gdouble d_w,
            gdouble d_h) {
   gdouble d_min_w = MIN(CROPRECT_MIN_SIZE, d_w);
   gdouble d_min_h = MIN(CROPRECT_MIN_SIZE, d_h);
   if (*p_nh < d_min_h) {
      *p_nh = d_min_h;
      *p_nw = d_min_h * d_aspect;
   }
   if (*p_nw < d_min_w) {
      *p_nw = d_min_w;
      *p_nh = d_min_w / d_aspect;
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
   _aspect_min(&d_nw, &d_nh, d_aspect, d_w, d_h);
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
   /* A NaN delta resizes by nothing; a NaN aspect (never produced by the
    * key table, but the value is a plain double) reads as "free" -- the
    * `> 0.0` test is false for NaN, which is the right answer here. */
   _move_edges(p_r, e_edge, _finite_or(d_dx, 0.0), _finite_or(d_dy, 0.0),
               MIN(CROPRECT_MIN_SIZE, d_w), MIN(CROPRECT_MIN_SIZE, d_h));
   if (d_aspect > 0.0) {
      _fit_aspect(p_r, e_edge, d_aspect, d_w, d_h);
   }
   croprect_clamp(p_r, d_w, d_h);
}

void
croprect_set_aspect(CropRect *p_r, gdouble d_aspect, gdouble d_w, gdouble d_h) {
   g_return_if_fail(p_r != NULL);
   if (!(d_aspect > 0.0) || !isfinite(d_aspect)) {
      return; /* free (0, negative, NaN): keep whatever shape the user made */
   }
   /* The largest rectangle of that shape inside the current one, centred
    * on it, so switching 3:2 -> 1:1 -> 3:2 stays put rather than drifting.
    * At the minimum size the other side grows instead (_aspect_min), so
    * the lock holds for a rectangle that was already as small as it gets. */
   gdouble d_nw = p_r->d_w;
   gdouble d_nh = d_nw / d_aspect;
   if (d_nh > p_r->d_h) {
      d_nh = p_r->d_h;
      d_nw = d_nh * d_aspect;
   }
   _aspect_min(&d_nw, &d_nh, d_aspect, d_w, d_h);
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

/* --- aspect locks ----------------------------------------------------------
 */

/* The fixed ratios and the names of every lock, indexed by CropRectAspect.
 * FREE and ORIGINAL carry 0: FREE locks nothing, and ORIGINAL's ratio is the
 * image's, read per call. */
static const struct {
   gdouble     d_ratio;
   const char *c_name;
} _ASPECTS[CROPRECT_ASPECT_COUNT] = {
   [CROPRECT_ASPECT_FREE]     = {0.0, "free"},
   [CROPRECT_ASPECT_1_1]      = {1.0, "1:1"},
   [CROPRECT_ASPECT_3_2]      = {3.0 / 2.0, "3:2"},
   [CROPRECT_ASPECT_4_3]      = {4.0 / 3.0, "4:3"},
   [CROPRECT_ASPECT_16_9]     = {16.0 / 9.0, "16:9"},
   [CROPRECT_ASPECT_ORIGINAL] = {0.0, "original"},
};

/* TRUE iff e_aspect names a lock (not COUNT, not garbage). */
static gboolean
_aspect_valid(CropRectAspect e_aspect) {
   return ((guint)e_aspect < (guint)CROPRECT_ASPECT_COUNT);
}

CropRectAspect
croprect_aspect_next(CropRectAspect e_aspect) {
   if (!_aspect_valid(e_aspect)) {
      return (CROPRECT_ASPECT_FREE);
   }
   return (
      (CropRectAspect)(((guint)e_aspect + 1u) % (guint)CROPRECT_ASPECT_COUNT));
}

gdouble
croprect_aspect_ratio(CropRectAspect e_aspect, gdouble d_w, gdouble d_h) {
   if (!_aspect_valid(e_aspect)) {
      return (0.0);
   }
   if (e_aspect == CROPRECT_ASPECT_ORIGINAL) {
      if (!isfinite(d_w) || !isfinite(d_h) || d_w <= 0.0 || d_h <= 0.0) {
         return (0.0); /* no shape to keep: free */
      }
      return (d_w / d_h);
   }
   return (_ASPECTS[e_aspect].d_ratio);
}

const char *
croprect_aspect_name(CropRectAspect e_aspect) {
   return (_aspect_valid(e_aspect) ? _ASPECTS[e_aspect].c_name : "free");
}
