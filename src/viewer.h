#ifndef GGAZE_VIEWER_H
#define GGAZE_VIEWER_H

/*:*
 * ggaze — large single-image viewer
 *
 * GgazeViewer : GtkWidget is the custom large-view canvas (decision #31, not
 * GtkPicture). It owns a GdkTexture plus zoom/pan/fit state and draws via GTK4
 * render nodes. Zoom is cursor-centered (mouse/pinch) or window-centered
 * (keys); panning clamps so the image can't drift off-screen. See
 * docs/ui-and-interactions.md "Zoom behavior" and docs/architecture.md
 * "Responsibilities / viewer".
 *
 * It also HOSTS a tool overlay (the crop rectangle, the straighten grid):
 * one draw callback painted after the image with the image's on-screen
 * geometry, and one drag callback that takes over the pointer drag from
 * panning while it is set. The viewer knows nothing about what is drawn;
 * tool-ctrl.c does, in image pixels, and this is where those become widget
 * pixels.
 *
 * And it PLAYS an animated GIF/WebP (M5, task yb2): the texture it is
 * given is that file's first frame with the other frames attached
 * (loader/animation.h), and the viewer alone steps through them -- on the
 * frame clock while mapped, at each frame's own delay, as many times as
 * the file says, restarting from the first frame whenever the texture is
 * set, holding it while a tool asks
 * (ggaze_viewer_hold_first_frame). Everything else in the app, and every
 * other accessor here, keeps seeing the first frame: zoom, pan and the
 * overlay geometry are the canvas's; ggaze_viewer_get_texture() is what
 * the cache, the histogram and hold-Space compare against; only
 * ggaze_viewer_get_frame() says what is on screen right now.
 *
 * Touch (task zb2): a pinch zooms around its midpoint, a horizontal swipe
 * asks for the next / previous image ("navigate", the signal the wheel's
 * navigate mode uses) and a two-finger tap asks for the info card
 * ("toggle-info"). The viewer only reports intent; the window gates the
 * navigation and owns the info action. See "touch gestures" below.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include "ggaze-enums.h"

G_BEGIN_DECLS

#define GGAZE_TYPE_VIEWER (ggaze_viewer_get_type())
#define GGAZE_VIEWER_PAN_STEP 24.0
G_DECLARE_FINAL_TYPE(GgazeViewer, ggaze_viewer, GGAZE, VIEWER, GtkWidget)

GtkWidget *ggaze_viewer_new(void);

/* Take p_texture (refs it; the caller still owns its own ref and should unref
 * when done). Resets to fit-to-window, clears pan. NULL clears the display.
 * A texture with an animation attached (animation_lookup) starts playing
 * from its first frame, if the widget is mapped; whatever played before
 * stops. */
void ggaze_viewer_set_texture(GgazeViewer *p_viewer, GdkTexture *p_texture);
GdkTexture *
ggaze_viewer_get_texture(GgazeViewer *p_viewer); /* (transfer none) */

/* The texture actually being drawn: the current animation frame while one
 * plays, else the texture itself (transfer none; NULL with no texture).
 * For tests and for anyone who needs the pixels on screen rather than the
 * file's picture. */
GdkTexture *ggaze_viewer_get_frame(GgazeViewer *p_viewer);

/* TRUE while a next frame is scheduled (a tick callback on the frame
 * clock, or the timeout that puts it back before the next frame): an
 * animation is attached, the widget is mapped, no hold is on and the
 * animation has not ended -- on a frame that holds for ever, or on its
 * last frame once the file's plays are done. */
gboolean ggaze_viewer_is_animating(GgazeViewer *p_viewer);

/* How many times the animation tick callback has run in this widget's
 * life (every run counted, whichever animation it played). A test seam:
 * nothing in the app reads it; a slow animation must keep the frame clock
 * ticking only near its frame changes, and a fast one's ticks are the
 * clock's measured rate (tests/test_viewer.c, yb2). */
guint ggaze_viewer_get_tick_count(GgazeViewer *p_viewer);

/* Hold (TRUE) or release (FALSE) an animation on its first frame. The
 * crop / straighten tools hold while they are up: they lay out and apply
 * against the first frame -- the texture everything but the viewer sees
 * -- so the picture under the rectangle or the horizon line must be that
 * frame, not whichever one is playing. The hold outlives set_texture (a
 * render landing under the tool) and releasing restarts playback from the
 * first frame if the widget is mapped. A still is unaffected either way. */
void ggaze_viewer_hold_first_frame(GgazeViewer *p_viewer, gboolean b_hold);

/* Zoom + pan actions (also reachable via the on-widget controllers). */
void ggaze_viewer_zoom_in(GgazeViewer *p_viewer);
void ggaze_viewer_zoom_out(GgazeViewer *p_viewer);

/* The scale actually being drawn: the fit-to-window ratio while fitting, else
 * the explicit zoom factor (1.0 = 100%). Returns 0.0 with no texture.
 *
 * Exists because zoom state was otherwise unobservable from outside the
 * widget, which is why the zoom path carried no tests and hx0's regression
 * went unnoticed. Prefer this over inferring zoom from a rendered snapshot. */
gdouble ggaze_viewer_get_scale(GgazeViewer *p_viewer);

/* Current pan offset from centred, in widget pixels. Companion to
 * ggaze_viewer_pan(); either out-parameter may be NULL.
 *
 * The scale alone is NOT enough to tell whether the widget is healthy: it is
 * derived from the fit ratio or d_zoom and stays perfectly finite while the
 * pan is NaN -- the exact state in which hx0's blank view was drawn. Assert on
 * this too when checking that the image is still displayable. */
void ggaze_viewer_get_pan(GgazeViewer *p_viewer, gdouble *p_x, gdouble *p_y);
void ggaze_viewer_toggle_fit_100(GgazeViewer *p_viewer);
void ggaze_viewer_pan(GgazeViewer *p_viewer, gdouble d_dx, gdouble d_dy);

/* --- tool overlay hosting --------------------------------------------------
 */

/* Where the image is on screen: its top-left corner in widget pixels and the
 * scale that turns image pixels into widget pixels (image * d_scale + d_x).
 * i_img_w/i_img_h are the texture's size, so a caller can tell whether the
 * texture on screen is the one its overlay was laid out on. */
typedef struct {
   gdouble d_x;
   gdouble d_y;
   gdouble d_scale;
   gint    i_img_w;
   gint    i_img_h;
} GgazeViewerGeom;

/* The current geometry; FALSE (p_out untouched) with no texture. */
gboolean ggaze_viewer_get_geometry(GgazeViewer     *p_viewer,
                                   GgazeViewerGeom *p_out);

typedef enum {
   GGAZE_VIEWER_DRAG_BEGIN,
   GGAZE_VIEWER_DRAG_UPDATE,
   GGAZE_VIEWER_DRAG_END
} GgazeViewerDragPhase;

/* Paint on top of the image. p_geom is the geometry the image was just drawn
 * with. */
typedef void (*GgazeViewerOverlayFn)(GtkSnapshot           *p_snap,
                                     const GgazeViewerGeom *p_geom,
                                     gpointer               p_data);

/* A pointer drag, in ABSOLUTE widget coordinates (begin: where it started;
 * update/end: where the pointer is now). While an overlay is installed the
 * drag gesture feeds this instead of panning. */
typedef void (*GgazeViewerDragFn)(GgazeViewerDragPhase e_phase, gdouble d_x,
                                  gdouble d_y, gpointer p_data);

/* Install (or, with both callbacks NULL, remove) the tool overlay. p_data is
 * borrowed and passed to both. Queues a redraw. */
void ggaze_viewer_set_overlay(GgazeViewer         *p_viewer,
                              GgazeViewerOverlayFn fn_draw,
                              GgazeViewerDragFn fn_drag, gpointer p_data);

/* Configure the background colour drawn behind the image and what the scroll
 * wheel does (applied from GSettings by the window). Defaults: dark / zoom. */
void ggaze_viewer_set_background(GgazeViewer *p_viewer, GgazeBackground e_bg);
void ggaze_viewer_set_scroll_behavior(GgazeViewer        *p_viewer,
                                      GgazeScrollBehavior e_scroll);

/* --- touch gestures (zb2) ---------------------------------------------------
 *
 * The bodies of the viewer's GtkGestureZoom and GtkGestureSwipe handlers,
 * which do nothing but read the gesture's points / velocity and call these.
 * Public so tests can drive a gesture with chosen points (GTK4 cannot
 * synthesise touch events); nothing in the app calls them. The rules and
 * thresholds are gesture-math.h's.
 *
 * Pinch: begin with the midpoint of the two touches, update with
 * GtkGestureZoom's scale (finger distance now / at the start) and the
 * current midpoint, end when a finger lifts. The view zooms to (zoom at
 * begin) x scale around the midpoint, through the same clamp and
 * non-finite guard as wheel zoom; begin also ends a one-finger drag in
 * progress (a tool gets its END where the finger was). A begin-end that
 * was short and still (gesture_math_is_two_finger_tap) and not a touchpad
 * pinch is a two-finger tap instead: the view goes back to what it was
 * before the begin and "toggle-info" is emitted; end returns TRUE then.
 * update / end without a begin do nothing. */
void     ggaze_viewer_pinch_begin(GgazeViewer *p_viewer, gdouble d_cx,
                                  gdouble d_cy);
void     ggaze_viewer_pinch_update(GgazeViewer *p_viewer, gdouble d_scale,
                                   gdouble d_cx, gdouble d_cy);
gboolean ggaze_viewer_pinch_end(GgazeViewer *p_viewer);

/* Swipe: a finished one-finger touch drag that moved (d_dx, d_dy) px and
 * ended at (d_vx, d_vy) px/s. Emits "navigate" (+1 for a leftward flick =
 * next, -1 rightward = previous) and returns that direction, or returns 0
 * and emits nothing: not a swipe (gesture_math_swipe_direction), no
 * texture, a tool overlay installed (the crop / straighten tool owns the
 * drag), or the picture zoomed wider than the widget (the finger was
 * panning). A swipe during which a pinch began never gets here. */
gint ggaze_viewer_swipe(GgazeViewer *p_viewer, gdouble d_dx, gdouble d_dy,
                        gdouble d_vx, gdouble d_vy);

G_END_DECLS

#endif /* GGAZE_VIEWER_H */
