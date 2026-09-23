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
 * is always a still -- the first frame -- and may carry a
 * GdkPixbufAnimation (animation_lookup, loader/animation.h). Playback is
 * this widget's alone: a GdkPixbufAnimationIter plus one pending timeout
 * per frame, each frame converted to a GdkTexture on the main thread and
 * drawn in place of the first frame at the SAME geometry (frames are the
 * canvas size), so zoom, pan, the fit ratio, the tool overlay's geometry
 * and ggaze_viewer_get_texture() all keep describing the first frame. It
 * runs only while the widget is mapped (map/unmap), so a grid view or an
 * unpresented window costs no frames, and it restarts from the first
 * frame on every set_texture, remap and hold-Space release, the same way
 * zoom resets. The decode itself is the loader's (worker thread); the
 * per-frame work here is a composition + texture upload, and with a
 * glycin-backed gdk-pixbuf the first pass over the frames fetches each
 * from the sandbox on demand (measured 1-2 ms for 640x480), cached after.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "viewer.h"

#include <math.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <graphene.h>

#include "loader/animation.h"
#include "loader/pixbuf-util.h"
#include "settings.h"

#define GGAZE_ZOOM_FACTOR 1.25
#define GGAZE_ZOOM_MIN 0.02
#define GGAZE_ZOOM_MAX 64.0

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
    * to p_texture, or NULL for a still; the other three are non-NULL /
    * non-zero only while playing. */
   GdkPixbufAnimation     *p_anim;
   GdkPixbufAnimationIter *p_iter;         /* where the playback is */
   GdkTexture             *p_frame;        /* drawn instead of p_texture */
   guint                   u_frame_source; /* the pending frame timeout */
};

G_DEFINE_TYPE(GgazeViewer, ggaze_viewer, GTK_TYPE_WIDGET)

static guint u_navigate_sig = 0;

/* --- animation playback -------------------------------------------------- */

/* gdk-pixbuf 2.44 deprecates the GdkPixbufAnimation API for glycin's,
 * which CI's fedora:40 (2.42) does not have and ggaze does not depend on;
 * the deprecated calls are the portable ones (see pixbuf-util.c), so this
 * section silences their warnings. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

/* Stop playing: no pending timeout, no iterator. The frame on screen
 * stays until the next start or set_texture replaces it, so an unmap does
 * not flash the first frame back. */
static void
_anim_stop(GgazeViewer *p_v) {
   g_clear_handle_id(&p_v->u_frame_source, g_source_remove);
   g_clear_object(&p_v->p_iter);
}

/* Draw the iterator's current frame from now on. A frame the conversion
 * cannot wrap (an empty pixbuf) leaves the previous one up. */
static void
_anim_show_current_frame(GgazeViewer *p_v) {
   GdkPixbuf  *p_pix = gdk_pixbuf_animation_iter_get_pixbuf(p_v->p_iter);
   GdkTexture *p_tex = (p_pix != NULL) ? pixbuf_util_to_texture(p_pix) : NULL;
   if (p_tex != NULL) {
      g_set_object(&p_v->p_frame, p_tex);
      g_object_unref(p_tex);
   }
   gtk_widget_queue_draw(GTK_WIDGET(p_v));
}

static gboolean _anim_tick_cb(gpointer p_data);

/* Arm the timeout for the next frame, or none when the animation has
 * ended on this frame (a GIF without a loop: the last frame holds). */
static void
_anim_schedule(GgazeViewer *p_v) {
   gint i_delay = animation_frame_delay_ms(
      gdk_pixbuf_animation_iter_get_delay_time(p_v->p_iter));
   if (i_delay < 0) {
      return;
   }
   p_v->u_frame_source = g_timeout_add((guint)i_delay, _anim_tick_cb, p_v);
}

/* One frame period elapsed: advance to whatever frame is due now (the
 * iterator skips ahead if the main loop was held up) and re-arm. */
static gboolean
_anim_tick_cb(gpointer p_data) {
   GgazeViewer *p_v    = GGAZE_VIEWER(p_data);
   p_v->u_frame_source = 0;
   if (gdk_pixbuf_animation_iter_advance(p_v->p_iter, NULL)) {
      _anim_show_current_frame(p_v);
   }
   _anim_schedule(p_v);
   return (G_SOURCE_REMOVE);
}

/* Start from the first frame if there is an animation and nothing is
 * playing yet. */
static void
_anim_start(GgazeViewer *p_v) {
   if (p_v->p_anim == NULL || p_v->p_iter != NULL) {
      return;
   }
   p_v->p_iter = gdk_pixbuf_animation_get_iter(p_v->p_anim, NULL);
   _anim_show_current_frame(p_v);
   _anim_schedule(p_v);
}

G_GNUC_END_IGNORE_DEPRECATIONS

/* --- geometry --------------------------------------------------------------
 */

static int
_tex_w(GgazeViewer *p_v) {
   return (p_v->p_texture != NULL) ? gdk_texture_get_width(p_v->p_texture) : 0;
}

static int
_tex_h(GgazeViewer *p_v) {
   return (p_v->p_texture != NULL) ? gdk_texture_get_height(p_v->p_texture) : 0;
}

/* The fit-to-window ratio for the given allocation: the largest scale that
 * shows the whole image. 1.0 when either the texture or the allocation has no
 * size yet, so callers never divide by zero.
 *
 * Note this is NOT bounded by GGAZE_ZOOM_MAX -- an image small enough relative
 * to the window fits at far more than 6400% (a 6x3 image in a 600x400 window
 * fits at 100x). _zoom_at relies on that being expressible; see the ceiling it
 * derives from this (jx0). */
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
 * image pixel. _compute_geom clamps on the next draw. */
static void
_zoom_at(GgazeViewer *p_v, gdouble d_cx, gdouble d_cy, gdouble d_new_zoom) {
   if (p_v->p_texture == NULL) {
      return;
   }
   /* Reject a non-finite centre or zoom instead of storing it (hx0). CLAMP
    * cannot filter NaN -- both of its comparisons are false, so NaN passes
    * straight through -- and a single NaN reaching d_pan_x/d_pan_y is not a
    * one-frame glitch: it makes the draw rect NaN, the image disappears, and
    * because every later zoom derives the new pan from the old one the widget
    * never recovers. Bailing keeps the last good geometry on screen. */
   if (!isfinite(d_cx) || !isfinite(d_cy) || !isfinite(d_new_zoom)) {
      return;
   }
   int i_w = gtk_widget_get_width(GTK_WIDGET(p_v));
   int i_h = gtk_widget_get_height(GTK_WIDGET(p_v));

   /* Ceiling is the NORMAL limit or the fit ratio, whichever is larger (jx0).
    * Clamping to GGAZE_ZOOM_MAX alone is wrong whenever fit-to-window already
    * exceeds it -- a small image in a big window fits at 100x -- because then
    * the very first zoom-in clamped 100 -> 64 and made the picture SMALLER,
    * the opposite of what was asked. Letting the ceiling rise to the fit ratio
    * turns that into a no-op at the top end instead of a reversal, while
    * keeping the normal 6400% limit for every image that fits below it. */
   gdouble d_max = MAX(GGAZE_ZOOM_MAX, _fit_scale(p_v, i_w, i_h));
   d_new_zoom    = CLAMP(d_new_zoom, GGAZE_ZOOM_MIN, d_max);

   gdouble s_old;
   gdouble x_old, y_old;
   _compute_geom(p_v, i_w, i_h, &s_old, &x_old, &y_old, NULL, NULL);

   /* Image-space pixel under the cursor before zoom. */
   gdouble img_x = (s_old > 0.0) ? (d_cx - x_old) / s_old : 0.0;
   gdouble img_y = (s_old > 0.0) ? (d_cy - y_old) / s_old : 0.0;

   p_v->b_fit  = FALSE;
   p_v->d_zoom = d_new_zoom;

   gdouble s_new  = d_new_zoom;
   gdouble want_x = d_cx - img_x * s_new;
   gdouble want_y = d_cy - img_y * s_new;
   p_v->d_pan_x = want_x - ((gdouble)i_w - (gdouble)_tex_w(p_v) * s_new) / 2.0;
   p_v->d_pan_y = want_y - ((gdouble)i_h - (gdouble)_tex_h(p_v) * s_new) / 2.0;

   gtk_widget_queue_draw(GTK_WIDGET(p_v));
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

static void
ggaze_viewer_snapshot(GtkWidget *p_widget, GtkSnapshot *p_snap) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_widget);
   int          i_w = gtk_widget_get_width(p_widget);
   int          i_h = gtk_widget_get_height(p_widget);

   /* Dark background (configurable via settings, applied by the window). */
   GdkRGBA bg;
   switch (p_v->e_bg) {
   case GGAZE_BG_BLACK:
      bg = (GdkRGBA){0.0f, 0.0f, 0.0f, 1.0f};
      break;
   case GGAZE_BG_GREY:
      bg = (GdkRGBA){0.2f, 0.2f, 0.2f, 1.0f};
      break;
   case GGAZE_BG_CHECKER:
      /* Flat mid-grey placeholder; a real checkerboard pattern would be drawn
       * here. Keeps the background distinct from dark/grey for now. */
      bg = (GdkRGBA){0.3f, 0.3f, 0.3f, 1.0f};
      break;
   case GGAZE_BG_DARK:
   default:
      bg = (GdkRGBA){0.07f, 0.07f, 0.07f, 1.0f};
      break;
   }
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
   gtk_snapshot_append_texture(
      p_snap, p_v->p_frame != NULL ? p_v->p_frame : p_v->p_texture, &rect);
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
   GTK_WIDGET_CLASS(ggaze_viewer_parent_class)->unmap(p_widget);
}

static void
ggaze_viewer_dispose(GObject *p_obj) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_obj);
   _anim_stop(p_v); /* before the widget goes: the timeout borrows it */
   g_clear_object(&p_v->p_frame);
   g_clear_object(&p_v->p_anim);
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
   /* "navigate": emitted by the scroll wheel in GGAZE_SCROLL_NAVIGATE mode.
    * The int arg is +1 (next) or -1 (prev). */
   u_navigate_sig =
      g_signal_new("navigate", G_OBJECT_CLASS_TYPE(p_oc), G_SIGNAL_RUN_LAST, 0,
                   NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

/* --- controllers ---------------------------------------------------------- */

/* A drag either pans the image or, while a tool overlay is installed, is
 * handed to the tool in absolute widget coordinates (GtkGestureDrag reports
 * offsets from the start point; the tool wants positions). Which of the two
 * it is gets decided at BEGIN and holds for the whole gesture: a tool that
 * starts mid-drag never sees an UPDATE without its BEGIN, and a tool that
 * goes away mid-drag (Esc while dragging the rectangle) hands the rest of
 * the gesture to panning from where the pointer is NOW -- the pan origin is
 * re-based at that moment, so the first pan step is not the whole offset
 * accumulated since the press. */
static void
_drag_begin_cb(GtkGestureDrag *p_gesture, gdouble d_x, gdouble d_y,
               gpointer p_data) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_data);
   (void)p_gesture;
   p_v->d_drag_start_x     = d_x;
   p_v->d_drag_start_y     = d_y;
   p_v->d_drag_start_pan_x = p_v->d_pan_x;
   p_v->d_drag_start_pan_y = p_v->d_pan_y;
   p_v->b_drag_to_overlay  = (p_v->fn_drag != NULL);
   if (p_v->b_drag_to_overlay) {
      p_v->fn_drag(GGAZE_VIEWER_DRAG_BEGIN, d_x, d_y, p_v->p_overlay_data);
   }
}

static void
_drag_update_cb(GtkGestureDrag *p_gesture, gdouble d_dx, gdouble d_dy,
                gpointer p_data) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_data);
   (void)p_gesture;
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
   if (p_v->b_drag_to_overlay && p_v->fn_drag != NULL) {
      p_v->fn_drag(GGAZE_VIEWER_DRAG_END, p_v->d_drag_start_x + d_dx,
                   p_v->d_drag_start_y + d_dy, p_v->p_overlay_data);
   }
   p_v->b_drag_to_overlay = FALSE;
}

/* The zoom centre for a scroll event: the pointer position translated into
 * widget space when the event carries one (finite), else the widget centre.
 * See the hx0 note in _scroll_cb for why the fallback is mandatory. */
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

static gboolean
_scroll_cb(GtkEventControllerScroll *p_scroll, gdouble d_dx, gdouble d_dy,
           gpointer p_data) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_data);
   if (p_v->p_texture == NULL) {
      return (FALSE);
   }
   (void)d_dx;

   /* Zoom centre: the pointer if the event carries a usable position, else the
    * widget centre.
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
   gdouble d_cx, d_cy;
   _event_zoom_centre(p_v, GTK_EVENT_CONTROLLER(p_scroll), &d_cx, &d_cy);

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

   GtkGesture *p_drag = gtk_gesture_drag_new();
   gtk_widget_add_controller(GTK_WIDGET(p_v), GTK_EVENT_CONTROLLER(p_drag));
   g_signal_connect(p_drag, "drag-begin", G_CALLBACK(_drag_begin_cb), p_v);
   g_signal_connect(p_drag, "drag-update", G_CALLBACK(_drag_update_cb), p_v);
   g_signal_connect(p_drag, "drag-end", G_CALLBACK(_drag_end_cb), p_v);

   GtkEventController *p_scroll =
      gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
   gtk_widget_add_controller(GTK_WIDGET(p_v), p_scroll);
   g_signal_connect(p_scroll, "scroll", G_CALLBACK(_scroll_cb), p_v);
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
   g_clear_object(&p_viewer->p_frame);
   g_set_object(&p_viewer->p_texture, p_texture);
   g_set_object(&p_viewer->p_anim, animation_lookup(p_texture));
   p_viewer->b_fit   = TRUE;
   p_viewer->d_zoom  = 1.0;
   p_viewer->d_pan_x = 0.0;
   p_viewer->d_pan_y = 0.0;
   if (gtk_widget_get_mapped(GTK_WIDGET(p_viewer))) {
      _anim_start(p_viewer);
   }
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
   return (p_viewer->p_frame != NULL ? p_viewer->p_frame : p_viewer->p_texture);
}

gboolean
ggaze_viewer_is_animating(GgazeViewer *p_viewer) {
   g_return_val_if_fail(GGAZE_IS_VIEWER(p_viewer), FALSE);
   return (p_viewer->u_frame_source != 0);
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
