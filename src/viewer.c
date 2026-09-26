/*:*
 * ggaze — large single-image viewer
 *
 * Custom GtkWidget (decision #31). Draws the GdkTexture scaled into the widget
 * with letterboxing, zoom (fit / 100% / in / out), cursor-centered zoom,
 * drag-to-pan with clamping, and a dark background. M1: synchronous single
 * image. M9 added the tool overlay hook (see viewer.h); hold-Space compare
 * lives in enhance-ctrl.c and only swaps the texture shown here.
 *
 * Animation (M5, task yb2): the texture handed to ggaze_viewer_set_texture
 * is always a still -- the first frame -- and may carry the animation's
 * other frames (animation_lookup, loader/animation.h), decoded to textures
 * by the loader's worker. Playback is this widget's alone and only picks
 * which of those immutable textures to draw: a tick callback on the
 * frame clock hands the frame time to animation_playback_advance (the
 * schedule, the stall resync and the file's loop count, plain C) and
 * redraws when the frame changed, so the per-frame main-thread cost is a
 * comparison and a redraw -- no decode, no composition, no pixel copy.
 * The tick is only on the clock near a due time: between the frames of a
 * slow animation it is dropped and a timeout re-adds it
 * GGAZE_ANIM_TICK_LEAD_MS before the next one (a tick callback makes GDK
 * draw every vblank). Plus, the
 * first time each frame is drawn, the renderer's upload of it (at most
 * GGAZE_ANIM_MAX_CANVAS_PIXELS x 4 bytes; later loops draw the uploaded
 * texture again). Frames are drawn at the first frame's geometry (frames
 * are the canvas size), so zoom, pan, the fit ratio, the tool overlay's
 * geometry and ggaze_viewer_get_texture() all keep describing the first
 * frame.
 *
 * When it runs: only while the widget is mapped (map/unmap), so the grid
 * page or an unpresented window plays nothing; and, because the ticks come
 * from the toplevel's frame clock rather than a g_timeout, only while GDK
 * drives that clock -- it stops ticking for a surface the compositor
 * reports as not being presented (a minimized window; on Wayland also a
 * fully covered one that gets no frame callbacks), and playback pauses
 * there with it. It ends on the last frame once the file's plays are done
 * (a GIF without a loop extension plays once). It restarts from the first
 * frame -- plays counted afresh -- on every set_texture, remap and hold
 * release, the same way zoom resets; and it holds the
 * first frame while a crop / straighten tool is up (hold_first_frame),
 * since the tool frames and applies against the first frame.
 *
 * Touch gestures (task zb2): GtkGestureZoom pinches (touchscreen, and a
 * touchpad pinch too) and GtkGestureSwipe swipes (touch only). The math --
 * zoom about a point, the pinch's zoom, what counts as a swipe or a
 * two-finger tap -- is gesture-math.c's, so the wheel, the keys and a
 * pinch share one zoom rule with one clamp and one NaN guard. Decisions:
 * a swipe is refused while the picture is zoomed wider than the widget
 * (the same finger pans it) and while a tool overlay is installed (the
 * tool owns every drag); a pinch ends the one-finger drag it grew out of
 * (a tool gets a CANCEL, not an END: the drag was not finished); a pinch
 * pans with its midpoint while it zooms, so two fingers moved together
 * drag the picture along -- except over a fitted picture while the scale
 * stays within a tap's wobble of 1 (a two-finger pan reports ~1.01): that
 * picture stays fitted and holds still, so `0`, a resize refit and a swipe
 * keep working (re-entering that band keeps the position the fingers
 * gave it: fit mode does not re-centre); a two-finger tap leaves the view
 * as it was before its first finger went down, and undoes what that
 * finger did to a tool (DRAG_REVERT, viewer.h). None of the gestures
 * claims its sequences, so the drag gesture keeps working for one finger
 * and the mouse path is untouched.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "viewer.h"

#include <math.h>

#include <glib.h>
#include <graphene.h>

#include "gesture-math.h"
#include "loader/animation.h"
#include "logical-size.h"
#include "settings.h"

/* One wheel notch / key press. The limits (GGAZE_ZOOM_MIN/MAX) live in
 * gesture-math.h with the clamp that applies them. */
#define GGAZE_ZOOM_FACTOR 1.25

/* The zoom / pan state a gesture may have to put back: the view before a
 * two-finger tap's first finger went down, the view a pinch began at. */
typedef struct {
   gboolean b_fit;
   gdouble  d_zoom;
   gdouble  d_pan_x;
   gdouble  d_pan_y;
} ViewState;

struct _GgazeViewer {
   GtkWidget           parent_instance;
   GdkTexture         *p_texture;
   gboolean            b_fit;   /* TRUE = fit-to-window; FALSE = use d_zoom */
   gdouble             d_zoom;  /* 1.0 = 100% (used when !b_fit) */
   gdouble             d_pan_x; /* offset from centred, in widget px */
   gdouble             d_pan_y;
   gdouble             d_drag_start_pan_x;
   gdouble             d_drag_start_pan_y;
   GgazeBackground     e_bg;           /* configurable viewer background */
   GgazeScrollBehavior e_scroll;       /* what the scroll wheel does */
   gdouble             d_drag_start_x; /* where the current drag began, in
                                        * widget px (for the overlay's
                                        * absolute coordinates) */
   gdouble  d_drag_start_y;
   gboolean b_drag_to_overlay; /* the drag in progress began with a tool
                                * overlay installed and belongs to it */
   /* The tool overlay (viewer.h): NULL callbacks = none installed. */
   GgazeViewerOverlayFn fn_overlay;
   GgazeViewerDragFn    fn_drag;
   gpointer             p_overlay_data;
   /* Animation playback (top-of-file comment). p_anim is the one attached
    * to p_texture (borrowed: it lives as long as p_texture, which this
    * widget holds), or NULL for a still. */
   const GgazeAnimation *p_anim;
   GgazeAnimPlayback     st_play; /* frame drawn (0 is p_texture), plays
                                   * done, when the next one is due */
   guint    u_tick_id;            /* the tick callback, near a due time */
   guint    u_wake_id;            /* the timeout that re-adds it */
   guint    u_ticks;              /* tick callbacks run (test seam) */
   gboolean b_hold;               /* a tool holds the first frame */
   /* Pointer drag bookkeeping for the touch gestures (zb2): a pinch ends
    * the one-finger drag it grew out of (see _drag_abandon). */
   GtkGesture *p_drag_gesture; /* borrowed: the widget owns it */
   gboolean    b_dragging;     /* between drag-begin and drag-end */
   gboolean    b_drag_dead;    /* abandoned by a pinch: ignore the rest */
   gdouble     d_drag_dx;      /* the drag's last offset from its start */
   gdouble     d_drag_dy;
   gdouble     d_drag_max_move; /* how far it strayed (a tap's stillness) */
   gint64      i_drag_start_us; /* when it began (a tap's duration) */
   /* Pinch / two-finger tap (zb2, top-of-file comment). */
   gboolean b_pinching;      /* two touches down */
   gboolean b_pinch_can_tap; /* not a touchpad pinch, not cancelled */
   gdouble  d_pinch_zoom0;   /* the scale drawn when the pinch began */
   gdouble  d_pinch_cx0;     /* its midpoint then, widget px */
   gdouble  d_pinch_cy0;
   gdouble  d_pinch_last_cx; /* the midpoint at the last update: the pan
                              * follows its movement */
   gdouble  d_pinch_last_cy;
   gdouble  d_pinch_max_move; /* how far the midpoint strayed */
   gdouble  d_pinch_max_dev;  /* how far the scale strayed from 1 */
   gint64   i_pinch_start_us;
   gboolean b_pinch_from_tool; /* it took a tool's drag over: a tap sends
                                * the tool a DRAG_REVERT */
   ViewState t_pinch0; /* the view the pinch began at (the fit detent) */
   ViewState t_pre;    /* the view before the tap's first finger went down
                        * (_snapshot_view), put back after a tap */
   /* Swipe (zb2): where the finger went down and where it is now. */
   gboolean b_swipe_spoiled; /* a pinch or a slideshow step happened
                              * during this swipe */
   gdouble d_swipe_x0;
   gdouble d_swipe_y0;
   gdouble d_swipe_x;
   gdouble d_swipe_y;
};

G_DEFINE_TYPE(GgazeViewer, ggaze_viewer, GTK_TYPE_WIDGET)

static guint u_navigate_sig    = 0;
static guint u_toggle_info_sig = 0;

/* --- animation playback -------------------------------------------------- */

/* How long before a frame is due the tick callback is put back on the
 * frame clock. A tick callback makes GDK produce a frame every vblank
 * whether or not anything changed, so between two frames of a slow
 * animation (a 1 s delay is common) it is taken off and a timeout brings
 * it back this long before the due time; the frame itself still changes
 * on a tick, at frame-clock time, and a clock that stopped (a minimized
 * window) still pauses the playback, since the re-added tick does not run
 * until the clock does. Two to three vblanks at 60 Hz: enough slack for
 * a timeout that fires a little late. Delays at or under it keep the tick
 * the whole time -- at those rates it runs nearly every vblank anyway. */
#define GGAZE_ANIM_TICK_LEAD_MS 40

static gboolean _anim_tick_cb(GtkWidget *p_widget, GdkFrameClock *p_clock,
                              gpointer p_data);

/* Stop playing: no tick callback, no pending wake-up. The frame on screen
 * stays until the next start or set_texture replaces it, so an unmap does
 * not flash the first frame back. */
static void
_anim_stop(GgazeViewer *p_v) {
   if (p_v->u_tick_id != 0) {
      gtk_widget_remove_tick_callback(GTK_WIDGET(p_v), p_v->u_tick_id);
      p_v->u_tick_id = 0;
   }
   g_clear_handle_id(&p_v->u_wake_id, g_source_remove);
}

static void
_anim_add_tick(GgazeViewer *p_v) {
   p_v->u_tick_id =
      gtk_widget_add_tick_callback(GTK_WIDGET(p_v), _anim_tick_cb, NULL, NULL);
}

/* The wake-up timeout: the next frame is close, put the tick back. */
static gboolean
_anim_wake_cb(gpointer p_data) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_data);
   p_v->u_wake_id   = 0;
   _anim_add_tick(p_v);
   return (G_SOURCE_REMOVE);
}

/* Called from a tick at i_now: TRUE to keep ticking, FALSE when the tick
 * is to be dropped -- the animation ended, or the next frame is far
 * enough off that a timeout re-adds the tick shortly before it
 * (GGAZE_ANIM_TICK_LEAD_MS). */
static gboolean
_anim_keep_ticking(GgazeViewer *p_v, gint64 i_now) {
   if (p_v->st_play.b_ended) {
      return (FALSE);
   }
   gint64 i_wait_ms = (p_v->st_play.i_due_us - i_now) / 1000;
   if (i_wait_ms <= GGAZE_ANIM_TICK_LEAD_MS) {
      return (TRUE);
   }
   p_v->u_wake_id = g_timeout_add((guint)(i_wait_ms - GGAZE_ANIM_TICK_LEAD_MS),
                                  _anim_wake_cb, p_v);
   return (FALSE);
}

/* Once per frame of the frame clock while a frame is near: move the
 * playback to the frame time (animation_playback_advance: the schedule,
 * the stall resync and the loop count live there, unit-tested) and redraw
 * when the frame changed. */
static gboolean
_anim_tick_cb(GtkWidget *p_widget, GdkFrameClock *p_clock, gpointer p_data) {
   (void)p_data;
   GgazeViewer *p_v   = GGAZE_VIEWER(p_widget);
   gint64       i_now = gdk_frame_clock_get_frame_time(p_clock);
   p_v->u_ticks++;
   if (animation_playback_advance(p_v->p_anim, &p_v->st_play, i_now)) {
      gtk_widget_queue_draw(p_widget);
   }
   if (_anim_keep_ticking(p_v, i_now)) {
      return (G_SOURCE_CONTINUE);
   }
   p_v->u_tick_id = 0; /* GTK drops the callback on REMOVE */
   return (G_SOURCE_REMOVE);
}

/* Start from the first frame if there is an animation to play, nothing
 * plays yet, no tool holds the first frame and the widget can be seen.
 * An animation whose first frame already holds for ever never starts. */
static void
_anim_start(GgazeViewer *p_v) {
   if (p_v->p_anim == NULL || p_v->u_tick_id != 0 || p_v->u_wake_id != 0 ||
       p_v->b_hold || !gtk_widget_get_mapped(GTK_WIDGET(p_v))) {
      return;
   }
   animation_playback_reset(&p_v->st_play);
   gtk_widget_queue_draw(GTK_WIDGET(p_v));
   if (animation_get_delay_ms(p_v->p_anim, 0) < 0) {
      return;
   }
   _anim_add_tick(p_v); /* the first tick anchors the schedule */
}

/* The texture drawn now: the current frame while an animation is on
 * frame 1 or later, else the texture itself. */
static GdkTexture *
_drawn_texture(GgazeViewer *p_v) {
   if (p_v->p_anim != NULL && p_v->st_play.u_frame != 0) {
      return (animation_get_frame(p_v->p_anim, p_v->st_play.u_frame));
   }
   return (p_v->p_texture);
}

/* --- geometry --------------------------------------------------------------
 */

/* The IMAGE's size, which every piece of geometry here is in: the size
 * the texture stands for (logical-size.h), not its pixel count. The two
 * differ for the scaled-down enhance preview (8l2): a 9248x6936 photo's
 * preview texture holds ~1500x1100 pixels, and fit, 100 % zoom, the pan
 * clamp and the tool overlay's geometry must all still be the photo's --
 * the snapshot simply draws the texture stretched over that rectangle. */
static int
_tex_w(GgazeViewer *p_v) {
   gint i_w = 0;
   if (p_v->p_texture != NULL) {
      logical_size_get(p_v->p_texture, &i_w, NULL);
   }
   return (i_w);
}

static int
_tex_h(GgazeViewer *p_v) {
   gint i_h = 0;
   if (p_v->p_texture != NULL) {
      logical_size_get(p_v->p_texture, NULL, &i_h);
   }
   return (i_h);
}

/* The fit-to-window ratio for the given allocation: the largest scale that
 * shows the whole image. 1.0 when either the texture or the allocation has no
 * size yet, so callers never divide by zero.
 *
 * Note this is NOT bounded by GGAZE_ZOOM_MAX -- an image small enough relative
 * to the window fits at far more than 6400% (a 6x3 image in a 600x400 window
 * fits at 100x), nor by GGAZE_ZOOM_MIN -- a 32768 px panorama in a 600 px
 * window fits at 1.83 %. _zoom_at relies on both being expressible; see the
 * limits gesture_math_clamp_zoom derives from this (jx0 / zb2). */
static gdouble
_fit_scale(GgazeViewer *p_v, int i_w, int i_h) {
   int i_tw = _tex_w(p_v);
   int i_th = _tex_h(p_v);
   if (i_tw <= 0 || i_th <= 0 || i_w <= 0 || i_h <= 0) {
      return (1.0);
   }
   return (MIN((gdouble)i_w / i_tw, (gdouble)i_h / i_th));
}

/* Display scale + clamped top-left for the current state. Also writes the
 * clamped pan back so stored state matches what is drawn (no "dead zone" when
 * a new drag begins from a clamped edge). */
static void
_compute_geom(GgazeViewer *p_v, int i_w, int i_h, gdouble *p_scale,
              gdouble *p_x, gdouble *p_y, gdouble *p_dw, gdouble *p_dh) {
   int i_tw = _tex_w(p_v);
   int i_th = _tex_h(p_v);

   gdouble s = p_v->b_fit ? _fit_scale(p_v, i_w, i_h) : p_v->d_zoom;

   gdouble dw = (gdouble)i_tw * s;
   gdouble dh = (gdouble)i_th * s;
   gdouble x  = ((gdouble)i_w - dw) / 2.0 + p_v->d_pan_x;
   gdouble y  = ((gdouble)i_h - dh) / 2.0 + p_v->d_pan_y;

   /* Clamp so the image can't drift off-screen. */
   gdouble cx =
      CLAMP(x, MIN(0.0, (gdouble)i_w - dw), MAX(0.0, (gdouble)i_w - dw));
   gdouble cy =
      CLAMP(y, MIN(0.0, (gdouble)i_h - dh), MAX(0.0, (gdouble)i_h - dh));
   /* Write clamped pan back so the stored state tracks the drawn position. */
   p_v->d_pan_x = cx - ((gdouble)i_w - dw) / 2.0;
   p_v->d_pan_y = cy - ((gdouble)i_h - dh) / 2.0;

   if (p_scale != NULL) {
      *p_scale = s;
   }
   if (p_x != NULL) {
      *p_x = cx;
   }
   if (p_y != NULL) {
      *p_y = cy;
   }
   if (p_dw != NULL) {
      *p_dw = dw;
   }
   if (p_dh != NULL) {
      *p_dh = dh;
   }
}

static gdouble
_current_scale(GgazeViewer *p_v) {
   if (p_v->b_fit) {
      gdouble s = 1.0;
      _compute_geom(p_v, gtk_widget_get_width(GTK_WIDGET(p_v)),
                    gtk_widget_get_height(GTK_WIDGET(p_v)), &s, NULL, NULL,
                    NULL, NULL);
      return (s);
   }
   return (p_v->d_zoom);
}

/* Zoom around widget point (d_cx, d_cy), keeping that point over the same
 * image pixel -- the one zoom path the wheel, the keys and a pinch share.
 * The rule itself, the 2 %..6400 % clamp with both limits widened to the
 * fit ratio (jx0 / zb2) and to the current scale (so a zoom a resize left
 * outside them is never pulled back through them) and the non-finite
 * guard (hx0) are gesture-math.c's
 * gesture_math_zoom_about; _compute_geom clamps the pan on the next draw.
 *
 * hx0 in short: a non-finite centre or zoom is rejected, not stored. CLAMP
 * cannot filter NaN, and a single NaN reaching d_pan_x/d_pan_y is not a
 * one-frame glitch: the draw rect goes NaN, the image disappears, and
 * because every later zoom derives the new pan from the old one the widget
 * never recovers. Bailing keeps the last good geometry on screen. */
static void
_zoom_at(GgazeViewer *p_v, gdouble d_cx, gdouble d_cy, gdouble d_new_zoom) {
   if (p_v->p_texture == NULL) {
      return;
   }
   int         i_w    = gtk_widget_get_width(GTK_WIDGET(p_v));
   int         i_h    = gtk_widget_get_height(GTK_WIDGET(p_v));
   GestureView t_view = {.i_w     = i_w,
                         .i_h     = i_h,
                         .i_tex_w = _tex_w(p_v),
                         .i_tex_h = _tex_h(p_v),
                         .d_fit   = _fit_scale(p_v, i_w, i_h)};
   _compute_geom(p_v, i_w, i_h, &t_view.d_scale, &t_view.d_x, &t_view.d_y, NULL,
                 NULL);
   gdouble d_zoom, d_pan_x, d_pan_y;
   if (!gesture_math_zoom_about(&t_view, d_cx, d_cy, d_new_zoom, &d_zoom,
                                &d_pan_x, &d_pan_y)) {
      return;
   }
   p_v->b_fit   = FALSE;
   p_v->d_zoom  = d_zoom;
   p_v->d_pan_x = d_pan_x;
   p_v->d_pan_y = d_pan_y;
   gtk_widget_queue_draw(GTK_WIDGET(p_v));
}

/* Remember the view as it is now: what a two-finger tap puts back (zb2).
 * Taken when the tap's first finger goes down -- at the drag's BEGIN when
 * one finger came first, else at the pinch's -- so the few px a first
 * finger pans before the second lands are undone too. */
static void
_view_save(const GgazeViewer *p_v, ViewState *p_out) {
   p_out->b_fit   = p_v->b_fit;
   p_out->d_zoom  = p_v->d_zoom;
   p_out->d_pan_x = p_v->d_pan_x;
   p_out->d_pan_y = p_v->d_pan_y;
}

static void
_view_restore(GgazeViewer *p_v, const ViewState *p_in) {
   p_v->b_fit   = p_in->b_fit;
   p_v->d_zoom  = p_in->d_zoom;
   p_v->d_pan_x = p_in->d_pan_x;
   p_v->d_pan_y = p_in->d_pan_y;
   gtk_widget_queue_draw(GTK_WIDGET(p_v));
}

static void
_snapshot_view(GgazeViewer *p_v) {
   _view_save(p_v, &p_v->t_pre);
}

/* Forget a pinch in progress (zb2): a new texture (set_texture) or an
 * unmap ends it as no tap and no zoom -- the rest of that pinch, whatever
 * GtkGestureZoom still reports, is ignored, since update / end need
 * b_pinching -- and the saved views become the view now, so a restore can
 * never put an old image's zoom and pan on a new one. */
static void
_pinch_reset(GgazeViewer *p_v) {
   p_v->b_pinching        = FALSE;
   p_v->b_pinch_can_tap   = FALSE;
   p_v->b_pinch_from_tool = FALSE;
   p_v->d_pinch_zoom0     = 1.0;
   _snapshot_view(p_v);
   _view_save(p_v, &p_v->t_pinch0);
}

static void _drag_abandon(GgazeViewer *p_v);

/* Forget a drag and a pinch in progress (zb2 review): a new texture or an
 * unmap. A tool drag gets its CANCEL first (viewer.h: taken away, not
 * finished); then the rest of that drag -- whatever the drag gesture
 * still reports until its own end -- is ignored (b_drag_dead), since its
 * pan origin and its tap time / wander belong to the old picture or the
 * old mapping, and a pinch that follows starts afresh rather than "from
 * the drag". Safe at window teardown too, and for the opposite reason
 * one might guess: on GTK 4.22 gtk_window_destroy unmaps the children
 * FIRST (this runs from ggaze_viewer_unmap) and only then disposes the
 * window (ggaze_window_dispose -> tool_ctrl_dispose drops the overlay).
 * So a tool that was mid-drag is still alive and installed at the unmap
 * and gets its CANCEL like any other; by the time it is torn down the
 * viewer holds no drag of it. */
static void
_gestures_reset(GgazeViewer *p_v) {
   _drag_abandon(p_v);
   p_v->b_dragging        = FALSE;
   p_v->b_drag_to_overlay = FALSE;
   p_v->b_drag_dead       = TRUE; /* until the next BEGIN or END */
   p_v->d_drag_max_move   = 0.0;
   _pinch_reset(p_v);
}

/* --- GtkWidget vfuncs ----------------------------------------------------- */

static void
ggaze_viewer_measure(GtkWidget *p_widget, GtkOrientation o, int i_for_size,
                     int *p_min, int *p_nat, int *p_min_bl, int *p_nat_bl) {
   (void)p_widget;
   (void)o;
   (void)i_for_size;
   /* Fit-to-window fills the allocation via hexpand/vexpand; the viewer's
    * own natural size is small so a large image doesn't grow the window. */
   *p_min    = 1;
   *p_nat    = 1;
   *p_min_bl = -1;
   *p_nat_bl = -1;
}

/* The background colour for e_bg (configurable via settings, applied by
 * the window). */
static GdkRGBA
_background_rgba(GgazeBackground e_bg) {
   switch (e_bg) {
   case GGAZE_BG_BLACK:
      return ((GdkRGBA){0.0f, 0.0f, 0.0f, 1.0f});
   case GGAZE_BG_GREY:
      return ((GdkRGBA){0.2f, 0.2f, 0.2f, 1.0f});
   case GGAZE_BG_CHECKER:
      /* Flat mid-grey placeholder; a real checkerboard pattern would be drawn
       * here. Keeps the background distinct from dark/grey for now. */
      return ((GdkRGBA){0.3f, 0.3f, 0.3f, 1.0f});
   case GGAZE_BG_DARK:
   default:
      return ((GdkRGBA){0.07f, 0.07f, 0.07f, 1.0f});
   }
}

static void
ggaze_viewer_snapshot(GtkWidget *p_widget, GtkSnapshot *p_snap) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_widget);
   int          i_w = gtk_widget_get_width(p_widget);
   int          i_h = gtk_widget_get_height(p_widget);

   GdkRGBA         bg = _background_rgba(p_v->e_bg);
   graphene_rect_t bg_rect =
      GRAPHENE_RECT_INIT(0.f, 0.f, (float)i_w, (float)i_h);
   gtk_snapshot_append_color(p_snap, &bg, &bg_rect);

   if (p_v->p_texture == NULL) {
      return;
   }

   gdouble x, y, dw, dh;
   _compute_geom(p_v, i_w, i_h, NULL, &x, &y, &dw, &dh);
   if (dw <= 0.0 || dh <= 0.0) {
      return;
   }
   graphene_rect_t rect =
      GRAPHENE_RECT_INIT((float)x, (float)y, (float)dw, (float)dh);
   /* The current animation frame, if playing, at the first frame's
    * geometry (the frames are the canvas size, so nothing moves). */
   gtk_snapshot_append_texture(p_snap, _drawn_texture(p_v), &rect);
   if (p_v->fn_overlay != NULL) {
      GgazeViewerGeom t_geom;
      if (ggaze_viewer_get_geometry(p_v, &t_geom)) {
         p_v->fn_overlay(p_snap, &t_geom, p_v->p_overlay_data);
      }
   }
}

/* Frames are only produced while the widget can be seen: the stack page
 * behind the grid, or a window not yet presented, plays nothing. */
static void
ggaze_viewer_map(GtkWidget *p_widget) {
   GTK_WIDGET_CLASS(ggaze_viewer_parent_class)->map(p_widget);
   _anim_start(GGAZE_VIEWER(p_widget));
}

static void
ggaze_viewer_unmap(GtkWidget *p_widget) {
   _anim_stop(GGAZE_VIEWER(p_widget));
   /* An unmapped widget gets no more touches; a drag or pinch it was in
    * must not wake up as a pan, a tap or a zoom on the next map
    * (hardening, zb2). */
   _gestures_reset(GGAZE_VIEWER(p_widget));
   GTK_WIDGET_CLASS(ggaze_viewer_parent_class)->unmap(p_widget);
}

static void
ggaze_viewer_dispose(GObject *p_obj) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_obj);
   _anim_stop(p_v);
   p_v->p_anim = NULL; /* borrowed from p_texture, which goes next */
   g_clear_object(&p_v->p_texture);
   G_OBJECT_CLASS(ggaze_viewer_parent_class)->dispose(p_obj);
}

static void
ggaze_viewer_class_init(GgazeViewerClass *p_klass) {
   GtkWidgetClass *p_wc = GTK_WIDGET_CLASS(p_klass);
   GObjectClass   *p_oc = G_OBJECT_CLASS(p_klass);
   p_wc->measure        = ggaze_viewer_measure;
   p_wc->snapshot       = ggaze_viewer_snapshot;
   p_wc->map            = ggaze_viewer_map;
   p_wc->unmap          = ggaze_viewer_unmap;
   p_oc->dispose        = ggaze_viewer_dispose;
   gtk_widget_class_set_css_name(p_wc, "ggazeviewer");
   /* "navigate": emitted by the scroll wheel in GGAZE_SCROLL_NAVIGATE mode
    * and by a horizontal touch swipe (zb2). The int arg is +1 (next) or -1
    * (prev). The window routes it through its Save/Discard/Cancel gate. */
   u_navigate_sig =
      g_signal_new("navigate", G_OBJECT_CLASS_TYPE(p_oc), G_SIGNAL_RUN_LAST, 0,
                   NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
   /* "toggle-info": a two-finger tap (zb2). An intent, like "navigate": the
    * viewer does not know the window's action names. */
   u_toggle_info_sig =
      g_signal_new("toggle-info", G_OBJECT_CLASS_TYPE(p_oc), G_SIGNAL_RUN_LAST,
                   0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/* --- controllers ---------------------------------------------------------- */

/* A drag either pans the image or, while a tool overlay is installed, is
 * handed to the tool in absolute widget coordinates (GtkGestureDrag reports
 * offsets from the start point; the tool wants positions). Which of the two
 * it is gets decided at BEGIN and holds for the whole gesture: a tool that
 * starts during a PAN drag never sees an UPDATE without its BEGIN (one
 * that REPLACES the tool drag's overlay mid-drag does -- viewer.h, both
 * tools ignore it), and a tool that goes away mid-drag (Esc while
 * dragging the rectangle) hands the rest of the gesture to panning from
 * where the pointer is NOW -- the pan origin is re-based at that moment,
 * so the first pan step is not the whole offset accumulated since the
 * press. */
static void
_drag_begin_cb(GtkGestureDrag *p_gesture, gdouble d_x, gdouble d_y,
               gpointer p_data) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_data);
   (void)p_gesture;
   p_v->d_drag_start_x     = d_x;
   p_v->d_drag_start_y     = d_y;
   p_v->d_drag_start_pan_x = p_v->d_pan_x;
   p_v->d_drag_start_pan_y = p_v->d_pan_y;
   p_v->d_drag_dx          = 0.0;
   p_v->d_drag_dy          = 0.0;
   p_v->d_drag_max_move    = 0.0;
   p_v->i_drag_start_us    = g_get_monotonic_time();
   p_v->b_dragging         = TRUE;
   /* This finger may be the first of a two-finger tap, whose view restore
    * must go back to before ANY of it -- including the few px this finger
    * pans before the second lands (ggaze_viewer_pinch_begin). */
   if (!p_v->b_pinching) {
      _snapshot_view(p_v);
   }
   /* A drag that starts while two fingers pinch is part of the pinch. */
   p_v->b_drag_dead       = p_v->b_pinching;
   p_v->b_drag_to_overlay = (p_v->fn_drag != NULL && !p_v->b_drag_dead);
   if (p_v->b_drag_to_overlay) {
      p_v->fn_drag(GGAZE_VIEWER_DRAG_BEGIN, d_x, d_y, p_v->p_overlay_data);
   }
}

/* A pinch began while one finger was dragging (zb2): take that drag away --
 * a tool gets a CANCEL where the finger is now (viewer.h: the straighten
 * tool drops its line, the crop rectangle keeps what was dragged so far);
 * a pan simply stops -- and ignore whatever the drag gesture still
 * reports. CANCEL, not END: an END means "the user finished this drag",
 * and the straighten tool levels the image on one. The pinch also DENIES the
 * drag gesture's sequence (_pinch_begin), which makes GTK end it for real; this
 * flag is what keeps the viewer's own state right in between, and when the
 * gesture is driven by emitted signals rather than touches (tests). */
static void
_drag_abandon(GgazeViewer *p_v) {
   if (!p_v->b_dragging || p_v->b_drag_dead) {
      return;
   }
   if (p_v->b_drag_to_overlay && p_v->fn_drag != NULL) {
      p_v->fn_drag(GGAZE_VIEWER_DRAG_CANCEL,
                   p_v->d_drag_start_x + p_v->d_drag_dx,
                   p_v->d_drag_start_y + p_v->d_drag_dy, p_v->p_overlay_data);
   }
   p_v->b_drag_to_overlay = FALSE;
   p_v->b_drag_dead       = TRUE;
}

static void
_drag_update_cb(GtkGestureDrag *p_gesture, gdouble d_dx, gdouble d_dy,
                gpointer p_data) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_data);
   (void)p_gesture;
   if (p_v->b_drag_dead) {
      return;
   }
   p_v->d_drag_dx = d_dx;
   p_v->d_drag_dy = d_dy;
   gdouble d_move = hypot(d_dx, d_dy);
   p_v->d_drag_max_move =
      isfinite(d_move) ? MAX(p_v->d_drag_max_move, d_move) : INFINITY;
   if (p_v->b_drag_to_overlay) {
      if (p_v->fn_drag != NULL) {
         p_v->fn_drag(GGAZE_VIEWER_DRAG_UPDATE, p_v->d_drag_start_x + d_dx,
                      p_v->d_drag_start_y + d_dy, p_v->p_overlay_data);
         return;
      }
      /* The overlay left mid-gesture: from here on this is a pan, measured
       * from the current offset so nothing jumps. */
      p_v->d_drag_start_pan_x = p_v->d_pan_x - d_dx;
      p_v->d_drag_start_pan_y = p_v->d_pan_y - d_dy;
      p_v->b_drag_to_overlay  = FALSE;
   }
   p_v->d_pan_x = p_v->d_drag_start_pan_x + d_dx;
   p_v->d_pan_y = p_v->d_drag_start_pan_y + d_dy;
   gtk_widget_queue_draw(GTK_WIDGET(p_v));
}

static void
_drag_end_cb(GtkGestureDrag *p_gesture, gdouble d_dx, gdouble d_dy,
             gpointer p_data) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_data);
   (void)p_gesture;
   if (p_v->b_drag_to_overlay && p_v->fn_drag != NULL && !p_v->b_drag_dead) {
      p_v->fn_drag(GGAZE_VIEWER_DRAG_END, p_v->d_drag_start_x + d_dx,
                   p_v->d_drag_start_y + d_dy, p_v->p_overlay_data);
   }
   p_v->b_drag_to_overlay = FALSE;
   p_v->b_dragging        = FALSE;
   p_v->b_drag_dead       = FALSE;
}

/* The zoom centre for a scroll event: the pointer position translated into
 * widget space when the event carries a usable one, else the widget centre.
 *
 * hx0: the return value of gdk_event_get_position() is NOT optional here.
 * A scroll event frequently has no position at all -- on X11 it reports
 * none for ordinary wheel events -- and GDK then writes NAN to BOTH
 * out-parameters rather than leaving them untouched. Ignoring the result
 * therefore did not "keep the centre default": it replaced it with NaN,
 * which flowed through _zoom_at into d_pan_x/d_pan_y, made _compute_geom
 * hand ggaze_viewer_snapshot a NaN rect, and left the texture undrawn --
 * the picture vanished on the FIRST wheel notch. Worse, the NaN persisted
 * in the pan fields, so every later zoom recomputed NaN from NaN and even
 * the keyboard could not bring the image back.
 *
 * The position, when there is one, is in the SURFACE coordinate space, so
 * it must be translated into widget space -- otherwise the header bar's
 * height alone offsets every cursor-centred zoom. The isfinite() check
 * guards the translated result too: it is the invariant the pan/zoom state
 * depends on, and it is cheaper to enforce here than to reason about every
 * arithmetic path downstream. */
static void
_event_zoom_centre(GgazeViewer *p_v, GtkEventController *p_ctrl, gdouble *p_cx,
                   gdouble *p_cy) {
   *p_cx             = (gdouble)gtk_widget_get_width(GTK_WIDGET(p_v)) / 2.0;
   *p_cy             = (gdouble)gtk_widget_get_height(GTK_WIDGET(p_v)) / 2.0;
   GdkEvent *p_event = gtk_event_controller_get_current_event(p_ctrl);
   GtkRoot  *p_root  = gtk_widget_get_root(GTK_WIDGET(p_v));
   gdouble   d_sx, d_sy;
   if (p_event == NULL || p_root == NULL ||
       !gdk_event_get_position(p_event, &d_sx, &d_sy)) {
      return;
   }
   graphene_point_t pt_out;
   if (gtk_widget_compute_point(GTK_WIDGET(p_root), GTK_WIDGET(p_v),
                                &GRAPHENE_POINT_INIT((float)d_sx, (float)d_sy),
                                &pt_out) &&
       isfinite(pt_out.x) && isfinite(pt_out.y)) {
      *p_cx = (gdouble)pt_out.x;
      *p_cy = (gdouble)pt_out.y;
   }
}

/* What one vertical wheel step d_dy does under the configured scroll
 * behaviour (e_scroll), zooming about (d_cx, d_cy) in zoom mode. TRUE when
 * the viewer consumed the event. */
static gboolean
_scroll_dispatch(GgazeViewer *p_v, gdouble d_dy, gdouble d_cx, gdouble d_cy) {
   switch (p_v->e_scroll) {
   case GGAZE_SCROLL_PAN_WHEN_ZOOMED:
      /* Only pan when zoomed in; at fit-to-window the wheel is a no-op so the
       * event stays available for any outer controller. */
      if (p_v->b_fit) {
         return (FALSE);
      }
      ggaze_viewer_pan(p_v, 0.0, -d_dy * GGAZE_VIEWER_PAN_STEP);
      return (TRUE);
   case GGAZE_SCROLL_NAVIGATE:
      /* Emit "navigate": d_dy > 0 -> next, < 0 -> prev. The window connects
       * and advances the navigator. */
      g_signal_emit(p_v, u_navigate_sig, 0, d_dy > 0.0 ? 1 : -1);
      return (TRUE);
   case GGAZE_SCROLL_ZOOM:
   default:
      break;
   }
   gdouble d_factor =
      (d_dy < 0.0) ? GGAZE_ZOOM_FACTOR : 1.0 / GGAZE_ZOOM_FACTOR;
   _zoom_at(p_v, d_cx, d_cy, _current_scale(p_v) * d_factor);
   return (TRUE);
}

/* The scroll controller's handler: nothing to do without a picture; else
 * the zoom centre (_event_zoom_centre, the hx0 rules) and the configured
 * behaviour (_scroll_dispatch). */
static gboolean
_scroll_cb(GtkEventControllerScroll *p_scroll, gdouble d_dx, gdouble d_dy,
           gpointer p_data) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_data);
   (void)d_dx;
   if (p_v->p_texture == NULL) {
      return (FALSE);
   }
   gdouble d_cx, d_cy;
   _event_zoom_centre(p_v, GTK_EVENT_CONTROLLER(p_scroll), &d_cx, &d_cy);
   return (_scroll_dispatch(p_v, d_dy, d_cx, d_cy));
}

/* --- touch gestures (zb2) ------------------------------------------------ */

/* The midpoint of a gesture's touches in widget coordinates. FALSE, the
 * outputs untouched, when the gesture has no points -- it is being driven
 * by emitted signals (tests) -- or GTK hands back something non-finite (the
 * hx0 invariant: nothing non-finite reaches the zoom state). The
 * is_active check is not decoration: gtk_gesture_get_bounding_box_center
 * inspects the last event, and with no points that is NULL (a
 * Gdk-CRITICAL on gtk 4.22). */
static gboolean
_gesture_midpoint(GtkGesture *p_g, gdouble *p_x, gdouble *p_y) {
   gdouble d_x, d_y;
   if (!gtk_gesture_is_active(p_g) ||
       !gtk_gesture_get_bounding_box_center(p_g, &d_x, &d_y) ||
       !isfinite(d_x) || !isfinite(d_y)) {
      return (FALSE);
   }
   *p_x = d_x;
   *p_y = d_y;
   return (TRUE);
}

/* GtkGestureZoom recognised two touches (or a touchpad pinch started). A
 * thin adapter: the midpoint (the widget centre while the gesture reports
 * none) and whether the event is a touchpad pinch go to
 * ggaze_viewer_pinch_begin, which owns every rule. */
static void
_pinch_begin_cb(GtkGesture *p_g, GdkEventSequence *p_seq, gpointer p_data) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_data);
   (void)p_seq;
   gdouble d_cx = (gdouble)gtk_widget_get_width(GTK_WIDGET(p_v)) / 2.0;
   gdouble d_cy = (gdouble)gtk_widget_get_height(GTK_WIDGET(p_v)) / 2.0;
   _gesture_midpoint(p_g, &d_cx, &d_cy);
   GdkEvent *p_ev =
      gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(p_g));
   ggaze_viewer_pinch_begin(p_v, d_cx, d_cy,
                            p_ev != NULL && gdk_event_get_event_type(p_ev) ==
                                               GDK_TOUCHPAD_PINCH);
}

/* The finger distance changed: zoom, and follow the midpoint where it is
 * now (where it last was while the gesture reports none: no pan then).
 * GtkGestureZoom reports on every change of the finger distance, which a
 * real two-finger move never keeps exactly constant. */
static void
_pinch_scale_cb(GtkGestureZoom *p_g, gdouble d_scale, gpointer p_data) {
   GgazeViewer *p_v  = GGAZE_VIEWER(p_data);
   gdouble      d_cx = p_v->d_pinch_last_cx;
   gdouble      d_cy = p_v->d_pinch_last_cy;
   _gesture_midpoint(GTK_GESTURE(p_g), &d_cx, &d_cy);
   ggaze_viewer_pinch_update(p_v, d_scale, d_cx, d_cy);
}

/* A sequence was cancelled (another widget claimed it, the window lost the
 * touch): whatever this was, it was not a tap. "end" follows. */
static void
_pinch_cancel_cb(GtkGesture *p_g, GdkEventSequence *p_seq, gpointer p_data) {
   (void)p_g;
   (void)p_seq;
   GGAZE_VIEWER(p_data)->b_pinch_can_tap = FALSE;
}

static void
_pinch_end_cb(GtkGesture *p_g, GdkEventSequence *p_seq, gpointer p_data) {
   (void)p_g;
   (void)p_seq;
   ggaze_viewer_pinch_end(GGAZE_VIEWER(p_data));
}

/* One finger went down: remember where (ggaze_viewer_swipe_track). NaN
 * when GTK has no point for the sequence, which
 * gesture_math_swipe_direction refuses. */
static void
_swipe_begin_cb(GtkGesture *p_g, GdkEventSequence *p_seq, gpointer p_data) {
   gdouble d_x = NAN;
   gdouble d_y = NAN;
   gtk_gesture_get_point(p_g, p_seq, &d_x, &d_y);
   ggaze_viewer_swipe_track(GGAZE_VIEWER(p_data), TRUE, d_x, d_y);
}

static void
_swipe_update_cb(GtkGesture *p_g, GdkEventSequence *p_seq, gpointer p_data) {
   gdouble d_x, d_y;
   if (gtk_gesture_get_point(p_g, p_seq, &d_x, &d_y)) {
      ggaze_viewer_swipe_track(GGAZE_VIEWER(p_data), FALSE, d_x, d_y);
   }
}

/* The finger lifted with velocity (d_vx, d_vy): judge the whole path --
 * unless a pinch began while it was down (the finger was half of it) or a
 * slideshow step changed the picture under it (ggaze_viewer_spoil_swipe). */
static void
_swipe_cb(GtkGestureSwipe *p_g, gdouble d_vx, gdouble d_vy, gpointer p_data) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_data);
   (void)p_g;
   if (p_v->b_swipe_spoiled) {
      return;
   }
   ggaze_viewer_swipe(p_v, p_v->d_swipe_x - p_v->d_swipe_x0,
                      p_v->d_swipe_y - p_v->d_swipe_y0, d_vx, d_vy);
}

/* Pinch (GtkGestureZoom: two touches, or a touchpad pinch) and swipe
 * (GtkGestureSwipe, touch only so a mouse drag stays a pan). Neither
 * claims its sequences, so the drag gesture keeps seeing the first finger
 * -- a one-finger drag still pans or drags the tool's rectangle -- until a
 * pinch starts and abandons it (_drag_abandon). The mouse and the wheel go
 * through none of this. */
static void
_init_touch_gestures(GgazeViewer *p_v) {
   GtkGesture *p_zoom = gtk_gesture_zoom_new();
   gtk_widget_add_controller(GTK_WIDGET(p_v), GTK_EVENT_CONTROLLER(p_zoom));
   g_signal_connect(p_zoom, "begin", G_CALLBACK(_pinch_begin_cb), p_v);
   g_signal_connect(p_zoom, "scale-changed", G_CALLBACK(_pinch_scale_cb), p_v);
   g_signal_connect(p_zoom, "cancel", G_CALLBACK(_pinch_cancel_cb), p_v);
   g_signal_connect(p_zoom, "end", G_CALLBACK(_pinch_end_cb), p_v);

   GtkGesture *p_swipe = gtk_gesture_swipe_new();
   gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(p_swipe), TRUE);
   gtk_widget_add_controller(GTK_WIDGET(p_v), GTK_EVENT_CONTROLLER(p_swipe));
   g_signal_connect(p_swipe, "begin", G_CALLBACK(_swipe_begin_cb), p_v);
   g_signal_connect(p_swipe, "update", G_CALLBACK(_swipe_update_cb), p_v);
   g_signal_connect(p_swipe, "swipe", G_CALLBACK(_swipe_cb), p_v);
}

static void
ggaze_viewer_init(GgazeViewer *p_v) {
   p_v->p_texture = NULL;
   p_v->b_fit     = TRUE;
   p_v->d_zoom    = 1.0;
   p_v->d_pan_x   = 0.0;
   p_v->d_pan_y   = 0.0;
   p_v->e_bg      = GGAZE_BG_DARK;
   p_v->e_scroll  = GGAZE_SCROLL_ZOOM;

   gtk_widget_set_focusable(GTK_WIDGET(p_v), TRUE);

   GtkGesture *p_drag  = gtk_gesture_drag_new();
   p_v->p_drag_gesture = p_drag;
   gtk_widget_add_controller(GTK_WIDGET(p_v), GTK_EVENT_CONTROLLER(p_drag));
   g_signal_connect(p_drag, "drag-begin", G_CALLBACK(_drag_begin_cb), p_v);
   g_signal_connect(p_drag, "drag-update", G_CALLBACK(_drag_update_cb), p_v);
   g_signal_connect(p_drag, "drag-end", G_CALLBACK(_drag_end_cb), p_v);

   GtkEventController *p_scroll =
      gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
   gtk_widget_add_controller(GTK_WIDGET(p_v), p_scroll);
   g_signal_connect(p_scroll, "scroll", G_CALLBACK(_scroll_cb), p_v);
   _init_touch_gestures(p_v);
   /* No key controller here: every key (zoom, pan, 0 = fit toggle, arrows)
    * is bound in shortcuts.c's single table to a win.* action the window
    * routes to the public methods below. A second, focus-dependent binding
    * set in the widget used to shadow that table and drift from the `?`
    * help. */
}

/* --- public API ----------------------------------------------------------- */

GtkWidget *
ggaze_viewer_new(void) {
   return (GTK_WIDGET(g_object_new(GGAZE_TYPE_VIEWER, NULL)));
}

void
ggaze_viewer_set_texture(GgazeViewer *p_viewer, GdkTexture *p_texture) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   /* Whatever was playing stops with its texture: a still, or the same
    * animation again, starts over from the first frame (the picture is
    * being (re)set, and zoom resets on the same rule). */
   _anim_stop(p_viewer);
   g_set_object(&p_viewer->p_texture, p_texture);
   p_viewer->p_anim = animation_lookup(p_texture);
   animation_playback_reset(&p_viewer->st_play);
   p_viewer->b_fit   = TRUE;
   p_viewer->d_zoom  = 1.0;
   p_viewer->d_pan_x = 0.0;
   p_viewer->d_pan_y = 0.0;
   /* A drag or pinch over the old picture is over (_gestures_reset): its
    * pan origin, zoom-at-begin and saved views belong to that picture. */
   _gestures_reset(p_viewer);
   _anim_start(p_viewer);
   gtk_widget_queue_draw(GTK_WIDGET(p_viewer));
}

GdkTexture *
ggaze_viewer_get_texture(GgazeViewer *p_viewer) {
   g_return_val_if_fail(GGAZE_IS_VIEWER(p_viewer), NULL);
   return (p_viewer->p_texture); /* (transfer none) */
}

GdkTexture *
ggaze_viewer_get_frame(GgazeViewer *p_viewer) {
   g_return_val_if_fail(GGAZE_IS_VIEWER(p_viewer), NULL);
   return (_drawn_texture(p_viewer));
}

gboolean
ggaze_viewer_is_animating(GgazeViewer *p_viewer) {
   g_return_val_if_fail(GGAZE_IS_VIEWER(p_viewer), FALSE);
   return (p_viewer->u_tick_id != 0 || p_viewer->u_wake_id != 0);
}

guint
ggaze_viewer_get_tick_count(GgazeViewer *p_viewer) {
   g_return_val_if_fail(GGAZE_IS_VIEWER(p_viewer), 0);
   return (p_viewer->u_ticks);
}

void
ggaze_viewer_hold_first_frame(GgazeViewer *p_viewer, gboolean b_hold) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   p_viewer->b_hold = b_hold;
   if (b_hold) {
      _anim_stop(p_viewer);
      animation_playback_reset(&p_viewer->st_play);
      gtk_widget_queue_draw(GTK_WIDGET(p_viewer));
   } else {
      _anim_start(p_viewer);
   }
}

void
ggaze_viewer_zoom_in(GgazeViewer *p_viewer) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   _zoom_at(p_viewer, (gdouble)gtk_widget_get_width(GTK_WIDGET(p_viewer)) / 2.0,
            (gdouble)gtk_widget_get_height(GTK_WIDGET(p_viewer)) / 2.0,
            _current_scale(p_viewer) * GGAZE_ZOOM_FACTOR);
}

void
ggaze_viewer_zoom_out(GgazeViewer *p_viewer) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   _zoom_at(p_viewer, (gdouble)gtk_widget_get_width(GTK_WIDGET(p_viewer)) / 2.0,
            (gdouble)gtk_widget_get_height(GTK_WIDGET(p_viewer)) / 2.0,
            _current_scale(p_viewer) / GGAZE_ZOOM_FACTOR);
}

gdouble
ggaze_viewer_get_scale(GgazeViewer *p_viewer) {
   g_return_val_if_fail(GGAZE_IS_VIEWER(p_viewer), 0.0);
   if (p_viewer->p_texture == NULL) {
      return (0.0);
   }
   return (_current_scale(p_viewer));
}

void
ggaze_viewer_get_pan(GgazeViewer *p_viewer, gdouble *p_x, gdouble *p_y) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   if (p_x != NULL) {
      *p_x = p_viewer->d_pan_x;
   }
   if (p_y != NULL) {
      *p_y = p_viewer->d_pan_y;
   }
}

void
ggaze_viewer_toggle_fit_100(GgazeViewer *p_viewer) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   if (p_viewer->b_fit) {
      p_viewer->b_fit   = FALSE;
      p_viewer->d_zoom  = 1.0;
      p_viewer->d_pan_x = 0.0;
      p_viewer->d_pan_y = 0.0;
   } else {
      p_viewer->b_fit = TRUE;
   }
   gtk_widget_queue_draw(GTK_WIDGET(p_viewer));
}

void
ggaze_viewer_pan(GgazeViewer *p_viewer, gdouble d_dx, gdouble d_dy) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   /* Same invariant _zoom_at enforces (hx0): the pan fields must stay finite.
    * A non-finite delta is absorbed by the += and is unrecoverable afterwards
    * -- the image stops being drawn and no later pan or zoom can restore it,
    * because every subsequent value derives from this one. The drag gesture
    * feeds this directly from event coordinates, so the guard belongs here
    * rather than at each call site. */
   if (!isfinite(d_dx) || !isfinite(d_dy)) {
      return;
   }
   p_viewer->d_pan_x += d_dx;
   p_viewer->d_pan_y += d_dy;
   gtk_widget_queue_draw(GTK_WIDGET(p_viewer));
}

gboolean
ggaze_viewer_get_geometry(GgazeViewer *p_viewer, GgazeViewerGeom *p_out) {
   g_return_val_if_fail(GGAZE_IS_VIEWER(p_viewer), FALSE);
   g_return_val_if_fail(p_out != NULL, FALSE);
   if (p_viewer->p_texture == NULL) {
      return (FALSE);
   }
   gdouble d_scale, d_x, d_y;
   _compute_geom(p_viewer, gtk_widget_get_width(GTK_WIDGET(p_viewer)),
                 gtk_widget_get_height(GTK_WIDGET(p_viewer)), &d_scale, &d_x,
                 &d_y, NULL, NULL);
   p_out->d_x     = d_x;
   p_out->d_y     = d_y;
   p_out->d_scale = d_scale;
   p_out->i_img_w = _tex_w(p_viewer);
   p_out->i_img_h = _tex_h(p_viewer);
   return (TRUE);
}

void
ggaze_viewer_set_overlay(GgazeViewer *p_viewer, GgazeViewerOverlayFn fn_draw,
                         GgazeViewerDragFn fn_drag, gpointer p_data) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   p_viewer->fn_overlay     = fn_draw;
   p_viewer->fn_drag        = fn_drag;
   p_viewer->p_overlay_data = p_data;
   gtk_widget_queue_draw(GTK_WIDGET(p_viewer));
}

void
ggaze_viewer_set_background(GgazeViewer *p_viewer, GgazeBackground e_bg) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   p_viewer->e_bg = e_bg;
   gtk_widget_queue_draw(GTK_WIDGET(p_viewer));
}

void
ggaze_viewer_set_scroll_behavior(GgazeViewer        *p_viewer,
                                 GgazeScrollBehavior e_scroll) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   p_viewer->e_scroll = e_scroll;
}

/* --- touch gestures, public half (zb2; viewer.h) -------------------------- */

void
ggaze_viewer_pinch_begin(GgazeViewer *p_viewer, gdouble d_cx, gdouble d_cy,
                         gboolean b_touchpad) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   gboolean b_from_drag = p_viewer->b_dragging && !p_viewer->b_drag_dead;
   /* Whether a tool must undo that drag should this turn out a tap. */
   p_viewer->b_pinch_from_tool = b_from_drag && p_viewer->b_drag_to_overlay;
   /* The first finger's drag is over: it becomes half of this pinch. Deny
    * it to GTK too, so the gesture ends for real rather than panning (or
    * dragging the crop rectangle) under the pinch. */
   _drag_abandon(p_viewer);
   gtk_gesture_set_state(p_viewer->p_drag_gesture, GTK_EVENT_SEQUENCE_DENIED);
   if (!isfinite(d_cx) || !isfinite(d_cy)) { /* hx0: the pan follows it */
      d_cx = (gdouble)gtk_widget_get_width(GTK_WIDGET(p_viewer)) / 2.0;
      d_cy = (gdouble)gtk_widget_get_height(GTK_WIDGET(p_viewer)) / 2.0;
   }
   p_viewer->b_pinching = TRUE;
   /* A touchpad pinch zooms like a touchscreen one but is never a tap:
    * resting two fingers on a touchpad must not toggle the info card. */
   p_viewer->b_pinch_can_tap = !b_touchpad;
   p_viewer->b_swipe_spoiled = TRUE;
   p_viewer->d_pinch_zoom0   = _current_scale(p_viewer);
   _view_save(p_viewer, &p_viewer->t_pinch0);
   p_viewer->d_pinch_cx0     = d_cx;
   p_viewer->d_pinch_cy0     = d_cy;
   p_viewer->d_pinch_last_cx = d_cx;
   p_viewer->d_pinch_last_cy = d_cy;
   p_viewer->d_pinch_max_dev = 0.0;
   if (b_from_drag) {
      /* A tap starts with its first finger: its time, its wander and its
       * view (_snapshot_view at the drag's BEGIN) count. */
      p_viewer->d_pinch_max_move = p_viewer->d_drag_max_move;
      p_viewer->i_pinch_start_us = p_viewer->i_drag_start_us;
   } else {
      p_viewer->d_pinch_max_move = 0.0;
      p_viewer->i_pinch_start_us = g_get_monotonic_time();
      _snapshot_view(p_viewer);
   }
}

/* Record how far the gesture strayed from a tap: the midpoint's distance
 * from where it began and the scale's distance from 1. Non-finite values
 * count as "far" -- whatever they were, they were not a tap. */
static void
_pinch_track(GgazeViewer *p_v, gdouble d_scale, gdouble d_cx, gdouble d_cy) {
   gdouble d_move = hypot(d_cx - p_v->d_pinch_cx0, d_cy - p_v->d_pinch_cy0);
   gdouble d_dev  = fabs(d_scale - 1.0);
   p_v->d_pinch_max_move =
      isfinite(d_move) ? MAX(p_v->d_pinch_max_move, d_move) : INFINITY;
   p_v->d_pinch_max_dev =
      isfinite(d_dev) ? MAX(p_v->d_pinch_max_dev, d_dev) : INFINITY;
}

/* Move the picture by how far the pinch's (finite) midpoint moved since
 * the last update. _compute_geom clamps the pan on the next draw, as for a
 * drag. */
static void
_pinch_follow(GgazeViewer *p_v, gdouble d_cx, gdouble d_cy) {
   if (p_v->p_texture == NULL) {
      return;
   }
   p_v->d_pan_x += d_cx - p_v->d_pinch_last_cx;
   p_v->d_pan_y += d_cy - p_v->d_pinch_last_cy;
   p_v->d_pinch_last_cx = d_cx;
   p_v->d_pinch_last_cy = d_cy;
   gtk_widget_queue_draw(GTK_WIDGET(p_v));
}

/* TRUE while a pinch that began over a fitted picture has zoomed it by no
 * more than a two-finger tap may wobble (GESTURE_TAP_MAX_SCALE_DEV): the
 * fit detent. A non-finite scale is outside it (and then refused by the
 * zoom, as ever). Past it the zoom is measured from the band's edge
 * (gesture_math_detent_scale), so leaving the detent is continuous. */
static gboolean
_pinch_in_fit_detent(const GgazeViewer *p_v, gdouble d_scale) {
   return (p_v->t_pinch0.b_fit &&
           fabs(d_scale - 1.0) <= GESTURE_TAP_MAX_SCALE_DEV);
}

/* Hold the picture at fit inside the detent: fit back on, and the pinch's
 * zoom-at-begin, but the pan left where it is now (clamped to the fitted
 * picture at once, so the stored state is what is drawn). Why not the pan
 * the pinch began at (zb2 fourth review): fit mode does not re-centre --
 * _compute_geom keeps a fitted picture's pan anywhere inside its
 * letterbox, and a one-finger drag moves it there -- so a picture the
 * fingers moved along the letterbox while zoomed past the band has a
 * legitimate fitted position where it is. Putting back the begin pan
 * snapped it across the letterbox (tens of px) on re-entry; keeping it
 * makes re-entry as continuous as leaving (at the band's edge the rebased
 * zoom IS the fit zoom). */
static void
_pinch_hold_fit(GgazeViewer *p_v) {
   p_v->b_fit  = p_v->t_pinch0.b_fit;
   p_v->d_zoom = p_v->t_pinch0.d_zoom;
   if (p_v->p_texture != NULL) {
      _compute_geom(p_v, gtk_widget_get_width(GTK_WIDGET(p_v)),
                    gtk_widget_get_height(GTK_WIDGET(p_v)), NULL, NULL, NULL,
                    NULL, NULL);
   }
   gtk_widget_queue_draw(GTK_WIDGET(p_v));
}

void
ggaze_viewer_pinch_update(GgazeViewer *p_viewer, gdouble d_scale, gdouble d_cx,
                          gdouble d_cy) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   if (!p_viewer->b_pinching) {
      return;
   }
   _pinch_track(p_viewer, d_scale, d_cx, d_cy);
   /* The zoom is absolute, from the zoom the pinch began at: rounding
    * cannot build up over the many updates of one pinch. It is about the
    * midpoint where it LAST was, and then the picture moves with the
    * midpoint -- together: the image pixel that was under the fingers
    * stays under them, so two fingers moved at a constant distance drag
    * the picture as common viewers do. Refused scales (0, NaN) leave the
    * zoom as it is (the pan still follows); a non-finite midpoint makes
    * the whole update a no-op (hx0: GTK's report is broken, so neither
    * half of it is trusted). */
   if (!isfinite(d_cx) || !isfinite(d_cy)) {
      return;
   }
   if (_pinch_in_fit_detent(p_viewer, d_scale)) {
      /* A two-finger pan over a fitted picture: GtkGestureZoom reports a
       * scale a little off 1 on every move, and zooming by it would turn
       * fit off for good (`0`, a resize refit, a swipe all read b_fit).
       * The picture fits, so it stays fitted and holds still -- also
       * after a pinch out and back into the band, which returns to fit
       * where the picture is (_pinch_hold_fit: zoom AND position are
       * continuous; at the band's edge the rebased zoom below IS the fit
       * zoom). The midpoint is still tracked, so a real zoom that follows
       * starts from here. */
      _pinch_hold_fit(p_viewer);
      p_viewer->d_pinch_last_cx = d_cx;
      p_viewer->d_pinch_last_cy = d_cy;
      return;
   }
   /* Out of the detent: zoom from its edge, not from 1, or the first
    * frame outside it would jump from fit straight to 1.1x fit. */
   if (p_viewer->t_pinch0.b_fit) {
      d_scale = gesture_math_detent_scale(d_scale);
   }
   gdouble d_zoom;
   if (gesture_math_pinch_zoom(p_viewer->d_pinch_zoom0, d_scale, &d_zoom)) {
      _zoom_at(p_viewer, p_viewer->d_pinch_last_cx, p_viewer->d_pinch_last_cy,
               d_zoom);
   }
   _pinch_follow(p_viewer, d_cx, d_cy);
}

gboolean
ggaze_viewer_pinch_end(GgazeViewer *p_viewer) {
   g_return_val_if_fail(GGAZE_IS_VIEWER(p_viewer), FALSE);
   if (!p_viewer->b_pinching) {
      return (FALSE);
   }
   gboolean b_from_tool        = p_viewer->b_pinch_from_tool;
   p_viewer->b_pinching        = FALSE;
   p_viewer->b_pinch_from_tool = FALSE;
   gint64 i_dur = g_get_monotonic_time() - p_viewer->i_pinch_start_us;
   if (!p_viewer->b_pinch_can_tap ||
       !gesture_math_is_two_finger_tap(i_dur, p_viewer->d_pinch_max_move,
                                       p_viewer->d_pinch_max_dev)) {
      return (FALSE);
   }
   /* A tap is not a zoom: the few percent two resting fingers wobble by,
    * and the few px they (or the first finger alone) pan by, are undone,
    * and a fitted view stays fitted (so a later resize still refits it). */
   _view_restore(p_viewer, &p_viewer->t_pre);
   /* Nor is it an edit: when the first finger was dragging a tool (the
    * crop rectangle's corner), the tool puts back what that finger's
    * jitter did before the CANCEL (viewer.h, DRAG_REVERT). The point is
    * where that drag last was. */
   if (b_from_tool && p_viewer->fn_drag != NULL) {
      p_viewer->fn_drag(GGAZE_VIEWER_DRAG_REVERT,
                        p_viewer->d_drag_start_x + p_viewer->d_drag_dx,
                        p_viewer->d_drag_start_y + p_viewer->d_drag_dy,
                        p_viewer->p_overlay_data);
   }
   g_signal_emit(p_viewer, u_toggle_info_sig, 0);
   return (TRUE);
}

void
ggaze_viewer_swipe_track(GgazeViewer *p_viewer, gboolean b_down, gdouble d_x,
                         gdouble d_y) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   if (b_down) {
      p_viewer->d_swipe_x0 = d_x;
      p_viewer->d_swipe_y0 = d_y;
      /* A pinch already in progress spoils the swipe: its first finger
       * is this one. */
      p_viewer->b_swipe_spoiled = p_viewer->b_pinching;
   }
   p_viewer->d_swipe_x = d_x;
   p_viewer->d_swipe_y = d_y;
}

void
ggaze_viewer_spoil_swipe(GgazeViewer *p_viewer) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   /* ggaze_viewer_swipe_track resets it on the next finger down. */
   p_viewer->b_swipe_spoiled = TRUE;
}

/* TRUE when a one-finger horizontal drag pans the picture: zoomed in past
 * the widget's width. A swipe is then a pan, never a page turn. */
static gboolean
_pans_horizontally(GgazeViewer *p_v) {
   if (p_v->b_fit) {
      return (FALSE);
   }
   gdouble d_dw = 0.0;
   int     i_w  = gtk_widget_get_width(GTK_WIDGET(p_v));
   _compute_geom(p_v, i_w, gtk_widget_get_height(GTK_WIDGET(p_v)), NULL, NULL,
                 NULL, &d_dw, NULL);
   return (d_dw > (gdouble)i_w + 0.5);
}

gint
ggaze_viewer_swipe(GgazeViewer *p_viewer, gdouble d_dx, gdouble d_dy,
                   gdouble d_vx, gdouble d_vy) {
   g_return_val_if_fail(GGAZE_IS_VIEWER(p_viewer), 0);
   /* Refused: nothing shown; a crop / straighten tool is up (its overlay
    * owns every drag, and navigating would abandon the tool); or the
    * finger was panning a zoomed-in picture. */
   if (p_viewer->p_texture == NULL || p_viewer->fn_drag != NULL ||
       _pans_horizontally(p_viewer)) {
      return (0);
   }
   gint i_dir = gesture_math_swipe_direction(d_dx, d_dy, d_vx, d_vy);
   if (i_dir != 0) {
      g_signal_emit(p_viewer, u_navigate_sig, 0, i_dir);
   }
   return (i_dir);
}
