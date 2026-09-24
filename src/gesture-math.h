#ifndef GGAZE_GESTURE_MATH_H
#define GGAZE_GESTURE_MATH_H

/*:*
 * ggaze — zoom and touch-gesture math (plain C, no GTK)
 *
 * The arithmetic behind the large view's zoom and its touch gestures (task
 * zb2), kept out of viewer.c so every rule is a pure function of its
 * arguments and unit-tested without a display (tests/test_gesture_math.c):
 *
 *   - zoom about a point: the one "new zoom around widget point (cx, cy)"
 *     rule that the wheel, the keys and a pinch all go through, with the
 *     2 %..6400 % clamp (raised to the fit ratio, jx0) and the non-finite
 *     guard (hx0) in exactly one place;
 *   - pinch: the zoom a pinch asks for, from the zoom it began at and
 *     GtkGestureZoom's scale factor, and the fit detent's rebased scale;
 *   - swipe: whether a one-finger touch drag was a deliberate horizontal
 *     flick, and which way;
 *   - two-finger tap: whether a two-touch gesture was a short tap rather
 *     than a pinch.
 *
 * GTK stays in viewer.c: it reads the gesture's points and velocities,
 * calls these, and acts on the answer. The thresholds are logical pixels
 * (GTK's widget coordinates), so they mean the same physical distance on a
 * HiDPI screen.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <glib.h>

G_BEGIN_DECLS

/* Zoom limits (1.0 = 100 %): see gesture_math_clamp_zoom for how the upper
 * one rises to the fit ratio. docs/ui-and-interactions.md "Zoom behavior". */
#define GGAZE_ZOOM_MIN 0.02
#define GGAZE_ZOOM_MAX 64.0

/* A swipe must travel at least this far horizontally (logical px) ... */
#define GESTURE_SWIPE_MIN_DISTANCE 80.0
/* ... end at least this fast horizontally (px/s, GtkGestureSwipe's
 * velocity over the last few events) ... */
#define GESTURE_SWIPE_MIN_VELOCITY 300.0
/* ... and be mostly horizontal: |dy| <= this * |dx| (about 27 degrees). */
#define GESTURE_SWIPE_MAX_SLOPE 0.5

/* A two-finger tap lasts at most this long (both fingers down to the first
 * one up) ... */
#define GESTURE_TAP_MAX_US (250 * G_TIME_SPAN_MILLISECOND)
/* ... its midpoint moves at most this far (logical px) ... */
#define GESTURE_TAP_MAX_MOVE 20.0
/* ... and the finger distance changes by at most this factor (|scale - 1|). */
#define GESTURE_TAP_MAX_SCALE_DEV 0.1

/* The view a zoom starts from: the widget and texture sizes, the scale and
 * top-left corner the image is drawn at now (already clamped, as the viewer
 * draws it), and the fit-to-window ratio. */
typedef struct {
   gint    i_w;     /* widget width, px */
   gint    i_h;     /* widget height, px */
   gint    i_tex_w; /* texture width, image px */
   gint    i_tex_h; /* texture height, image px */
   gdouble d_scale; /* current scale (> 0 when drawable) */
   gdouble d_x;     /* current top-left, widget px */
   gdouble d_y;
   gdouble d_fit; /* fit-to-window ratio */
} GestureView;

/* Clamp d_zoom to [GGAZE_ZOOM_MIN, MAX(GGAZE_ZOOM_MAX, d_fit)] (jx0: a small
 * image in a big window fits above 6400 %, and clamping it to the bare
 * ceiling made zoom-in SHRINK it). d_zoom must be finite. */
gdouble gesture_math_clamp_zoom(gdouble d_zoom, gdouble d_fit);

/* Zoom to d_zoom (clamped) around widget point (d_cx, d_cy): the image pixel
 * under that point before stays under it after. Writes the clamped zoom and
 * the pan (offset from centred, widget px) that achieves it. FALSE, with the
 * outputs untouched, when any input is non-finite (hx0: a NaN reaching the
 * viewer's pan made the image vanish for good) -- the caller keeps its last
 * good geometry. */
gboolean gesture_math_zoom_about(const GestureView *p_view, gdouble d_cx,
                                 gdouble d_cy, gdouble d_zoom, gdouble *p_zoom,
                                 gdouble *p_pan_x, gdouble *p_pan_y);

/* The zoom a pinch asks for: the zoom it began at times GtkGestureZoom's
 * scale (the finger distance now over the distance at the start). FALSE,
 * p_zoom untouched, for a non-finite or non-positive scale or start zoom
 * (two touches on one spot give scale 0 or NaN). Not clamped here:
 * gesture_math_zoom_about does that. */
gboolean gesture_math_pinch_zoom(gdouble d_start_zoom, gdouble d_scale,
                                 gdouble *p_zoom);

/* The fit detent (zb2): a pinch that began over a FITTED picture holds it
 * at fit while |d_scale - 1| <= GESTURE_TAP_MAX_SCALE_DEV (a two-finger
 * pan reports such scales). Outside that band it zooms by the scale
 * measured from the band edge it crossed -- d_scale / (1 + band) out,
 * d_scale / (1 - band) in -- so the first zoom past the edge is the fit
 * zoom itself and the zoom is continuous, rather than jumping straight to
 * 1.1x fit. Returns that scale for the caller to hand to
 * gesture_math_pinch_zoom: 1.0 inside the band, and d_scale unchanged
 * when it is non-finite (pinch_zoom refuses it, and a zero or negative
 * scale stays zero or negative, refused alike). */
gdouble gesture_math_detent_scale(gdouble d_scale);

/* Classify a finished one-finger touch drag that moved (d_dx, d_dy) px and
 * ended at velocity (d_vx, d_vy) px/s: +1 for a leftward flick (the next
 * image slides in from the right, as with a page), -1 for a rightward one
 * (previous), 0 for anything else -- too short, too slow (a zero-duration
 * swipe has no velocity), mostly vertical, ending against its own
 * direction, or non-finite. */
gint gesture_math_swipe_direction(gdouble d_dx, gdouble d_dy, gdouble d_vx,
                                  gdouble d_vy);

/* TRUE when a two-touch gesture that lasted i_duration_us, whose midpoint
 * strayed at most d_max_move px and whose scale strayed at most
 * d_max_scale_dev from 1, was a tap. Negative or non-finite inputs are
 * not a tap. */
gboolean gesture_math_is_two_finger_tap(gint64  i_duration_us,
                                        gdouble d_max_move,
                                        gdouble d_max_scale_dev);

G_END_DECLS

#endif /* GGAZE_GESTURE_MATH_H */
