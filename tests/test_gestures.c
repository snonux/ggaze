/*:*
 * ggaze — touch gestures on the large view (integration, task zb2)
 *
 * Pinch to zoom, swipe for next / previous, two-finger tap for the info
 * card, on a real, presented GgazeWindow. GTK4 cannot synthesise touch
 * events, so the gestures are driven two ways:
 *
 *   - through the viewer's public gesture bodies (viewer.h "touch
 *     gestures": ggaze_viewer_pinch_begin/update/end, ggaze_viewer_swipe),
 *     which is what the GtkGestureZoom / GtkGestureSwipe handlers call
 *     with the gesture's own points -- this is where the chosen midpoints
 *     and velocities go;
 *   - by emitting the gestures' own signals on the controllers the viewer
 *     installed (as test_viewer.c does for the drag gesture), which runs
 *     the real handlers: GtkGestureZoom "begin" / "scale-changed" /
 *     "cancel" / "end" (no points: the widget centre is the midpoint) and
 *     GtkGestureSwipe "begin" / "swipe", the latter over a path laid down
 *     with ggaze_viewer_swipe_track (what the swipe's "begin" / "update"
 *     handlers call with the finger's point).
 *
 * What is asserted: a pinch zooms about its midpoint (the image pixel under
 * it stays put) and pans with it when the midpoint moves -- also at a
 * constant finger distance -- with the wheel's clamp and NaN / Inf /
 * zero-scale guards; a pinch takes a one-finger tool drag away with a
 * CANCEL where the finger was, stops a pan drag where it got to, and
 * swallows the rest of either; a leftward swipe shows the next image and a
 * rightward one the previous, while a vertical, tiny or zero-velocity
 * swipe, a swipe over a zoomed-in picture, a swipe with a tool overlay
 * installed and a swipe a pinch or a slideshow step spoiled navigate
 * nowhere; a swipe or a
 * navigate-mode wheel notch stops a running slideshow, as l / h do; a
 * pinch over a fitted picture holds it at fit within the tap's wobble and
 * zooms on continuously (no jump) past that band; a
 * two-finger tap toggles the info card and leaves the view as it was
 * before its first finger went down, a long or moving one, a touchpad
 * pinch and one cut short by a new texture or an unmap do not. The
 * dirty-gate and real-tool halves (a swipe away from an enhance preview
 * prompts, a crop tool refuses a swipe, a pinch in the straighten tool
 * levels nothing) need GEGL and live in test_enhance_flow.c. Not reachable
 * here: the drag gesture's DENIED claim at pinch begin, which needs a real
 * touch sequence (docs/IMPLEMENTATION.md, touch gestures).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "window.h"

#include "gesture-math.h"
#include "gtk_helpers.h"
#include "settings.h"
#include "temp_dir.h"
#include "viewer.h"
#include "wait_until.h"

#include <math.h>

#include <gio/gio.h>
#include <gtk/gtk.h>

/* Two sizes, so "the next image is on screen" is a texture-size wait. Both
 * are larger than the 600x400 window: fit is well below 1.0 and far below
 * the 6400 % ceiling. */
#define A_W 1200
#define A_H 800
#define B_W 1000
#define B_H 800

/* The zoom factor a pinch that began over a FITTED picture ends up at for
 * a GtkGestureZoom scale d_s past the fit detent's upper edge: measured
 * from that edge (gesture_math_detent_scale), not from 1. */
#define DETENT_OUT(d_s) ((d_s) / (1.0 + GESTURE_TAP_MAX_SCALE_DEV))

/* 2 s (scaled) for a card or a navigation to land. */
#define WAIT_US (2 * G_TIME_SPAN_SECOND)

typedef struct {
   char        *c_dir;
   GgazeWindow *p_win;
   GgazeViewer *p_viewer;
} GestureFx;

static void
write_png(const char *c_dir, const char *c_name, int i_w, int i_h,
          guint32 u_rgba) {
   GError    *p_err = NULL;
   GdkPixbuf *p_pix = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, i_w, i_h);
   g_assert_nonnull(p_pix);
   gdk_pixbuf_fill(p_pix, u_rgba);
   char *c_path = g_build_filename(c_dir, c_name, NULL);
   g_assert_true(gdk_pixbuf_save(p_pix, c_path, "png", &p_err, NULL));
   g_assert_no_error(p_err);
   g_free(c_path);
   g_object_unref(p_pix);
}

/* A presented 600x400 window on a.png of a folder holding a.png and b.png,
 * waited until a.png is on screen in a settled allocation (gtk_helpers.h
 * "large-view readiness"), since every assertion reads scale or geometry. */
static void
fx_open(GestureFx *p_fx) {
   GError *p_err = NULL;
   p_fx->c_dir   = g_dir_make_tmp("ggaze-gestures-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   write_png(p_fx->c_dir, "a.png", A_W, A_H, 0x336699ffu);
   write_png(p_fx->c_dir, "b.png", B_W, B_H, 0x993366ffu);
   char  *c_a    = g_build_filename(p_fx->c_dir, "a.png", NULL);
   GFile *p_file = g_file_new_for_path(c_a);
   p_fx->p_win   = GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL));
   gtk_window_set_default_size(GTK_WINDOW(p_fx->p_win), 600, 400);
   gtk_window_present(GTK_WINDOW(p_fx->p_win));
   ggaze_window_open(p_fx->p_win, p_file);
   p_fx->p_viewer = GGTEST_WAIT_FOR_VIEW(p_fx->p_win, A_W, A_H);
   g_object_unref(p_file);
   g_free(c_a);
}

static void
fx_close(GestureFx *p_fx) {
   gtk_window_destroy(GTK_WINDOW(p_fx->p_win));
   ggtest_drain_main(200);
   ggtest_cleanup_temp_dir(p_fx->c_dir);
}

/* The image pixel under widget point (d_x, d_y) as the viewer draws now. */
static void
image_point_at(GgazeViewer *p_v, gdouble d_x, gdouble d_y, gdouble *p_ix,
               gdouble *p_iy) {
   GgazeViewerGeom t_geom;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &t_geom));
   *p_ix = (d_x - t_geom.d_x) / t_geom.d_scale;
   *p_iy = (d_y - t_geom.d_y) / t_geom.d_scale;
}

static void
assert_pan_finite(GgazeViewer *p_v) {
   gdouble d_px, d_py;
   ggaze_viewer_get_pan(p_v, &d_px, &d_py);
   g_assert_true(isfinite(d_px) && isfinite(d_py));
}

/* The first controller of p_type the viewer installed (borrowed). */
static GtkEventController *
controller_of(GgazeViewer *p_v, GType p_type) {
   GListModel *p_ctrls = gtk_widget_observe_controllers(GTK_WIDGET(p_v));
   GtkEventController *p_found = NULL;
   for (guint i = 0; i < g_list_model_get_n_items(p_ctrls); i++) {
      GtkEventController *p_c = g_list_model_get_item(p_ctrls, i);
      if (G_TYPE_CHECK_INSTANCE_TYPE(p_c, p_type) && p_found == NULL) {
         p_found = p_c; /* borrowed: the widget owns it */
      }
      g_object_unref(p_c);
   }
   g_object_unref(p_ctrls);
   g_assert_nonnull(p_found);
   return (p_found);
}

/* --- pinch --------------------------------------------------------------- */

/* A pinch at an off-centre midpoint zooms and keeps the image pixel under
 * the midpoint where it was (cursor-centred like the wheel,
 * docs/ui-and-interactions.md "Zoom behavior"); the update is absolute
 * from the begin scale (measured from the fit detent's edge, as the pinch
 * began fitted), and a moved midpoint zooms about the new one. */
static void
test_pinch_zooms_about_the_midpoint(void) {
   GestureFx fx;
   fx_open(&fx);
   gdouble d_fit = ggaze_viewer_get_scale(fx.p_viewer);
   gdouble d_ix0, d_iy0, d_ix1, d_iy1;
   image_point_at(fx.p_viewer, 150.0, 120.0, &d_ix0, &d_iy0);
   ggaze_viewer_pinch_begin(fx.p_viewer, 150.0, 120.0, FALSE);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.5, 150.0, 120.0);
   ggaze_viewer_pinch_update(fx.p_viewer, 2.0, 150.0, 120.0);
   g_assert_cmpfloat(
      fabs(ggaze_viewer_get_scale(fx.p_viewer) - DETENT_OUT(2.0) * d_fit), <,
      1e-9);
   image_point_at(fx.p_viewer, 150.0, 120.0, &d_ix1, &d_iy1);
   g_assert_cmpfloat(fabs(d_ix1 - d_ix0), <, 1e-6);
   g_assert_cmpfloat(fabs(d_iy1 - d_iy0), <, 1e-6);
   /* Midpoint moved while zooming: the pixel that was under the fingers
    * follows them to where they are now (no pan clamp at this scale). */
   ggaze_viewer_pinch_update(fx.p_viewer, 2.5, 100.0, 100.0);
   image_point_at(fx.p_viewer, 100.0, 100.0, &d_ix1, &d_iy1);
   g_assert_cmpfloat(fabs(d_ix1 - d_ix0), <, 1e-6);
   g_assert_cmpfloat(fabs(d_iy1 - d_iy0), <, 1e-6);
   /* A real pinch, not a tap: no info card toggle, the zoom stays. */
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   g_assert_cmpfloat(
      fabs(ggaze_viewer_get_scale(fx.p_viewer) - DETENT_OUT(2.5) * d_fit), <,
      1e-9);
   fx_close(&fx);
}

/* Two fingers moved together at a constant distance (scale 1) drag the
 * picture by the midpoint's movement, as common viewers do; a non-finite
 * midpoint moves nothing. */
static void
test_pinch_pans_with_the_midpoint(void) {
   GestureFx fx;
   fx_open(&fx);
   ggaze_viewer_toggle_fit_100(fx.p_viewer); /* 1200x800: room to pan */
   gdouble d_px, d_py;
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.0, 350.0, 230.0);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.0, 330.0, 240.0);
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(fabs(d_px - 30.0), <, 1e-9);
   g_assert_cmpfloat(fabs(d_py - 40.0), <, 1e-9);
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, 1.0);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.0, NAN, 240.0);
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(fabs(d_px - 30.0), <, 1e-9);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer)); /* it moved */
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(fabs(d_px - 30.0), <, 1e-9);
   fx_close(&fx);
}

/* A two-finger pan over a FITTED picture (GtkGestureZoom reports a scale
 * a hair off 1 on every move) keeps it fitted -- it has nothing to pan --
 * so `0` still goes to 100 % and a swipe still turns the page (zoomed by
 * 1.01 it would be "wider than the widget" and refused). A pinch out and
 * back within the same wobble returns to fit too (zb2 second review). */
static void
test_two_finger_pan_keeps_fit(void) {
   GestureFx fx;
   fx_open(&fx);
   gdouble d_fit = ggaze_viewer_get_scale(fx.p_viewer);
   gdouble d_px, d_py;
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.01, 360.0, 205.0);
   ggaze_viewer_pinch_update(fx.p_viewer, 0.99, 420.0, 210.0);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer)); /* it moved */
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, d_fit);
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(d_px, ==, 0.0);
   g_assert_cmpfloat(d_py, ==, 0.0);
   /* Out to 1.5x and back to 1.02x: fitted again, not 1.02 x fit. */
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.5, 300.0, 200.0);
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), >, d_fit);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.02, 340.0, 200.0);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, d_fit);
   /* Still fitted: the swipe navigates. */
   g_assert_cmpint(ggaze_viewer_swipe(fx.p_viewer, -200.0, 10.0, -900.0, 0.0),
                   ==, 1);
   GGTEST_WAIT_FOR_TEXTURE(fx.p_win, B_W, B_H);
   fx_close(&fx);
}

/* Leaving the fit detent is continuous (zb2 third review): the first
 * frame just past either edge of the band is the fit zoom itself, not
 * 1.1x (or 0.9x) fit, and further out the zoom grows from that edge.
 * Pinching back into the band snaps to fit, seamlessly. */
static void
test_leaving_the_fit_detent_does_not_jump(void) {
   GestureFx fx;
   fx_open(&fx);
   gdouble       d_fit  = ggaze_viewer_get_scale(fx.p_viewer);
   const gdouble d_edge = 1.0 + GESTURE_TAP_MAX_SCALE_DEV;
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   ggaze_viewer_pinch_update(fx.p_viewer, d_edge + 1e-6, 300.0, 200.0);
   gdouble d_first = ggaze_viewer_get_scale(fx.p_viewer);
   g_assert_cmpfloat(d_first, >, d_fit);
   g_assert_cmpfloat(fabs(d_first / d_fit - 1.0), <, 1e-5);
   ggaze_viewer_pinch_update(fx.p_viewer, 2.0 * d_edge, 300.0, 200.0);
   g_assert_cmpfloat(fabs(ggaze_viewer_get_scale(fx.p_viewer) - 2.0 * d_fit), <,
                     1e-9);
   ggaze_viewer_pinch_update(fx.p_viewer, d_edge - 1e-6, 300.0, 200.0);
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, d_fit);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   /* Pinching in: the lower edge, symmetrically. */
   const gdouble d_low = 1.0 - GESTURE_TAP_MAX_SCALE_DEV;
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   ggaze_viewer_pinch_update(fx.p_viewer, d_low - 1e-6, 300.0, 200.0);
   d_first = ggaze_viewer_get_scale(fx.p_viewer);
   g_assert_cmpfloat(d_first, <, d_fit);
   g_assert_cmpfloat(fabs(d_first / d_fit - 1.0), <, 1e-5);
   ggaze_viewer_pinch_update(fx.p_viewer, 0.5 * d_low, 300.0, 200.0);
   g_assert_cmpfloat(fabs(ggaze_viewer_get_scale(fx.p_viewer) - 0.5 * d_fit), <,
                     1e-9);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   /* Negative: a pinch that began zoomed (not fitted) has no detent and
    * zooms by the raw scale from its first frame. At 0.5x fit now:
    * the first toggle fits, the second goes to 100 %. */
   ggaze_viewer_toggle_fit_100(fx.p_viewer);
   ggaze_viewer_toggle_fit_100(fx.p_viewer);
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, 1.0);
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.05, 300.0, 200.0);
   g_assert_cmpfloat(fabs(ggaze_viewer_get_scale(fx.p_viewer) - 1.05), <, 1e-9);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.5, 300.0, 200.0); /* no tap */
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   fx_close(&fx);
}

/* Pinching further out at the 6400 % ceiling zooms no more, but the picture
 * still moves with the fingers: the image pixel under the midpoint stays
 * under it as the midpoint moves (zb2 second review). */
static void
test_pinch_at_max_zoom_still_pans(void) {
   GestureFx fx;
   fx_open(&fx);
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   ggaze_viewer_pinch_update(fx.p_viewer, 1e9, 300.0, 200.0);
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, GGAZE_ZOOM_MAX);
   gdouble d_ix0, d_iy0, d_ix, d_iy, d_px0, d_py0, d_px, d_py;
   image_point_at(fx.p_viewer, 300.0, 200.0, &d_ix0, &d_iy0);
   ggaze_viewer_get_pan(fx.p_viewer, &d_px0, &d_py0);
   ggaze_viewer_pinch_update(fx.p_viewer, 2e9, 330.0, 215.0);
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, GGAZE_ZOOM_MAX);
   image_point_at(fx.p_viewer, 330.0, 215.0, &d_ix, &d_iy);
   g_assert_cmpfloat(fabs(d_ix - d_ix0), <, 1e-6);
   g_assert_cmpfloat(fabs(d_iy - d_iy0), <, 1e-6);
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(fabs(d_px - d_px0 - 30.0), <, 1e-6);
   g_assert_cmpfloat(fabs(d_py - d_py0 - 15.0), <, 1e-6);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   fx_close(&fx);
}

/* The wheel's clamp (2 %..6400 %) and its hx0 guards apply to a pinch:
 * NaN / Inf / zero / negative scales and a non-finite midpoint change
 * nothing, and an update or end without a begin is ignored. */
static void
test_pinch_clamps_and_guards(void) {
   GestureFx fx;
   fx_open(&fx);
   gdouble d_fit = ggaze_viewer_get_scale(fx.p_viewer);
   ggaze_viewer_pinch_update(fx.p_viewer, 2.0, 10.0, 10.0); /* no begin */
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, d_fit);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   ggaze_viewer_pinch_update(fx.p_viewer, 1e9, 300.0, 200.0);
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, GGAZE_ZOOM_MAX);
   ggaze_viewer_pinch_update(fx.p_viewer, 1e-9, 300.0, 200.0);
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, GGAZE_ZOOM_MIN);
   ggaze_viewer_pinch_update(fx.p_viewer, 2.0, 300.0, 200.0);
   gdouble       d_good  = ggaze_viewer_get_scale(fx.p_viewer);
   const gdouble c_bad[] = {NAN, INFINITY, -INFINITY, 0.0, -1.0};
   for (guint u = 0; u < G_N_ELEMENTS(c_bad); u++) {
      ggaze_viewer_pinch_update(fx.p_viewer, c_bad[u], 300.0, 200.0);
      g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, d_good);
      ggaze_viewer_pinch_update(fx.p_viewer, 3.0, NAN, 200.0);
      ggaze_viewer_pinch_update(fx.p_viewer, 3.0, 300.0, INFINITY);
      g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, d_good);
      assert_pan_finite(fx.p_viewer);
   }
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer)); /* it zoomed */
   fx_close(&fx);
}

static gboolean
has_handler(gpointer p_obj, const char *c_signal) {
   guint u_id = g_signal_lookup(c_signal, G_OBJECT_TYPE(p_obj));
   g_assert_cmpuint(u_id, !=, 0);
   return (g_signal_has_handler_pending(p_obj, u_id, 0, FALSE));
}

static gboolean
info_visible(gpointer p_data) {
   return (gtk_widget_get_visible(ggaze_window_get_info_label(p_data)));
}

static gboolean
info_hidden(gpointer p_data) {
   return (!info_visible(p_data));
}

/* The real GtkGestureZoom handlers, driven by the gesture's own signals.
 * With no touches the gesture reports no points, so "begin" pinches about
 * the widget centre: "scale-changed" zooms about it (the image pixel there
 * stays), "end" closes the pinch -- no tap, it zoomed -- and a later
 * "scale-changed" does nothing. A quick "begin" + "end" is a tap through
 * the handlers and toggles the info card; a "cancel" in between makes it
 * none. */
static void
test_zoom_controller_drives_the_pinch(void) {
   GestureFx fx;
   fx_open(&fx);
   gdouble d_fit = ggaze_viewer_get_scale(fx.p_viewer);
   gdouble d_mx  = gtk_widget_get_width(GTK_WIDGET(fx.p_viewer)) / 2.0;
   gdouble d_my  = gtk_widget_get_height(GTK_WIDGET(fx.p_viewer)) / 2.0;
   GtkEventController *p_zoom =
      controller_of(fx.p_viewer, GTK_TYPE_GESTURE_ZOOM);
   g_assert_true(has_handler(p_zoom, "begin"));
   gdouble d_ix0, d_iy0, d_ix1, d_iy1;
   image_point_at(fx.p_viewer, d_mx, d_my, &d_ix0, &d_iy0);
   g_signal_emit_by_name(p_zoom, "begin", NULL);
   g_signal_emit_by_name(p_zoom, "scale-changed", 2.0);
   g_assert_cmpfloat(
      fabs(ggaze_viewer_get_scale(fx.p_viewer) - DETENT_OUT(2.0) * d_fit), <,
      1e-9);
   image_point_at(fx.p_viewer, d_mx, d_my, &d_ix1, &d_iy1);
   g_assert_cmpfloat(fabs(d_ix1 - d_ix0), <, 1e-6);
   g_assert_cmpfloat(fabs(d_iy1 - d_iy0), <, 1e-6);
   g_signal_emit_by_name(p_zoom, "end", NULL);
   g_signal_emit_by_name(p_zoom, "scale-changed", 3.0);
   g_assert_cmpfloat(
      fabs(ggaze_viewer_get_scale(fx.p_viewer) - DETENT_OUT(2.0) * d_fit), <,
      1e-9);
   ggtest_drain_main(100);
   g_assert_false(info_visible(fx.p_win));
   /* A tap through the handlers: on, and the zoom stays what it was. */
   g_signal_emit_by_name(p_zoom, "begin", NULL);
   g_signal_emit_by_name(p_zoom, "end", NULL);
   g_assert_true(ggtest_wait_until(info_visible, fx.p_win, WAIT_US));
   g_assert_cmpfloat(
      fabs(ggaze_viewer_get_scale(fx.p_viewer) - DETENT_OUT(2.0) * d_fit), <,
      1e-9);
   /* Cancelled: no tap, the card stays on. */
   g_signal_emit_by_name(p_zoom, "begin", NULL);
   g_signal_emit_by_name(p_zoom, "cancel", NULL);
   g_signal_emit_by_name(p_zoom, "end", NULL);
   ggtest_drain_main(200);
   g_assert_true(info_visible(fx.p_win));
   fx_close(&fx);
}

/* The real GtkGestureSwipe "swipe" handler judges the path laid down by
 * ggaze_viewer_swipe_track (what its "begin" / "update" handlers call with
 * the finger's point): a leftward flick shows the next image. A swipe a
 * pinch began during is refused -- and the very same swipe unspoiled
 * navigates, so this fails whichever way the spoiled check is broken. The
 * gesture is touch-only, so a mouse drag can never turn a page, and a
 * "begin" that found no point leaves no path, which is refused. */
static void
test_swipe_controller_navigates_unless_spoiled(void) {
   GestureFx fx;
   fx_open(&fx);
   GtkEventController *p_swipe =
      controller_of(fx.p_viewer, GTK_TYPE_GESTURE_SWIPE);
   g_assert_true(
      gtk_gesture_single_get_touch_only(GTK_GESTURE_SINGLE(p_swipe)));
   g_assert_true(has_handler(p_swipe, "update"));
   g_signal_emit_by_name(p_swipe, "begin", NULL); /* no point: NaN path */
   g_signal_emit_by_name(p_swipe, "swipe", -2000.0, 0.0);
   ggtest_drain_main(100);
   g_assert_nonnull(
      g_strstr_len(gtk_window_get_title(GTK_WINDOW(fx.p_win)), -1, "a.png"));
   /* Spoiled: a pinch (a moving one, so no tap) began under the finger. */
   ggaze_viewer_swipe_track(fx.p_viewer, TRUE, 400.0, 200.0);
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   ggaze_viewer_pinch_update(fx.p_viewer, 0.9, 300.0 + 3 * GESTURE_TAP_MAX_MOVE,
                             200.0);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   ggaze_viewer_swipe_track(fx.p_viewer, FALSE, 150.0, 205.0);
   g_signal_emit_by_name(p_swipe, "swipe", -900.0, 0.0);
   ggtest_drain_main(200);
   g_assert_nonnull(
      g_strstr_len(gtk_window_get_title(GTK_WINDOW(fx.p_win)), -1, "a.png"));
   /* The same swipe with no pinch: next. The pinch left the picture
    * narrower than the widget, so this is no pan. */
   ggaze_viewer_swipe_track(fx.p_viewer, TRUE, 400.0, 200.0);
   ggaze_viewer_swipe_track(fx.p_viewer, FALSE, 150.0, 205.0);
   g_signal_emit_by_name(p_swipe, "swipe", -900.0, 0.0);
   GGTEST_WAIT_FOR_TEXTURE(fx.p_win, B_W, B_H);
   g_assert_nonnull(
      g_strstr_len(gtk_window_get_title(GTK_WINDOW(fx.p_win)), -1, "b.png"));
   fx_close(&fx);
}

typedef struct {
   guint                u_calls;
   GgazeViewerDragPhase e_last;
   gdouble              d_last_x;
} DragLog;

static void
log_drag_cb(GgazeViewerDragPhase e_phase, gdouble d_x, gdouble d_y,
            gpointer p_data) {
   DragLog *p_log = p_data;
   (void)d_y;
   p_log->u_calls++;
   p_log->e_last   = e_phase;
   p_log->d_last_x = d_x;
}

/* A second finger landing during a one-finger tool drag makes a pinch: the
 * tool gets a CANCEL (not an END: the drag was taken away, not finished --
 * viewer.h) where the finger was, and the rest of that drag --
 * updates and the gesture's own end -- reaches neither the tool nor the
 * pan. */
static void
test_pinch_ends_a_tool_drag(void) {
   GestureFx fx;
   fx_open(&fx);
   DragLog t_log = {0};
   ggaze_viewer_set_overlay(fx.p_viewer, NULL, log_drag_cb, &t_log);
   GtkEventController *p_drag =
      controller_of(fx.p_viewer, GTK_TYPE_GESTURE_DRAG);
   g_signal_emit_by_name(p_drag, "drag-begin", 100.0, 100.0);
   g_signal_emit_by_name(p_drag, "drag-update", 20.0, 0.0);
   g_assert_cmpuint(t_log.u_calls, ==, 2);
   ggaze_viewer_pinch_begin(fx.p_viewer, 150.0, 100.0, FALSE);
   g_assert_cmpuint(t_log.u_calls, ==, 3);
   g_assert_cmpint(t_log.e_last, ==, GGAZE_VIEWER_DRAG_CANCEL);
   g_assert_cmpfloat(t_log.d_last_x, ==, 120.0);
   g_signal_emit_by_name(p_drag, "drag-update", 80.0, 0.0);
   g_signal_emit_by_name(p_drag, "drag-end", 80.0, 0.0);
   g_assert_cmpuint(t_log.u_calls, ==, 3);
   ggaze_viewer_pinch_end(fx.p_viewer);
   ggaze_viewer_set_overlay(fx.p_viewer, NULL, NULL, NULL);
   /* The same for a pan drag at 100 % (room to pan): what the drag did
    * before the pinch stays, what it reports after moves nothing. */
   ggaze_viewer_toggle_fit_100(fx.p_viewer);
   gdouble d_px, d_py;
   g_signal_emit_by_name(p_drag, "drag-begin", 100.0, 100.0);
   g_signal_emit_by_name(p_drag, "drag-update", 20.0, 0.0);
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(d_px, ==, 20.0);
   ggaze_viewer_pinch_begin(fx.p_viewer, 150.0, 100.0, FALSE);
   g_signal_emit_by_name(p_drag, "drag-update", 80.0, 0.0);
   g_signal_emit_by_name(p_drag, "drag-end", 80.0, 0.0);
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(d_px, ==, 20.0);
   ggaze_viewer_pinch_end(fx.p_viewer);
   fx_close(&fx);
}

/* --- two-finger tap ------------------------------------------------------ */

/* A short, still two-finger touch toggles the info card (win.info, as `i`
 * does) -- on, then off -- and leaves a fitted view fitted even though the
 * fingers wobbled the scale a little. */
static void
test_two_finger_tap_toggles_info(void) {
   GestureFx fx;
   fx_open(&fx);
   gdouble d_fit = ggaze_viewer_get_scale(fx.p_viewer);
   g_assert_false(info_visible(fx.p_win));
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.05, 303.0, 201.0);
   g_assert_true(ggaze_viewer_pinch_end(fx.p_viewer));
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, d_fit);
   g_assert_true(ggtest_wait_until(info_visible, fx.p_win, WAIT_US));
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   g_assert_true(ggaze_viewer_pinch_end(fx.p_viewer));
   g_assert_true(ggtest_wait_until(info_hidden, fx.p_win, WAIT_US));
   fx_close(&fx);
}

/* Not taps: two fingers that moved, pinched, or stayed down too long. */
static void
test_long_or_moving_two_finger_touch_is_no_tap(void) {
   GestureFx fx;
   fx_open(&fx);
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.0, 300.0 + 3 * GESTURE_TAP_MAX_MOVE,
                             200.0);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.5, 300.0, 200.0);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   g_usleep(GESTURE_TAP_MAX_US + 50 * G_TIME_SPAN_MILLISECOND);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   ggtest_drain_main(200);
   g_assert_false(info_visible(fx.p_win));
   fx_close(&fx);
}

/* A tap is measured from its FIRST finger: the view goes back to what it
 * was before that finger's drag began -- not to the few px it panned
 * before the second finger landed -- and a first finger that wandered
 * far before the second landed makes no tap. */
static void
test_tap_restores_the_view_before_the_first_finger(void) {
   GestureFx fx;
   fx_open(&fx);
   ggaze_viewer_toggle_fit_100(fx.p_viewer); /* room to pan */
   GtkEventController *p_drag =
      controller_of(fx.p_viewer, GTK_TYPE_GESTURE_DRAG);
   gdouble d_px, d_py;
   g_signal_emit_by_name(p_drag, "drag-begin", 100.0, 100.0);
   g_signal_emit_by_name(p_drag, "drag-update", 3.0, 2.0); /* jitter */
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(d_px, ==, 3.0);
   ggaze_viewer_pinch_begin(fx.p_viewer, 150.0, 100.0, FALSE);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.02, 151.0, 101.0);
   g_assert_true(ggaze_viewer_pinch_end(fx.p_viewer));
   g_signal_emit_by_name(p_drag, "drag-end", 3.0, 2.0);
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(d_px, ==, 0.0);
   g_assert_cmpfloat(d_py, ==, 0.0);
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, 1.0);
   g_assert_true(ggtest_wait_until(info_visible, fx.p_win, WAIT_US));
   /* The first finger panned far first: a pan and a pinch, no tap. */
   g_signal_emit_by_name(p_drag, "drag-begin", 100.0, 100.0);
   g_signal_emit_by_name(p_drag, "drag-update", 3 * GESTURE_TAP_MAX_MOVE, 0.0);
   ggaze_viewer_pinch_begin(fx.p_viewer, 150.0, 100.0, FALSE);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   g_signal_emit_by_name(p_drag, "drag-end", 3 * GESTURE_TAP_MAX_MOVE, 0.0);
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(d_px, ==, 3 * GESTURE_TAP_MAX_MOVE);
   ggtest_drain_main(200);
   g_assert_true(info_visible(fx.p_win)); /* not toggled off */
   fx_close(&fx);
}

/* A touchpad pinch (the handler passes b_touchpad from the event type)
 * zooms but is never a tap, however short and still: resting two fingers
 * on a touchpad must not toggle the card. */
static void
test_touchpad_pinch_is_never_a_tap(void) {
   GestureFx fx;
   fx_open(&fx);
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, TRUE);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, TRUE);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.5, 300.0, 200.0);
   gdouble d_zoomed = ggaze_viewer_get_scale(fx.p_viewer);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, d_zoomed);
   ggtest_drain_main(200);
   g_assert_false(info_visible(fx.p_win));
   fx_close(&fx);
}

/* A new texture (the next file, a preview render) ends a pinch in progress:
 * the rest of it neither zooms nor taps, so a restore can never put the
 * old picture's zoom and pan on the new one. An unmap does the same. */
static void
test_new_texture_or_unmap_ends_the_pinch(void) {
   GestureFx fx;
   fx_open(&fx);
   ggaze_viewer_toggle_fit_100(fx.p_viewer);
   ggaze_viewer_pan(fx.p_viewer, 40.0, 0.0);
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   /* A plain B_W x B_H memory texture: the viewer only needs its size. */
   GBytes *p_px =
      g_bytes_new_take(g_malloc0((gsize)B_W * B_H * 4), (gsize)B_W * B_H * 4);
   GdkTexture *p_tex = gdk_memory_texture_new(B_W, B_H, GDK_MEMORY_R8G8B8A8,
                                              p_px, (gsize)B_W * 4);
   g_bytes_unref(p_px);
   ggaze_viewer_set_texture(fx.p_viewer, p_tex);
   gdouble d_fit = ggaze_viewer_get_scale(fx.p_viewer);
   ggaze_viewer_pinch_update(fx.p_viewer, 2.0, 300.0, 200.0);
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, d_fit);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   gdouble d_px, d_py;
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, d_fit);
   g_assert_cmpfloat(d_px, ==, 0.0);
   /* Unmapped mid-pinch: the pinch is over when it comes back. */
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0, FALSE);
   gtk_widget_set_visible(GTK_WIDGET(fx.p_viewer), FALSE);
   g_assert_false(gtk_widget_get_mapped(GTK_WIDGET(fx.p_viewer)));
   gtk_widget_set_visible(GTK_WIDGET(fx.p_viewer), TRUE);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   ggtest_drain_main(200);
   g_assert_false(info_visible(fx.p_win));
   g_object_unref(p_tex);
   fx_close(&fx);
}

/* A two-finger TAP whose first finger was dragging a tool: the tool gets
 * the CANCEL when the second lands and then, as the pinch ends as a tap, a
 * DRAG_REVERT at the same point (viewer.h), so it can undo the jitter. A
 * pinch that is no tap sends none (zb2 second review). */
static void
test_tap_reverts_a_tool_drag(void) {
   GestureFx fx;
   fx_open(&fx);
   DragLog t_log = {0};
   ggaze_viewer_set_overlay(fx.p_viewer, NULL, log_drag_cb, &t_log);
   GtkEventController *p_drag =
      controller_of(fx.p_viewer, GTK_TYPE_GESTURE_DRAG);
   g_signal_emit_by_name(p_drag, "drag-begin", 100.0, 100.0);
   g_signal_emit_by_name(p_drag, "drag-update", 3.0, 2.0);
   ggaze_viewer_pinch_begin(fx.p_viewer, 150.0, 100.0, FALSE);
   g_assert_cmpint(t_log.e_last, ==, GGAZE_VIEWER_DRAG_CANCEL);
   g_assert_true(ggaze_viewer_pinch_end(fx.p_viewer));
   g_assert_cmpuint(t_log.u_calls, ==, 4);
   g_assert_cmpint(t_log.e_last, ==, GGAZE_VIEWER_DRAG_REVERT);
   g_assert_cmpfloat(t_log.d_last_x, ==, 103.0);
   g_signal_emit_by_name(p_drag, "drag-end", 3.0, 2.0);
   g_assert_cmpuint(t_log.u_calls, ==, 4);
   /* A real pinch: CANCEL only. */
   g_signal_emit_by_name(p_drag, "drag-begin", 100.0, 100.0);
   ggaze_viewer_pinch_begin(fx.p_viewer, 150.0, 100.0, FALSE);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.5, 150.0, 100.0);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   g_assert_cmpuint(t_log.u_calls, ==, 6);
   g_assert_cmpint(t_log.e_last, ==, GGAZE_VIEWER_DRAG_CANCEL);
   g_signal_emit_by_name(p_drag, "drag-end", 0.0, 0.0);
   ggaze_viewer_set_overlay(fx.p_viewer, NULL, NULL, NULL);
   fx_close(&fx);
}

/* An unmap in the middle of a tool drag CANCELs it (the tool is told), and
 * the rest of that drag reaches nobody after the remap; a pinch then is a
 * fresh one -- not "from the drag": it sends no second CANCEL, it is a
 * tap on its own time, and it sends no REVERT for a drag it did not take
 * over. A new texture in the middle of a pan drag ends it the same way:
 * what the finger reports next pans nothing (zb2 second review). */
static void
test_unmap_or_new_texture_ends_a_drag(void) {
   GestureFx fx;
   fx_open(&fx);
   DragLog t_log = {0};
   ggaze_viewer_set_overlay(fx.p_viewer, NULL, log_drag_cb, &t_log);
   GtkEventController *p_drag =
      controller_of(fx.p_viewer, GTK_TYPE_GESTURE_DRAG);
   g_signal_emit_by_name(p_drag, "drag-begin", 100.0, 100.0);
   g_signal_emit_by_name(p_drag, "drag-update", 3.0, 2.0);
   gtk_widget_set_visible(GTK_WIDGET(fx.p_viewer), FALSE);
   g_assert_cmpuint(t_log.u_calls, ==, 3);
   g_assert_cmpint(t_log.e_last, ==, GGAZE_VIEWER_DRAG_CANCEL);
   gtk_widget_set_visible(GTK_WIDGET(fx.p_viewer), TRUE);
   g_signal_emit_by_name(p_drag, "drag-update", 10.0, 0.0);
   ggaze_viewer_pinch_begin(fx.p_viewer, 150.0, 100.0, FALSE);
   g_assert_true(ggaze_viewer_pinch_end(fx.p_viewer));
   g_signal_emit_by_name(p_drag, "drag-end", 10.0, 0.0);
   g_assert_cmpuint(t_log.u_calls, ==, 3);
   ggaze_viewer_set_overlay(fx.p_viewer, NULL, NULL, NULL);
   /* A pan drag at 100 %, then a new texture under it. */
   ggaze_viewer_toggle_fit_100(fx.p_viewer);
   gdouble d_px, d_py;
   g_signal_emit_by_name(p_drag, "drag-begin", 100.0, 100.0);
   g_signal_emit_by_name(p_drag, "drag-update", 20.0, 0.0);
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(d_px, ==, 20.0);
   GBytes *p_px =
      g_bytes_new_take(g_malloc0((gsize)B_W * B_H * 4), (gsize)B_W * B_H * 4);
   GdkTexture *p_tex = gdk_memory_texture_new(B_W, B_H, GDK_MEMORY_R8G8B8A8,
                                              p_px, (gsize)B_W * 4);
   g_bytes_unref(p_px);
   ggaze_viewer_set_texture(fx.p_viewer, p_tex);
   g_signal_emit_by_name(p_drag, "drag-update", 60.0, 0.0);
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(d_px, ==, 0.0);
   g_signal_emit_by_name(p_drag, "drag-end", 60.0, 0.0);
   g_object_unref(p_tex);
   ggtest_drain_main(200);
   fx_close(&fx);
}

/* --- swipe --------------------------------------------------------------- */

static gboolean
title_has(GgazeWindow *p_win, const char *c_name) {
   return (g_strstr_len(gtk_window_get_title(GTK_WINDOW(p_win)), -1, c_name) !=
           NULL);
}

/* A leftward flick shows the next image, a rightward one the previous; each
 * goes through the window's gated navigation (a clean window: no prompt). */
static void
test_swipe_navigates(void) {
   GestureFx fx;
   fx_open(&fx);
   g_assert_cmpint(ggaze_viewer_swipe(fx.p_viewer, -200.0, 10.0, -900.0, 0.0),
                   ==, 1);
   GGTEST_WAIT_FOR_TEXTURE(fx.p_win, B_W, B_H);
   g_assert_true(title_has(fx.p_win, "b.png"));
   g_assert_cmpint(ggaze_viewer_swipe(fx.p_viewer, 200.0, -10.0, 900.0, 0.0),
                   ==, -1);
   GGTEST_WAIT_FOR_TEXTURE(fx.p_win, A_W, A_H);
   g_assert_true(title_has(fx.p_win, "a.png"));
   fx_close(&fx);
}

/* Swipes that must not turn the page: vertical, tiny, zero-velocity; any
 * swipe while a tool overlay is installed (the crop / straighten tool owns
 * the drag); and a swipe over a picture zoomed wider than the widget (the
 * same finger is panning it). Zoomed in but still narrower than the widget,
 * a swipe navigates again. */
static void
test_swipe_refusals(void) {
   GestureFx fx;
   fx_open(&fx);
   g_assert_cmpint(ggaze_viewer_swipe(fx.p_viewer, 0.0, 300.0, 0.0, 900.0), ==,
                   0);
   g_assert_cmpint(ggaze_viewer_swipe(fx.p_viewer, -20.0, 0.0, -900.0, 0.0), ==,
                   0);
   g_assert_cmpint(ggaze_viewer_swipe(fx.p_viewer, -200.0, 0.0, 0.0, 0.0), ==,
                   0);
   DragLog t_log = {0};
   ggaze_viewer_set_overlay(fx.p_viewer, NULL, log_drag_cb, &t_log);
   g_assert_cmpint(ggaze_viewer_swipe(fx.p_viewer, -200.0, 0.0, -900.0, 0.0),
                   ==, 0);
   ggaze_viewer_set_overlay(fx.p_viewer, NULL, NULL, NULL);
   ggaze_viewer_toggle_fit_100(fx.p_viewer); /* 100 %: 1200 px > widget */
   g_assert_cmpint(ggaze_viewer_swipe(fx.p_viewer, -200.0, 0.0, -900.0, 0.0),
                   ==, 0);
   ggaze_viewer_toggle_fit_100(fx.p_viewer); /* back to fit */
   ggaze_viewer_zoom_out(fx.p_viewer);       /* zoomed, but narrower */
   ggtest_drain_main(100);
   g_assert_true(title_has(fx.p_win, "a.png"));
   g_assert_cmpint(ggaze_viewer_swipe(fx.p_viewer, -200.0, 0.0, -900.0, 0.0),
                   ==, 1);
   GGTEST_WAIT_FOR_TEXTURE(fx.p_win, B_W, B_H);
   fx_close(&fx);
}

static gboolean
status_has(GgazeWindow *p_win, const char *c_prefix) {
   const char *c_text =
      gtk_label_get_text(GTK_LABEL(ggaze_window_get_info_label(p_win)));
   return (g_str_has_prefix(c_text, c_prefix));
}

/* A swipe is exactly `l` / `h`: it stops a running slideshow before it
 * navigates. So does a wheel notch in the wheel's navigate mode (a
 * behaviour change with zb2: it used to navigate under a running
 * slideshow and leave it running), driven through the real scroll
 * handler. */
static void
test_swipe_and_wheel_stop_the_slideshow(void) {
   GestureFx fx;
   fx_open(&fx);
   gtk_widget_activate_action(GTK_WIDGET(fx.p_win), "win.slideshow", NULL);
   g_assert_true(status_has(fx.p_win, "Slideshow started"));
   g_assert_cmpint(ggaze_viewer_swipe(fx.p_viewer, -200.0, 0.0, -900.0, 0.0),
                   ==, 1);
   g_assert_true(status_has(fx.p_win, "Slideshow stopped"));
   GGTEST_WAIT_FOR_TEXTURE(fx.p_win, B_W, B_H);
   /* Stopped for real: the toggle starts it again rather than stopping. */
   gtk_widget_activate_action(GTK_WIDGET(fx.p_win), "win.slideshow", NULL);
   g_assert_true(status_has(fx.p_win, "Slideshow started"));
   ggaze_viewer_set_scroll_behavior(fx.p_viewer, GGAZE_SCROLL_NAVIGATE);
   GtkEventController *p_scroll =
      controller_of(fx.p_viewer, GTK_TYPE_EVENT_CONTROLLER_SCROLL);
   gboolean b_handled = FALSE;
   g_signal_emit_by_name(p_scroll, "scroll", 0.0, -1.0, &b_handled);
   g_assert_true(b_handled);
   g_assert_true(status_has(fx.p_win, "Slideshow stopped"));
   GGTEST_WAIT_FOR_TEXTURE(fx.p_win, A_W, A_H);
   gtk_widget_activate_action(GTK_WIDGET(fx.p_win), "win.slideshow", NULL);
   g_assert_true(status_has(fx.p_win, "Slideshow started"));
   gtk_widget_activate_action(GTK_WIDGET(fx.p_win), "win.slideshow", NULL);
   fx_close(&fx);
}

static void
count_navigate_cb(GgazeViewer *p_v, gint i_dir, gpointer p_data) {
   (void)p_v;
   (void)i_dir;
   (*(guint *)p_data)++;
}

/* A slideshow step while a finger is down spoils that swipe (zb2 third
 * review): the flick was aimed at a.png, and letting it turn the page
 * again from b.png, which the slideshow put up under it, would skip an
 * image. The spoil is per touch: the next finger swipes as ever. */
static void
test_slideshow_step_spoils_a_swipe(void) {
   Settings *p_s = settings_new();
   settings_set_slideshow_delay(p_s, 0.3);
   GestureFx fx;
   fx_open(&fx);
   GtkEventController *p_swipe =
      controller_of(fx.p_viewer, GTK_TYPE_GESTURE_SWIPE);
   guint  u_nav = 0;
   gulong u_id  = g_signal_connect(fx.p_viewer, "navigate",
                                   G_CALLBACK(count_navigate_cb), &u_nav);
   ggaze_viewer_swipe_track(fx.p_viewer, TRUE, 400.0, 200.0); /* on a.png */
   gtk_widget_activate_action(GTK_WIDGET(fx.p_win), "win.slideshow", NULL);
   GGTEST_WAIT_FOR_TEXTURE(fx.p_win, B_W, B_H); /* the slideshow stepped */
   gtk_widget_activate_action(GTK_WIDGET(fx.p_win), "win.slideshow", NULL);
   g_assert_true(status_has(fx.p_win, "Slideshow stopped"));
   ggaze_viewer_swipe_track(fx.p_viewer, FALSE, 150.0, 205.0);
   g_signal_emit_by_name(p_swipe, "swipe", -900.0, 0.0);
   ggtest_drain_main(200);
   g_assert_cmpuint(u_nav, ==, 0);
   /* Negative: a fresh finger (a rightward flick) navigates. */
   ggaze_viewer_swipe_track(fx.p_viewer, TRUE, 150.0, 200.0);
   ggaze_viewer_swipe_track(fx.p_viewer, FALSE, 400.0, 205.0);
   g_signal_emit_by_name(p_swipe, "swipe", 900.0, 0.0);
   g_assert_cmpuint(u_nav, ==, 1);
   ggtest_drain_main(300);
   g_signal_handler_disconnect(fx.p_viewer, u_id);
   fx_close(&fx);
   g_settings_reset(settings_get_gsettings(p_s), "slideshow-delay");
   settings_delete(p_s);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
   if (!gtk_init_check()) {
      /* Exit 77 is meson's "skipped"; see tests/meson.build "Lane
       * determinism" for why this must not report a passing empty run. */
      g_print("1..0 # SKIP no display available (run under xvfb)\n");
      return (77);
   }
   g_test_add_func("/gestures/pinch_zooms_about_the_midpoint",
                   test_pinch_zooms_about_the_midpoint);
   g_test_add_func("/gestures/pinch_clamps_and_guards",
                   test_pinch_clamps_and_guards);
   g_test_add_func("/gestures/pinch_pans_with_the_midpoint",
                   test_pinch_pans_with_the_midpoint);
   g_test_add_func("/gestures/two_finger_pan_keeps_fit",
                   test_two_finger_pan_keeps_fit);
   g_test_add_func("/gestures/leaving_the_fit_detent_does_not_jump",
                   test_leaving_the_fit_detent_does_not_jump);
   g_test_add_func("/gestures/pinch_at_max_zoom_still_pans",
                   test_pinch_at_max_zoom_still_pans);
   g_test_add_func("/gestures/zoom_controller_drives_the_pinch",
                   test_zoom_controller_drives_the_pinch);
   g_test_add_func("/gestures/swipe_controller_navigates_unless_spoiled",
                   test_swipe_controller_navigates_unless_spoiled);
   g_test_add_func("/gestures/pinch_ends_a_tool_drag",
                   test_pinch_ends_a_tool_drag);
   g_test_add_func("/gestures/two_finger_tap_toggles_info",
                   test_two_finger_tap_toggles_info);
   g_test_add_func("/gestures/long_or_moving_two_finger_touch_is_no_tap",
                   test_long_or_moving_two_finger_touch_is_no_tap);
   g_test_add_func("/gestures/tap_restores_the_view_before_the_first_finger",
                   test_tap_restores_the_view_before_the_first_finger);
   g_test_add_func("/gestures/touchpad_pinch_is_never_a_tap",
                   test_touchpad_pinch_is_never_a_tap);
   g_test_add_func("/gestures/new_texture_or_unmap_ends_the_pinch",
                   test_new_texture_or_unmap_ends_the_pinch);
   g_test_add_func("/gestures/tap_reverts_a_tool_drag",
                   test_tap_reverts_a_tool_drag);
   g_test_add_func("/gestures/unmap_or_new_texture_ends_a_drag",
                   test_unmap_or_new_texture_ends_a_drag);
   g_test_add_func("/gestures/swipe_navigates", test_swipe_navigates);
   g_test_add_func("/gestures/swipe_and_wheel_stop_the_slideshow",
                   test_swipe_and_wheel_stop_the_slideshow);
   g_test_add_func("/gestures/swipe_refusals", test_swipe_refusals);
   g_test_add_func("/gestures/slideshow_step_spoils_a_swipe",
                   test_slideshow_step_spoils_a_swipe);
   return (g_test_run());
}
