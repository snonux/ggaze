/*:*
 * ggaze — large-view zoom (integration)
 *
 * hx0: zooming a picture in the large ("detailed") view. The viewer has had
 * zoom logic since decision #31, but nothing outside the widget could ever
 * observe the resulting scale, so the whole path went untested and a defect
 * that made the image DISAPPEAR on the first wheel notch shipped unnoticed.
 * These tests therefore assert on ggaze_viewer_get_scale() (viewer.h) and on
 * the finiteness of the geometry -- never merely that a call returned.
 *
 * The regression itself: a scroll event often carries no position (X11 wheel
 * events do not), and gdk_event_get_position() writes NAN to both
 * out-parameters when it fails. That NaN reached the pan fields and every
 * later frame drew a NaN rect, so the picture vanished permanently -- not even
 * the keyboard could recover it, because each zoom derives its pan from the
 * previous one. The guards now live in _zoom_at() and ggaze_viewer_pan(), and
 * the "non-finite" tests below pin them: such an input must be REJECTED,
 * leaving the last good geometry on screen.
 *
 * Fixtures are generated here rather than taken from tests/fixtures: the
 * shared ones are a few pixels across (plain.jpg is 6x3), which in a 600x400
 * window puts the fit scale at 100 -- above GGAZE_ZOOM_MAX (64) -- so they
 * cannot express "zooming in makes it bigger" at all.
 *
 * Needs a display (a real GgazeWindow with a realized, allocated viewer),
 * hence the `integration` suite.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "window.h"

#include "ggaze-config.h"
#include "viewer.h"

#include <math.h>

#include <gio/gio.h>
#include <glib/gstdio.h> /* g_remove / g_rmdir */
#include <gtk/gtk.h>

/* Comfortably larger than the test window, so fit-to-window is well below 1.0
 * and a zoom step in either direction stays inside the clamp range. */
#define FIXTURE_W 1200
#define FIXTURE_H 800

static void
drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

static GgazeViewer *
viewer_of(GgazeWindow *p_win) {
   GtkStack *p_stack = ggaze_window_get_stack(p_win);
   return (GGAZE_VIEWER(gtk_stack_get_child_by_name(p_stack, "large")));
}

/* A folder holding one FIXTURE_W x FIXTURE_H PNG. Returns the temp directory;
 * the caller frees it via cleanup_dir(). */
static char *
make_fixture_dir(char **p_img_path) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-viewer-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   GdkPixbuf *p_pix =
      gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, FIXTURE_W, FIXTURE_H);
   g_assert_nonnull(p_pix);
   gdk_pixbuf_fill(p_pix, 0x336699ffu);
   char *c_path = g_build_filename(c_dir, "big.png", NULL);
   g_assert_true(gdk_pixbuf_save(p_pix, c_path, "png", &p_err, NULL));
   g_assert_no_error(p_err);
   g_object_unref(p_pix);
   *p_img_path = c_path;
   return (c_dir);
}

static void
cleanup_dir(char *c_dir, char *c_img) {
   g_remove(c_img);
   g_rmdir(c_dir);
   g_free(c_img);
   g_free(c_dir);
}

/* Fixture bundle: every subtest needs the same window-on-a-big-image setup,
 * and each must tear down the temp folder it created. */
typedef struct {
   char        *c_dir;
   char        *c_img;
   GgazeWindow *p_win;
   GgazeViewer *p_viewer;
} ViewerFx;

/* Zoom is computed from the widget allocation, so an unrealized viewer would
 * make every scale meaningless (_compute_geom's zero-size guard returns a fit
 * ratio of 1.0). Wait for a real allocation before any assertion. */
static void
fx_open(ViewerFx *p_fx) {
   p_fx->c_dir         = make_fixture_dir(&p_fx->c_img);
   GFile       *p_file = g_file_new_for_path(p_fx->c_img);
   GgazeWindow *p_win  = GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL));
   gtk_window_set_default_size(GTK_WINDOW(p_win), 600, 400);
   gtk_window_present(GTK_WINDOW(p_win));
   ggaze_window_open(p_win, p_file);
   p_fx->p_win    = p_win;
   p_fx->p_viewer = viewer_of(p_win);

   for (guint u = 0;
        u < 3000 && ggaze_viewer_get_texture(p_fx->p_viewer) == NULL; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(ggaze_viewer_get_texture(p_fx->p_viewer));
   for (guint u = 0;
        u < 3000 && gtk_widget_get_width(GTK_WIDGET(p_fx->p_viewer)) == 0;
        u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(p_fx->p_viewer)), >, 0);
   g_object_unref(p_file);
}

static void
fx_close(ViewerFx *p_fx) {
   gtk_window_destroy(GTK_WINDOW(p_fx->p_win));
   drain_main(200);
   cleanup_dir(p_fx->c_dir, p_fx->c_img);
}

static void
fire(GgazeWindow *p_win, const char *c_action) {
   gtk_widget_activate_action(GTK_WIDGET(p_win), c_action, NULL);
}

/* hx0 as an assertion: win.zoom-in must actually increase the drawn scale
 * while the stack is on "large". */
static void
test_zoom_in_action_increases_scale(void) {
   ViewerFx fx;
   fx_open(&fx);
   g_assert_cmpstr(
      gtk_stack_get_visible_child_name(ggaze_window_get_stack(fx.p_win)), ==,
      "large");

   gdouble d_before = ggaze_viewer_get_scale(fx.p_viewer);
   g_assert_cmpfloat(d_before, >, 0.0);
   fire(fx.p_win, "win.zoom-in");
   drain_main(50);
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), >, d_before);

   fx_close(&fx);
}

static void
test_zoom_out_action_decreases_scale(void) {
   ViewerFx fx;
   fx_open(&fx);

   gdouble d_before = ggaze_viewer_get_scale(fx.p_viewer);
   g_assert_cmpfloat(d_before, >, 0.0);
   fire(fx.p_win, "win.zoom-out");
   drain_main(50);
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), <, d_before);

   fx_close(&fx);
}

/* Zoom in then out must land back where it started, so repeated use cannot
 * drift the scale. */
static void
test_zoom_in_then_out_round_trips(void) {
   ViewerFx fx;
   fx_open(&fx);

   gdouble d_start = ggaze_viewer_get_scale(fx.p_viewer);
   ggaze_viewer_zoom_in(fx.p_viewer);
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), >, d_start);
   ggaze_viewer_zoom_out(fx.p_viewer);
   g_assert_cmpfloat(ABS(ggaze_viewer_get_scale(fx.p_viewer) - d_start), <,
                     0.0001);

   fx_close(&fx);
}

/* `0` toggles fit <-> 100% (docs/ui-and-interactions.md "Zoom behavior"). The
 * fixture is larger than the window, so fit is below 1.0 and the toggle is
 * observable in both directions. */
static void
test_toggle_fit_100(void) {
   ViewerFx fx;
   fx_open(&fx);

   gdouble d_fit = ggaze_viewer_get_scale(fx.p_viewer);
   g_assert_cmpfloat(d_fit, >, 0.0);
   g_assert_cmpfloat(d_fit, <, 1.0);
   ggaze_viewer_toggle_fit_100(fx.p_viewer);
   g_assert_cmpfloat(ABS(ggaze_viewer_get_scale(fx.p_viewer) - 1.0), <, 0.0001);
   ggaze_viewer_toggle_fit_100(fx.p_viewer);
   g_assert_cmpfloat(ABS(ggaze_viewer_get_scale(fx.p_viewer) - d_fit), <,
                     0.0001);

   fx_close(&fx);
}

/* THE hx0 REGRESSION. A non-finite pan delta -- what the wheel path produced
 * once gdk_event_get_position()'s NAN reached the pan fields -- must be
 * rejected outright. Before the guard this poisoned d_pan_x/d_pan_y forever:
 * the scale still read back plausibly, but the drawn rect was NaN, so the
 * picture was gone and every later zoom kept deriving NaN from NaN. Asserting
 * that zoom still WORKS afterwards is the part that would have failed. */
static void
test_non_finite_pan_is_rejected(void) {
   ViewerFx fx;
   fx_open(&fx);

   gdouble d_start = ggaze_viewer_get_scale(fx.p_viewer);
   ggaze_viewer_pan(fx.p_viewer, NAN, 0.0);
   ggaze_viewer_pan(fx.p_viewer, 0.0, NAN);
   ggaze_viewer_pan(fx.p_viewer, INFINITY, -INFINITY);
   drain_main(50);

   /* Assert on the PAN, not just the scale. The scale is derived from the fit
    * ratio or d_zoom and stays finite even while the pan is NaN -- so a
    * scale-only check passes against the very bug this test exists for. The
    * pan is what the draw rect is built from, and what stays poisoned. */
   gdouble d_px = 0.0, d_py = 0.0;
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_true(isfinite(d_px));
   g_assert_true(isfinite(d_py));
   g_assert_true(isfinite(ggaze_viewer_get_scale(fx.p_viewer)));

   /* The widget must still be usable: zoom continues to respond and leaves
    * both scale and pan finite, which it would not if a NaN had been absorbed.
    */
   ggaze_viewer_zoom_in(fx.p_viewer);
   gdouble d_in = ggaze_viewer_get_scale(fx.p_viewer);
   g_assert_true(isfinite(d_in));
   g_assert_cmpfloat(d_in, >, d_start);
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_true(isfinite(d_px));
   g_assert_true(isfinite(d_py));
   ggaze_viewer_zoom_out(fx.p_viewer);
   g_assert_cmpfloat(ABS(ggaze_viewer_get_scale(fx.p_viewer) - d_start), <,
                     0.0001);

   fx_close(&fx);
}

/* The guard above must not have turned panning into a no-op: a finite pan is
 * still applied, and leaves the scale untouched and finite. */
static void
test_finite_pan_still_applies(void) {
   ViewerFx fx;
   fx_open(&fx);

   ggaze_viewer_toggle_fit_100(fx.p_viewer); /* 100%: image exceeds window */
   drain_main(50);
   gdouble d_scale = ggaze_viewer_get_scale(fx.p_viewer);
   ggaze_viewer_pan(fx.p_viewer, -40.0, -30.0);
   drain_main(50);
   g_assert_true(isfinite(ggaze_viewer_get_scale(fx.p_viewer)));
   g_assert_cmpfloat(ABS(ggaze_viewer_get_scale(fx.p_viewer) - d_scale), <,
                     0.0001);

   fx_close(&fx);
}

/* Negative: with no texture there is nothing to scale, and zoom/pan must be
 * safe no-ops rather than dividing by a zero-sized image. */
static void
test_zoom_without_texture_is_safe(void) {
   GtkWidget *p_v = ggaze_viewer_new();
   g_object_ref_sink(p_v);
   g_assert_cmpfloat(ggaze_viewer_get_scale(GGAZE_VIEWER(p_v)), ==, 0.0);
   ggaze_viewer_zoom_in(GGAZE_VIEWER(p_v));
   ggaze_viewer_zoom_out(GGAZE_VIEWER(p_v));
   ggaze_viewer_pan(GGAZE_VIEWER(p_v), 10.0, 10.0);
   g_assert_cmpfloat(ggaze_viewer_get_scale(GGAZE_VIEWER(p_v)), ==, 0.0);
   g_object_unref(p_v);
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

   g_test_add_func("/viewer/zoom_in_action_increases_scale",
                   test_zoom_in_action_increases_scale);
   g_test_add_func("/viewer/zoom_out_action_decreases_scale",
                   test_zoom_out_action_decreases_scale);
   g_test_add_func("/viewer/zoom_in_then_out_round_trips",
                   test_zoom_in_then_out_round_trips);
   g_test_add_func("/viewer/toggle_fit_100", test_toggle_fit_100);
   g_test_add_func("/viewer/non_finite_pan_is_rejected",
                   test_non_finite_pan_is_rejected);
   g_test_add_func("/viewer/finite_pan_still_applies",
                   test_finite_pan_still_applies);
   g_test_add_func("/viewer/zoom_without_texture_is_safe",
                   test_zoom_without_texture_is_safe);
   return (g_test_run());
}
