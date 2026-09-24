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
 * Touch (task zb2): a pinch zooms around its midpoint and pans with it, a
 * horizontal swipe asks for the next / previous image ("navigate", the
 * signal the wheel's navigate mode uses) and a two-finger tap asks for the
 * info card ("toggle-info"). The viewer only reports intent; the window gates
 * the navigation and owns the info action. See "touch gestures" below.
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
   GGAZE_VIEWER_DRAG_END,
   /* The drag was taken away rather than finished: a second finger landed
    * and made it a pinch (zb2). Not a gesture the user completed, so a
    * tool must not act on it as it would on END -- the straighten tool
    * drops its line (an END would level by whatever jitter the finger
    * made); the crop tool lets go and keeps the rectangle as the last
    * UPDATE left it (what the user saw while dragging). Its coordinates
    * are the last ones the drag reported. */
   GGAZE_VIEWER_DRAG_CANCEL,
   /* After a CANCEL: the pinch that took the drag over ended as a
    * two-finger tap, so the drag was the tap's first finger and nothing
    * the user meant -- put back what it changed since its BEGIN (the crop
    * tool restores the rectangle it grabbed; the straighten tool, whose
    * CANCEL already dropped the line, has nothing to undo). Sent at most
    * once, only to the overlay installed when the tap ends, which may not
    * be the one that got the BEGIN (then it has nothing of that drag to
    * undo and ignores it). Its coordinates are the CANCEL's. */
   GGAZE_VIEWER_DRAG_REVERT
} GgazeViewerDragPhase;

/* Paint on top of the image. p_geom is the geometry the image was just drawn
 * with. */
typedef void (*GgazeViewerOverlayFn)(GtkSnapshot           *p_snap,
                                     const GgazeViewerGeom *p_geom,
                                     gpointer               p_data);

/* A pointer drag, in ABSOLUTE widget coordinates (begin: where it started;
 * update/end/cancel/revert: where the pointer is now, or last was). A
 * BEGIN is followed by at most one END or CANCEL, never both (neither when
 * the overlay is removed mid-drag); a CANCEL may be followed by one
 * REVERT. A drag is also CANCELled when a new texture is set or the viewer
 * is unmapped mid-drag (the rest of that drag then reaches nobody). While
 * an overlay is installed the drag gesture feeds this instead of panning.
 * The phases arrive per drag, not per overlay: an overlay installed
 * mid-drag, in place of the one that got the BEGIN (a tool switched by
 * key while the finger is down), receives that drag's UPDATE / END /
 * CANCEL / REVERT without ever having seen its BEGIN, so a callback must
 * tolerate that -- both tools ignore a phase with no grab or line of
 * their own (tool-ctrl.c). */
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
 * which do nothing but read the gesture's points / velocity / event type
 * and call these. Public so tests can drive a gesture with chosen points
 * (GTK4 cannot synthesise touch events); nothing else in the app calls
 * them. The rules and
 * thresholds are gesture-math.h's.
 *
 * Pinch: begin with the midpoint of the two touches and whether it is a
 * touchpad pinch, update with GtkGestureZoom's scale (finger distance now
 * / at the start) and the current midpoint, end when a finger lifts. The
 * view zooms to (zoom at begin) x scale and moves with the midpoint: the
 * image pixel under the fingers stays under them, so two fingers moved at
 * a constant distance pan. Zoom goes through the same clamp and
 * non-finite guard as wheel zoom; an update with a non-finite midpoint
 * changes nothing.
 * Begin also takes away a one-finger drag in progress (a tool gets a
 * CANCEL where the finger was). A touch that was short and still
 * (gesture_math_is_two_finger_tap) and not a touchpad pinch is a
 * two-finger tap instead -- when a one-finger drag came first, its time
 * and movement count, from that drag's start: the view goes back to what
 * it was before the first finger went down and "toggle-info" is emitted;
 * end returns TRUE then, and a tool whose drag the pinch took over gets a
 * DRAG_REVERT after its CANCEL. A pinch that began over a fitted picture
 * keeps it fitted while its scale stays within a tap's wobble of 1
 * (GESTURE_TAP_MAX_SCALE_DEV): a two-finger pan reports such scales, and
 * a fitted picture has nothing to pan. update / end without a begin do
 * nothing, and a set_texture or an unmap in between ends the pinch (no
 * tap, no zoom) and any drag (a tool gets a CANCEL). */
void ggaze_viewer_pinch_begin(GgazeViewer *p_viewer, gdouble d_cx, gdouble d_cy,
                              gboolean b_touchpad);
void ggaze_viewer_pinch_update(GgazeViewer *p_viewer, gdouble d_scale,
                               gdouble d_cx, gdouble d_cy);
gboolean ggaze_viewer_pinch_end(GgazeViewer *p_viewer);

/* Swipe path: where the one finger went down (b_down) and where it is now,
 * in widget px. The GtkGestureSwipe "begin" / "update" handlers call this
 * with the gesture's point; its "swipe" handler then judges the path with
 * ggaze_viewer_swipe below -- unless a pinch began since the finger went
 * down (the finger was half of it) or the swipe was spoiled since
 * (ggaze_viewer_spoil_swipe: a slideshow step). */
void ggaze_viewer_swipe_track(GgazeViewer *p_viewer, gboolean b_down,
                              gdouble d_x, gdouble d_y);

/* Spoil the swipe in progress, if any: when its finger lifts it navigates
 * nowhere; the next finger down starts a fresh one. The window calls this
 * as the slideshow advances (window.c _slideshow_tick): a flick that began
 * over one picture must not turn the page again from the picture the
 * slideshow put up under it, which would skip an image. A set_texture does
 * not do this itself (unlike a pinch or drag, _gestures_reset): the new
 * texture of a slideshow step lands only after its decode, so a flick
 * ending in between would still get through; and textures change for
 * reasons that are no page turn (an enhance preview render, a reload),
 * which must not swallow a real swipe. */
void ggaze_viewer_spoil_swipe(GgazeViewer *p_viewer);

/* Swipe: a finished one-finger touch drag that moved (d_dx, d_dy) px and
 * ended at (d_vx, d_vy) px/s. Emits "navigate" (+1 for a leftward flick =
 * next, -1 rightward = previous) and returns that direction, or returns 0
 * and emits nothing: not a swipe (gesture_math_swipe_direction), no
 * texture, a tool overlay installed (the crop / straighten tool owns the
 * drag), or the picture zoomed wider than the widget (the finger was
 * panning). A swipe during which a pinch began, or that was spoiled, never
 * gets here. */
gint ggaze_viewer_swipe(GgazeViewer *p_viewer, gdouble d_dx, gdouble d_dy,
                        gdouble d_vx, gdouble d_vy);

G_END_DECLS

#endif /* GGAZE_VIEWER_H */
