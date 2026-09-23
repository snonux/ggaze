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
 *     installed (as test_viewer.c does for the drag gesture), which pins
 *     that the controllers exist, are wired, and fall back safely when the
 *     gesture reports no points.
 *
 * What is asserted: a pinch zooms about its midpoint (the image pixel under
 * it stays put) with the wheel's clamp and NaN / Inf / zero-scale guards; a
 * pinch ends a one-finger tool drag with an END where the finger was, and a
 * pan drag where it got to, and swallows the rest of either; a leftward
 * swipe shows the next image and a rightward one the previous, while a
 * vertical, tiny or zero-velocity
 * swipe, a swipe over a zoomed-in picture and a swipe with a tool overlay
 * installed navigate nowhere; a two-finger tap toggles the info card and
 * leaves the view as it was, a long or moving one does not. The dirty-gate
 * and real-tool halves (a swipe away from an enhance preview prompts, a
 * crop tool refuses a swipe) need GEGL and live in test_enhance_flow.c.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "window.h"

#include "gesture-math.h"
#include "gtk_helpers.h"
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

/* A pinch at an off-centre midpoint doubles the scale and keeps the image
 * pixel under the midpoint where it was (cursor-centred like the wheel,
 * docs/ui-and-interactions.md "Zoom behavior"); the update is absolute
 * from the begin scale, and a moved midpoint zooms about the new one. */
static void
test_pinch_zooms_about_the_midpoint(void) {
   GestureFx fx;
   fx_open(&fx);
   gdouble d_fit = ggaze_viewer_get_scale(fx.p_viewer);
   gdouble d_ix0, d_iy0, d_ix1, d_iy1;
   image_point_at(fx.p_viewer, 150.0, 120.0, &d_ix0, &d_iy0);
   ggaze_viewer_pinch_begin(fx.p_viewer, 150.0, 120.0);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.5, 150.0, 120.0);
   ggaze_viewer_pinch_update(fx.p_viewer, 2.0, 150.0, 120.0);
   g_assert_cmpfloat(fabs(ggaze_viewer_get_scale(fx.p_viewer) - 2.0 * d_fit), <,
                     1e-9);
   image_point_at(fx.p_viewer, 150.0, 120.0, &d_ix1, &d_iy1);
   g_assert_cmpfloat(fabs(d_ix1 - d_ix0), <, 1e-6);
   g_assert_cmpfloat(fabs(d_iy1 - d_iy0), <, 1e-6);
   /* Midpoint moved: zoom about where it is now. */
   image_point_at(fx.p_viewer, 400.0, 200.0, &d_ix0, &d_iy0);
   ggaze_viewer_pinch_update(fx.p_viewer, 2.5, 400.0, 200.0);
   image_point_at(fx.p_viewer, 400.0, 200.0, &d_ix1, &d_iy1);
   g_assert_cmpfloat(fabs(d_ix1 - d_ix0), <, 1e-6);
   g_assert_cmpfloat(fabs(d_iy1 - d_iy0), <, 1e-6);
   /* A real pinch, not a tap: no info card toggle, the zoom stays. */
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   g_assert_cmpfloat(fabs(ggaze_viewer_get_scale(fx.p_viewer) - 2.5 * d_fit), <,
                     1e-9);
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
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0);
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

/* The controllers are there and wired. The pinch's signals are emitted
 * inside a pinch opened through the seam -- GtkGestureZoom's own "begin"
 * class handler reads the gesture's last event and cannot be emitted
 * without real touches -- so "scale-changed" zooms about the begin
 * midpoint (the gesture reports no points), "end" closes the pinch (a
 * later "scale-changed" does nothing), and "cancel" makes a tap-shaped
 * touch no tap. */
static void
test_gesture_controllers_are_wired(void) {
   GestureFx fx;
   fx_open(&fx);
   gdouble             d_fit = ggaze_viewer_get_scale(fx.p_viewer);
   GtkEventController *p_zoom =
      controller_of(fx.p_viewer, GTK_TYPE_GESTURE_ZOOM);
   g_assert_true(has_handler(p_zoom, "begin"));
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0);
   g_signal_emit_by_name(p_zoom, "scale-changed", 2.0);
   g_assert_cmpfloat(fabs(ggaze_viewer_get_scale(fx.p_viewer) - 2.0 * d_fit), <,
                     1e-9);
   g_signal_emit_by_name(p_zoom, "end", NULL);
   g_signal_emit_by_name(p_zoom, "scale-changed", 3.0);
   g_assert_cmpfloat(fabs(ggaze_viewer_get_scale(fx.p_viewer) - 2.0 * d_fit), <,
                     1e-9);
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0);
   g_signal_emit_by_name(p_zoom, "cancel", NULL);
   g_signal_emit_by_name(p_zoom, "end", NULL);
   ggtest_drain_main(200);
   g_assert_false(
      gtk_widget_get_visible(ggaze_window_get_info_label(fx.p_win)));
   /* The swipe gesture: touch-only, so a mouse drag can never turn a page;
    * a "swipe" whose "begin" found no point has no path and is refused. */
   GtkEventController *p_swipe =
      controller_of(fx.p_viewer, GTK_TYPE_GESTURE_SWIPE);
   g_assert_true(
      gtk_gesture_single_get_touch_only(GTK_GESTURE_SINGLE(p_swipe)));
   g_assert_true(has_handler(p_swipe, "update"));
   g_signal_emit_by_name(p_swipe, "begin", NULL);
   g_signal_emit_by_name(p_swipe, "swipe", -2000.0, 0.0);
   ggtest_drain_main(100);
   g_assert_nonnull(
      g_strstr_len(gtk_window_get_title(GTK_WINDOW(fx.p_win)), -1, "a.png"));
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
 * tool gets its END where the finger was, and the rest of that drag --
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
   ggaze_viewer_pinch_begin(fx.p_viewer, 150.0, 100.0);
   g_assert_cmpuint(t_log.u_calls, ==, 3);
   g_assert_cmpint(t_log.e_last, ==, GGAZE_VIEWER_DRAG_END);
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
   ggaze_viewer_pinch_begin(fx.p_viewer, 150.0, 100.0);
   g_signal_emit_by_name(p_drag, "drag-update", 80.0, 0.0);
   g_signal_emit_by_name(p_drag, "drag-end", 80.0, 0.0);
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(d_px, ==, 20.0);
   ggaze_viewer_pinch_end(fx.p_viewer);
   fx_close(&fx);
}

/* --- two-finger tap ------------------------------------------------------ */

static gboolean
info_visible(gpointer p_data) {
   return (gtk_widget_get_visible(ggaze_window_get_info_label(p_data)));
}

static gboolean
info_hidden(gpointer p_data) {
   return (!info_visible(p_data));
}

/* A short, still two-finger touch toggles the info card (win.info, as `i`
 * does) -- on, then off -- and leaves a fitted view fitted even though the
 * fingers wobbled the scale a little. */
static void
test_two_finger_tap_toggles_info(void) {
   GestureFx fx;
   fx_open(&fx);
   gdouble d_fit = ggaze_viewer_get_scale(fx.p_viewer);
   g_assert_false(info_visible(fx.p_win));
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.05, 303.0, 201.0);
   g_assert_true(ggaze_viewer_pinch_end(fx.p_viewer));
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), ==, d_fit);
   g_assert_true(ggtest_wait_until(info_visible, fx.p_win, WAIT_US));
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0);
   g_assert_true(ggaze_viewer_pinch_end(fx.p_viewer));
   g_assert_true(ggtest_wait_until(info_hidden, fx.p_win, WAIT_US));
   fx_close(&fx);
}

/* Not taps: two fingers that moved, pinched, or stayed down too long. */
static void
test_long_or_moving_two_finger_touch_is_no_tap(void) {
   GestureFx fx;
   fx_open(&fx);
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.0, 300.0 + 3 * GESTURE_TAP_MAX_MOVE,
                             200.0);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0);
   ggaze_viewer_pinch_update(fx.p_viewer, 1.5, 300.0, 200.0);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   ggaze_viewer_pinch_begin(fx.p_viewer, 300.0, 200.0);
   g_usleep(GESTURE_TAP_MAX_US + 50 * G_TIME_SPAN_MILLISECOND);
   g_assert_false(ggaze_viewer_pinch_end(fx.p_viewer));
   ggtest_drain_main(200);
   g_assert_false(info_visible(fx.p_win));
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
   g_test_add_func("/gestures/controllers_are_wired",
                   test_gesture_controllers_are_wired);
   g_test_add_func("/gestures/pinch_ends_a_tool_drag",
                   test_pinch_ends_a_tool_drag);
   g_test_add_func("/gestures/two_finger_tap_toggles_info",
                   test_two_finger_tap_toggles_info);
   g_test_add_func("/gestures/long_or_moving_two_finger_touch_is_no_tap",
                   test_long_or_moving_two_finger_touch_is_no_tap);
   g_test_add_func("/gestures/swipe_navigates", test_swipe_navigates);
   g_test_add_func("/gestures/swipe_refusals", test_swipe_refusals);
   return (g_test_run());
}
