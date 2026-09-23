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
 * The /viewer/animation_* subtests (yb2) play tests/fixtures/anim.gif
 * through the real pipeline and assert what the viewer contract promises:
 * the frames advance on their own while the widget is mapped, the first
 * frame stays THE texture (what the cache, the histogram and hold-Space
 * compare against), zoom / pan / the overlay geometry are the canvas's,
 * a still never animates, an unmapped viewer (the grid page) plays
 * nothing and resumes on remap, and a 0 ms delay plays at the clamp
 * instead of spinning the main loop.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "window.h"

#include "ggaze-config.h"
#include "loader/animation.h"
#include "loader/loader.h"
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

/* jx0: an image small enough that fit-to-window already exceeds
 * GGAZE_ZOOM_MAX. tests/fixtures/plain.jpg is 6x3, so in this 600x400 window
 * it fits at 100x -- above the 64x ceiling. Zooming in must never make such a
 * picture SMALLER, which is exactly what clamping to a bare GGAZE_ZOOM_MAX
 * did (scale went 100 -> 64 on the first win.zoom-in). At the top end a
 * no-op is correct; a reversal is not. */
static void
test_zoom_in_never_shrinks_a_tiny_image(void) {
   const gchar *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char        *c_path = g_build_filename(c_fx, "plain.jpg", NULL);
   GFile       *p_file = g_file_new_for_path(c_path);
   GgazeWindow *p_win  = GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL));
   gtk_window_set_default_size(GTK_WINDOW(p_win), 600, 400);
   gtk_window_present(GTK_WINDOW(p_win));
   ggaze_window_open(p_win, p_file);
   GgazeViewer *p_v = viewer_of(p_win);
   for (guint u = 0; u < 3000 && ggaze_viewer_get_texture(p_v) == NULL; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(ggaze_viewer_get_texture(p_v));
   for (guint u = 0; u < 3000 && gtk_widget_get_width(GTK_WIDGET(p_v)) == 0;
        u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }

   /* Guard the premise: if this fixture ever stops fitting above the ceiling
    * the test would silently stop covering jx0. */
   gdouble d_fit = ggaze_viewer_get_scale(p_v);
   g_assert_cmpfloat(d_fit, >, 64.0);

   ggaze_viewer_zoom_in(p_v);
   drain_main(50);
   g_assert_cmpfloat(ggaze_viewer_get_scale(p_v), >=, d_fit);

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(200);
   g_free(c_path);
}

/* The viewer's GtkGestureDrag (its handlers are static; the test drives
 * them by emitting the gesture's own signals, which is what GTK does). */
static GtkGesture *
drag_gesture_of(GgazeViewer *p_v) {
   GListModel *p_ctrls = gtk_widget_observe_controllers(GTK_WIDGET(p_v));
   GtkGesture *p_drag  = NULL;
   for (guint i = 0; i < g_list_model_get_n_items(p_ctrls); i++) {
      GtkEventController *p_c = g_list_model_get_item(p_ctrls, i);
      if (GTK_IS_GESTURE_DRAG(p_c) && p_drag == NULL) {
         p_drag = GTK_GESTURE(p_c); /* borrowed: the widget owns it */
      }
      g_object_unref(p_c);
   }
   g_object_unref(p_ctrls);
   g_assert_nonnull(p_drag);
   return (p_drag);
}

static void
overlay_drag_cb(GgazeViewerDragPhase e_phase, gdouble d_x, gdouble d_y,
                gpointer p_data) {
   (void)e_phase;
   (void)d_x;
   (void)d_y;
   (*(guint *)p_data)++;
}

/* A drag that began with a tool overlay installed belongs to the tool; if
 * the overlay goes away mid-gesture (Esc while dragging the crop rectangle)
 * the rest of the gesture pans from where the pointer is NOW -- the first
 * pan step used to be the whole offset accumulated since the press. An
 * overlay installed mid-gesture never gets an UPDATE without its BEGIN. */
static void
test_overlay_removed_mid_drag_rebases_the_pan(void) {
   ViewerFx fx;
   fx_open(&fx);
   ggaze_viewer_zoom_in(fx.p_viewer); /* so a pan can move the image */
   GtkGesture *p_drag  = drag_gesture_of(fx.p_viewer);
   guint       u_calls = 0;
   ggaze_viewer_set_overlay(fx.p_viewer, NULL, overlay_drag_cb, &u_calls);
   g_signal_emit_by_name(p_drag, "drag-begin", 10.0, 10.0);
   g_signal_emit_by_name(p_drag, "drag-update", 100.0, 50.0);
   g_assert_cmpuint(u_calls, ==, 2); /* BEGIN + UPDATE went to the tool */
   gdouble d_px, d_py;
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(d_px, ==, 0.0);
   ggaze_viewer_set_overlay(fx.p_viewer, NULL, NULL, NULL); /* Esc */
   g_signal_emit_by_name(p_drag, "drag-update", 100.0, 50.0);
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(d_px, ==, 0.0); /* not 100: re-based, no jump */
   g_assert_cmpfloat(d_py, ==, 0.0);
   g_signal_emit_by_name(p_drag, "drag-update", 130.0, 60.0);
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(d_px, ==, 30.0); /* the delta since the hand-over */
   g_assert_cmpfloat(d_py, ==, 10.0);
   g_signal_emit_by_name(p_drag, "drag-end", 130.0, 60.0);
   g_assert_cmpuint(u_calls, ==, 2); /* the END did not go to the tool */
   /* The other way round: a pan drag stays a pan when a tool appears. */
   g_signal_emit_by_name(p_drag, "drag-begin", 0.0, 0.0);
   ggaze_viewer_set_overlay(fx.p_viewer, NULL, overlay_drag_cb, &u_calls);
   g_signal_emit_by_name(p_drag, "drag-update", 5.0, 0.0);
   g_signal_emit_by_name(p_drag, "drag-end", 5.0, 0.0);
   g_assert_cmpuint(u_calls, ==, 2); /* no BEGIN, so no UPDATE / END */
   ggaze_viewer_get_pan(fx.p_viewer, &d_px, &d_py);
   g_assert_cmpfloat(d_px, ==, 35.0);
   ggaze_viewer_set_overlay(fx.p_viewer, NULL, NULL, NULL);
   fx_close(&fx);
}

/* --- animation (yb2) ----------------------------------------------------- */

/* A folder holding one copy of a committed fixture, opened in a presented
 * window; waits for the texture and a real allocation like fx_open(). */
static void
fx_open_fixture(ViewerFx *p_fx, const char *c_fixture) {
   GError *p_err = NULL;
   p_fx->c_dir   = g_dir_make_tmp("ggaze-viewer-anim-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   const gchar *c_fx  = g_getenv("GGAZE_FIXTURES_DIR");
   char        *c_src = g_build_filename(c_fx, c_fixture, NULL);
   p_fx->c_img        = g_build_filename(p_fx->c_dir, c_fixture, NULL);
   GFile *p_src       = g_file_new_for_path(c_src);
   GFile *p_dst       = g_file_new_for_path(p_fx->c_img);
   g_assert_true(g_file_copy(p_src, p_dst, G_FILE_COPY_OVERWRITE, NULL, NULL,
                             NULL, &p_err));
   g_assert_no_error(p_err);
   g_object_unref(p_src);
   g_free(c_src);

   GgazeWindow *p_win = GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL));
   gtk_window_set_default_size(GTK_WINDOW(p_win), 600, 400);
   gtk_window_present(GTK_WINDOW(p_win));
   ggaze_window_open(p_win, p_dst);
   p_fx->p_win    = p_win;
   p_fx->p_viewer = viewer_of(p_win);
   for (guint u = 0;
        u < 3000 && (ggaze_viewer_get_texture(p_fx->p_viewer) == NULL ||
                     gtk_widget_get_width(GTK_WIDGET(p_fx->p_viewer)) == 0);
        u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(ggaze_viewer_get_texture(p_fx->p_viewer));
   g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(p_fx->p_viewer)), >, 0);
   g_object_unref(p_dst);
}

/* The green channel of p_tex's top-left pixel. The animated fixtures'
 * first frame is solid (0, 255, 0); every later frame is far below 200
 * (tests/fixtures/gen.py ANIM_FRAME_RGB). gdk_texture_download() writes
 * the whole texture as premultiplied BGRA; the fixtures are opaque. */
static guint8
green_of(GdkTexture *p_tex) {
   gsize   u_stride = 4u * (gsize)gdk_texture_get_width(p_tex);
   guint8 *p_px = g_malloc0(u_stride * (gsize)gdk_texture_get_height(p_tex));
   gdk_texture_download(p_tex, p_px, u_stride);
   guint8 u_g = p_px[1];
   g_free(p_px);
   return (u_g);
}

/* Pump until the frame on screen is not the first one (or 3 s). TRUE when
 * it happened. */
static gboolean
wait_past_first_frame(GgazeViewer *p_v) {
   for (guint u = 0; u < 3000; u++) {
      GdkTexture *p_f = ggaze_viewer_get_frame(p_v);
      if (p_f != NULL && p_f != ggaze_viewer_get_texture(p_v) &&
          green_of(p_f) < 200) {
         return (TRUE);
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   return (FALSE);
}

/* The core promise: the frames advance by themselves, and the texture the
 * rest of the app sees stays the first frame, canvas-sized, throughout. */
static void
test_animation_plays_and_keeps_first_frame_texture(void) {
   ViewerFx fx;
   fx_open_fixture(&fx, "anim.gif");
   GdkTexture *p_tex = ggaze_viewer_get_texture(fx.p_viewer);
   g_assert_nonnull(animation_lookup(p_tex));
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 8);
   g_assert_cmpuint(green_of(p_tex), >, 240);
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   g_assert_true(wait_past_first_frame(fx.p_viewer));
   g_assert_true(ggaze_viewer_get_texture(fx.p_viewer) == p_tex);
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   /* And it keeps going: another frame after this one. */
   GdkTexture *p_now =
      (GdkTexture *)g_object_ref(ggaze_viewer_get_frame(fx.p_viewer));
   for (guint u = 0; u < 3000 && ggaze_viewer_get_frame(fx.p_viewer) == p_now;
        u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_true(ggaze_viewer_get_frame(fx.p_viewer) != p_now);
   g_object_unref(p_now);
   fx_close(&fx);
}

/* Zoom and pan act on the canvas (the first frame's size) and never stop
 * the playback; the overlay hook sees the canvas geometry too. */
static void
overlay_draw_cb(GtkSnapshot *p_snap, const GgazeViewerGeom *p_geom,
                gpointer p_data) {
   (void)p_snap;
   g_assert_cmpint(p_geom->i_img_w, ==, 8);
   g_assert_cmpint(p_geom->i_img_h, ==, 6);
   (*(guint *)p_data)++;
}

static void
test_animation_survives_zoom_pan_and_overlay(void) {
   ViewerFx fx;
   fx_open_fixture(&fx, "anim.gif");
   gdouble d_fit   = ggaze_viewer_get_scale(fx.p_viewer);
   guint   u_draws = 0;
   ggaze_viewer_set_overlay(fx.p_viewer, overlay_draw_cb, NULL, &u_draws);
   ggaze_viewer_zoom_in(fx.p_viewer);
   ggaze_viewer_pan(fx.p_viewer, -10.0, -5.0);
   g_assert_true(wait_past_first_frame(fx.p_viewer));
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   g_assert_cmpfloat(ggaze_viewer_get_scale(fx.p_viewer), >=, d_fit);
   GgazeViewerGeom st_geom;
   g_assert_true(ggaze_viewer_get_geometry(fx.p_viewer, &st_geom));
   g_assert_cmpint(st_geom.i_img_w, ==, 8);
   g_assert_cmpint(st_geom.i_img_h, ==, 6);
   drain_main(150); /* a couple of frames drawn with the overlay up */
   g_assert_cmpuint(u_draws, >=, 1);
   ggaze_viewer_set_overlay(fx.p_viewer, NULL, NULL, NULL);
   fx_close(&fx);
}

/* Static images are unchanged: nothing attached, nothing scheduled, and
 * what is drawn IS the texture. */
static void
test_still_image_does_not_animate(void) {
   ViewerFx fx;
   fx_open_fixture(&fx, "plain.jpg");
   g_assert_null(animation_lookup(ggaze_viewer_get_texture(fx.p_viewer)));
   g_assert_false(ggaze_viewer_is_animating(fx.p_viewer));
   drain_main(100);
   g_assert_false(ggaze_viewer_is_animating(fx.p_viewer));
   g_assert_true(ggaze_viewer_get_frame(fx.p_viewer) ==
                 ggaze_viewer_get_texture(fx.p_viewer));
   fx_close(&fx);
}

/* Pump until the viewer's mapped state is b_mapped (or 3 s): the window's
 * stack crossfades its pages, so the large page stays mapped for the
 * length of the transition after the grid was selected. */
static void
wait_mapped(GgazeViewer *p_v, gboolean b_mapped) {
   for (guint u = 0;
        u < 3000 && gtk_widget_get_mapped(GTK_WIDGET(p_v)) != b_mapped; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(p_v)) == b_mapped);
}

/* Frames are only produced while the viewer can be seen: the grid page
 * over it stops the playback, and coming back restarts it. */
static void
test_animation_pauses_while_unmapped(void) {
   ViewerFx fx;
   fx_open_fixture(&fx, "anim.gif");
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   GtkStack *p_stack = ggaze_window_get_stack(fx.p_win);
   gtk_stack_set_visible_child_name(p_stack, "grid");
   wait_mapped(fx.p_viewer, FALSE);
   g_assert_false(ggaze_viewer_is_animating(fx.p_viewer));
   drain_main(50);
   g_assert_false(ggaze_viewer_is_animating(fx.p_viewer));
   gtk_stack_set_visible_child_name(p_stack, "large");
   wait_mapped(fx.p_viewer, TRUE);
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   g_assert_true(wait_past_first_frame(fx.p_viewer));
   fx_close(&fx);
}

/* A 0 ms frame delay plays at GGAZE_ANIM_MIN_DELAY_MS, not as fast as the
 * main loop can spin: the frames still advance, and the loop stays
 * responsive enough that a 100-iteration drain takes about its 100 ms. */
static void
test_zero_delay_plays_at_the_clamp(void) {
   ViewerFx fx;
   fx_open_fixture(&fx, "zerodelay.gif");
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   g_assert_true(wait_past_first_frame(fx.p_viewer));
   gint64 i_start = g_get_monotonic_time();
   drain_main(100);
   g_assert_cmpint(g_get_monotonic_time() - i_start, <, 1000000);
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   fx_close(&fx);
}

/* Without a window nothing is mapped, so a set_texture with an animation
 * attached schedules nothing and draws the first frame; clearing and
 * disposing with an animation attached is safe. */
static void
test_unmapped_viewer_holds_first_frame(void) {
   const gchar *c_fx   = g_getenv("GGAZE_FIXTURES_DIR");
   char        *c_path = g_build_filename(c_fx, "anim.gif", NULL);
   GFile       *p_file = g_file_new_for_path(c_path);
   GError      *p_err  = NULL;
   GdkTexture  *p_tex  = loader_load(p_file, NULL, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(animation_lookup(p_tex));

   GtkWidget *p_v = ggaze_viewer_new();
   g_object_ref_sink(p_v);
   ggaze_viewer_set_texture(GGAZE_VIEWER(p_v), p_tex);
   drain_main(60);
   g_assert_false(ggaze_viewer_is_animating(GGAZE_VIEWER(p_v)));
   g_assert_true(ggaze_viewer_get_frame(GGAZE_VIEWER(p_v)) == p_tex);
   ggaze_viewer_set_texture(GGAZE_VIEWER(p_v), NULL);
   g_assert_null(ggaze_viewer_get_frame(GGAZE_VIEWER(p_v)));
   ggaze_viewer_set_texture(GGAZE_VIEWER(p_v), p_tex);
   g_object_unref(p_v); /* disposed with the animation attached */

   g_object_unref(p_tex);
   g_object_unref(p_file);
   g_free(c_path);
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
   g_test_add_func("/viewer/zoom_in_never_shrinks_a_tiny_image",
                   test_zoom_in_never_shrinks_a_tiny_image);
   g_test_add_func("/viewer/overlay_removed_mid_drag_rebases_the_pan",
                   test_overlay_removed_mid_drag_rebases_the_pan);
   g_test_add_func("/viewer/zoom_without_texture_is_safe",
                   test_zoom_without_texture_is_safe);
   g_test_add_func("/viewer/animation_plays_and_keeps_first_frame_texture",
                   test_animation_plays_and_keeps_first_frame_texture);
   g_test_add_func("/viewer/animation_survives_zoom_pan_and_overlay",
                   test_animation_survives_zoom_pan_and_overlay);
   g_test_add_func("/viewer/still_image_does_not_animate",
                   test_still_image_does_not_animate);
   g_test_add_func("/viewer/animation_pauses_while_unmapped",
                   test_animation_pauses_while_unmapped);
   g_test_add_func("/viewer/zero_delay_plays_at_the_clamp",
                   test_zero_delay_plays_at_the_clamp);
   g_test_add_func("/viewer/unmapped_viewer_holds_first_frame",
                   test_unmapped_viewer_holds_first_frame);
   return (g_test_run());
}
