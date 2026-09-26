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
 * window puts the fit scale at about 98-100 depending on backend (the
 * viewer's share of the window differs, see gtk_helpers.h "large-view
 * readiness") -- above GGAZE_ZOOM_MAX (64) either way -- so they cannot
 * express "zooming in makes it bigger" at all.
 *
 * Every subtest that shows a picture opens its window through
 * open_and_settle(), which waits for the FULL decode inside a SETTLED
 * allocation (gtk_helpers.h "large-view readiness") before anything reads
 * a scale. A read taken off the JPEG
 * backend's 1x1 preview of plain.jpg, or off an interim allocation, disagrees
 * with every read taken after -- the load-dependent flake of 2d2.
 *
 * Needs a display (a real GgazeWindow with a realized, allocated viewer),
 * hence the `integration` suite.
 *
 * The /viewer/animation_* subtests (yb2) play tests/fixtures/anim.gif
 * through the real pipeline and assert what the viewer contract promises:
 * the frames advance on their own while the widget is mapped, the first
 * frame stays THE texture (what the cache, the histogram and hold-Space
 * compare against), zoom / pan / the overlay geometry are the canvas's,
 * with the same pixels after frames played, a still never animates (nor
 * does one set after an animation), an unmapped viewer (the grid page)
 * plays nothing and resumes on remap, a tool's hold keeps the first frame
 * up, a 10 ms delay plays at the 20 ms clamp instead of spinning, a file
 * that plays once (GIF without a loop extension, WebP ANIM count 1) ends
 * on its last frame, and a slow animation keeps the frame clock ticking
 * only near its frame changes (ggaze_viewer_get_tick_count).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "window.h"

#include "ggaze-config.h"
#include "gtk_helpers.h"
#include "loader/animation.h"
#include "loader/loader.h"
#include "logical-size.h"
#include "pixbuf_modules.h"
#include "viewer.h"

#include <math.h>

#include <gio/gio.h>
#include <glib/gstdio.h> /* g_remove / g_rmdir */
#include <gtk/gtk.h>

/* Comfortably larger than the test window, so fit-to-window is well below 1.0
 * and a zoom step in either direction stays inside the clamp range. */
#define FIXTURE_W 1200
#define FIXTURE_H 800

/* tests/fixtures/plain.jpg, the tiny fixture of the jx0 subtest. Its size is
 * what the readiness wait matches the viewer's texture against, so it must be
 * the FULL-DECODE size: the JPEG backend's two-phase load (loader/backends/
 * jpeg.c) shows a 1/8-scale preview first, which for 6x3 is 1x1. */
#define PLAIN_JPG_W 6
#define PLAIN_JPG_H 3

/* The canvas (and so the first frame's size) of every animated fixture
 * (tests/fixtures/gen.py). */
#define ANIM_W 8
#define ANIM_H 6

/* Present p_win at the 600x400 the header comment's numbers assume, open
 * p_file on it and wait until the viewer shows the i_tex_w x i_tex_h decode
 * inside its settled allocation (gtk_helpers.h "large-view readiness").
 * Its callers go on to read the viewer's scale or pan (directly or through
 * a zoom/pan/fit step), and those are meaningless off an unallocated viewer
 * (_compute_geom's zero-size guard returns a fit ratio of 1.0) and WRONG off
 * the JPEG backend's 1/8-scale preview -- the flake of 2d2, where the 1x1
 * preview's fit (about 344) was read in place of the file's (about 98).
 * Returns the viewer (borrowed). */
static GgazeViewer *
open_and_settle(GgazeWindow *p_win, GFile *p_file, int i_tex_w, int i_tex_h) {
   gtk_window_set_default_size(GTK_WINDOW(p_win), 600, 400);
   gtk_window_present(GTK_WINDOW(p_win));
   ggaze_window_open(p_win, p_file);
   return (GGTEST_WAIT_FOR_VIEW(p_win, i_tex_w, i_tex_h));
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

/* The generated PNG goes through the pixbuf backend, which delivers no
 * partial, so here the wait's texture-size match only pins the decode; the
 * settled allocation is what every scale assertion below leans on. */
static void
fx_open(ViewerFx *p_fx) {
   p_fx->c_dir         = make_fixture_dir(&p_fx->c_img);
   GFile       *p_file = g_file_new_for_path(p_fx->c_img);
   GgazeWindow *p_win  = GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL));
   p_fx->p_win         = p_win;
   p_fx->p_viewer      = open_and_settle(p_win, p_file, FIXTURE_W, FIXTURE_H);
   g_object_unref(p_file);
}

static void
fx_close(ViewerFx *p_fx) {
   gtk_window_destroy(GTK_WINDOW(p_fx->p_win));
   ggtest_drain_main(200);
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
   ggtest_drain_main(50);
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
   ggtest_drain_main(50);
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

/* 8l2: a scaled-down texture that stands for a larger image
 * (logical-size.h, the enhance preview's) is laid out as that image. Put
 * over the 1200x800 fixture at a quarter of its pixels, the viewer keeps
 * the same fit scale and the same on-screen rectangle, reports the IMAGE's
 * size in its geometry (what the crop / straighten tools measure in), and
 * its 100 % is one image pixel per widget pixel -- the preview magnified,
 * not a 300x200 picture at 1:1. */
static void
test_scaled_texture_keeps_the_image_geometry(void) {
   ViewerFx fx;
   fx_open(&fx);
   GgazeViewerGeom t_full, t_small;
   g_assert_true(ggaze_viewer_get_geometry(fx.p_viewer, &t_full));
   gdouble d_fit = ggaze_viewer_get_scale(fx.p_viewer);

   gsize   u_len = 300 * 200 * 4;
   GBytes *p_b   = g_bytes_new_take(g_malloc0(u_len), u_len);

   GdkTexture *p_tex =
      gdk_memory_texture_new(300, 200, GDK_MEMORY_R8G8B8A8, p_b, 300 * 4);
   g_bytes_unref(p_b);
   logical_size_set(p_tex, FIXTURE_W, FIXTURE_H);
   ggaze_viewer_set_texture(fx.p_viewer, p_tex);
   g_assert_true(ggaze_viewer_get_geometry(fx.p_viewer, &t_small));
   g_assert_cmpint(t_small.i_img_w, ==, FIXTURE_W);
   g_assert_cmpint(t_small.i_img_h, ==, FIXTURE_H);
   g_assert_cmpfloat(ABS(t_small.d_scale - t_full.d_scale), <, 1e-9);
   g_assert_cmpfloat(ABS(t_small.d_x - t_full.d_x), <, 1e-9);
   g_assert_cmpfloat(ABS(t_small.d_y - t_full.d_y), <, 1e-9);
   g_assert_cmpfloat(ABS(ggaze_viewer_get_scale(fx.p_viewer) - d_fit), <, 1e-9);
   ggaze_viewer_toggle_fit_100(fx.p_viewer);
   g_assert_true(ggaze_viewer_get_geometry(fx.p_viewer, &t_small));
   g_assert_cmpfloat(ABS(t_small.d_scale - 1.0), <, 1e-9);
   g_assert_cmpint(t_small.i_img_w, ==, FIXTURE_W); /* still the image */
   g_assert_cmpint(gdk_texture_get_width(ggaze_viewer_get_texture(fx.p_viewer)),
                   ==, 300);
   g_object_unref(p_tex);
   fx_close(&fx);
}

static void
count_emission(GgazeViewer *p_v, gpointer p_data) {
   (void)p_v;
   (*(guint *)p_data)++;
}

/* 8l2 review: how magnified the texture's own pixels are drawn -- device
 * pixels per texture pixel -- and the "zoom-changed" signal the window
 * watches it by. A quarter-resolution texture standing for the image is
 * magnified 4x sooner than the full one; every zoom-state change emits
 * (a new texture, the 100 % toggle, a zoom), a pan does not; no texture
 * reads 0. */
static void
test_texel_scale_follows_the_zoom(void) {
   ViewerFx fx;
   fx_open(&fx);
   guint u_emits = 0;
   g_signal_connect(fx.p_viewer, "zoom-changed", G_CALLBACK(count_emission),
                    &u_emits);
   gdouble d_sf  = gtk_widget_get_scale_factor(GTK_WIDGET(fx.p_viewer));
   gdouble d_fit = ggaze_viewer_get_scale(fx.p_viewer);
   g_assert_true(ggaze_viewer_is_fit(fx.p_viewer));
   g_assert_cmpfloat_with_epsilon(ggaze_viewer_get_texel_scale(fx.p_viewer),
                                  d_fit * d_sf, 1e-9);
   gsize       u_len = 300 * 200 * 4;
   GBytes     *p_b   = g_bytes_new_take(g_malloc0(u_len), u_len);
   GdkTexture *p_tex =
      gdk_memory_texture_new(300, 200, GDK_MEMORY_R8G8B8A8, p_b, 300 * 4);
   g_bytes_unref(p_b);
   logical_size_set(p_tex, FIXTURE_W, FIXTURE_H);
   ggaze_viewer_set_texture(fx.p_viewer, p_tex);
   g_assert_cmpuint(u_emits, ==, 1);
   g_assert_cmpfloat_with_epsilon(ggaze_viewer_get_texel_scale(fx.p_viewer),
                                  d_fit * d_sf * 4.0, 1e-9);
   ggaze_viewer_toggle_fit_100(fx.p_viewer);
   g_assert_cmpuint(u_emits, ==, 2);
   g_assert_false(ggaze_viewer_is_fit(fx.p_viewer));
   g_assert_cmpfloat_with_epsilon(ggaze_viewer_get_texel_scale(fx.p_viewer),
                                  d_sf * 4.0, 1e-9);
   ggaze_viewer_zoom_in(fx.p_viewer);
   g_assert_cmpuint(u_emits, ==, 3);
   g_assert_cmpfloat_with_epsilon(ggaze_viewer_get_texel_scale(fx.p_viewer),
                                  d_sf * 4.0 * 1.25, 1e-9);
   ggaze_viewer_pan(fx.p_viewer, 5.0, 5.0);
   g_assert_cmpuint(u_emits, ==, 3); /* a pan is not a zoom */
   ggaze_viewer_set_texture(fx.p_viewer, NULL);
   g_assert_cmpfloat(ggaze_viewer_get_texel_scale(fx.p_viewer), ==, 0.0);
   g_object_unref(p_tex);
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
   ggtest_drain_main(50);

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
   ggtest_drain_main(50);
   gdouble d_scale = ggaze_viewer_get_scale(fx.p_viewer);
   ggaze_viewer_pan(fx.p_viewer, -40.0, -30.0);
   ggtest_drain_main(50);
   g_assert_true(isfinite(ggaze_viewer_get_scale(fx.p_viewer)));
   g_assert_cmpfloat(ABS(ggaze_viewer_get_scale(fx.p_viewer) - d_scale), <,
                     0.0001);

   fx_close(&fx);
}

/* jx0: an image small enough that fit-to-window already exceeds
 * GGAZE_ZOOM_MAX. tests/fixtures/plain.jpg is 6x3, so in this 600x400 window
 * it fits at about 98-100x depending on backend (roughly 590-600 px of
 * viewer width over 6) -- above the 64x ceiling. Zooming in must never make
 * such a picture SMALLER, which is exactly what clamping to a bare
 * GGAZE_ZOOM_MAX did (scale went from the fit down to 64 on the first
 * win.zoom-in). At the top end a no-op is correct; a reversal is not.
 *
 * The wait in open_and_settle matches the texture's size against the file's
 * (2d2): the JPEG backend shows a 1/8-scale preview before the full decode,
 * and plain.jpg's 1x1 preview fits at roughly 344x (backend-dependent, like
 * the 98). A d_fit read off that preview passed the premise below, and the
 * real decode landing a millisecond later then read as a shrink -- a flake
 * whose window was the gap between the two textures, which only a starved
 * worker thread on a loaded lane made wide enough to hit. The minimal lane
 * (jpeg disabled) never showed a preview and never flaked. */
static void
test_zoom_in_never_shrinks_a_tiny_image(void) {
   const gchar *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char        *c_path = g_build_filename(c_fx, "plain.jpg", NULL);
   GFile       *p_file = g_file_new_for_path(c_path);
   GgazeWindow *p_win  = GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL));
   GgazeViewer *p_v = open_and_settle(p_win, p_file, PLAIN_JPG_W, PLAIN_JPG_H);

   /* Guard the premise: if this fixture ever stops fitting above the ceiling
    * the test would silently stop covering jx0. */
   gdouble d_fit = ggaze_viewer_get_scale(p_v);
   g_assert_cmpfloat(d_fit, >, 64.0);

   ggaze_viewer_zoom_in(p_v);
   ggtest_drain_main(50);
   g_assert_cmpfloat(ggaze_viewer_get_scale(p_v), >=, d_fit);

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   ggtest_drain_main(200);
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
 * window through open_and_settle(): the shared view wait (gtk_helpers.h
 * "large-view readiness") pins the i_w x i_h decode inside a settled
 * allocation, which the scale reads below lean on. */
static void
fx_open_fixture(ViewerFx *p_fx, const char *c_fixture, int i_w, int i_h) {
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

   p_fx->p_win    = GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL));
   p_fx->p_viewer = open_and_settle(p_fx->p_win, p_dst, i_w, i_h);
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

/* A copy of p_tex's pixels (premultiplied BGRA, tightly packed). */
static guint8 *
pixels_of(GdkTexture *p_tex, gsize *p_len) {
   gsize u_stride = 4u * (gsize)gdk_texture_get_width(p_tex);
   *p_len         = u_stride * (gsize)gdk_texture_get_height(p_tex);
   guint8 *p_px   = g_malloc0(*p_len);
   gdk_texture_download(p_tex, p_px, u_stride);
   return (p_px);
}

/* Pump until the frame on screen is another texture than p_from (or 3 s).
 * TRUE when it happened. */
static gboolean
wait_frame_change(GgazeViewer *p_v, GdkTexture *p_from) {
   for (guint u = 0; u < 3000; u++) {
      if (ggaze_viewer_get_frame(p_v) != p_from) {
         return (TRUE);
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   return (FALSE);
}

/* The core promise: the frames advance by themselves, and the texture the
 * rest of the app sees stays the first frame, canvas-sized, throughout --
 * the same object AND the same pixels. The pixel compare is review finding
 * 1 of yb2: on gdk-pixbuf 2.42 the first-frame texture used to share the
 * GIF loader's composite buffer, so after a few frames it read back as
 * whichever frame was drawn last. Six changes cover every frame of the
 * four-frame fixture at least once. */
static void
test_animation_plays_and_keeps_first_frame_texture(void) {
   ViewerFx fx;
   fx_open_fixture(&fx, "anim.gif", ANIM_W, ANIM_H);
   GdkTexture *p_tex = ggaze_viewer_get_texture(fx.p_viewer);
   g_assert_nonnull(animation_lookup(p_tex));
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, ANIM_W);
   g_assert_cmpuint(green_of(p_tex), >, 240);
   gsize   u_len    = 0;
   guint8 *p_before = pixels_of(p_tex, &u_len);
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   g_assert_true(wait_past_first_frame(fx.p_viewer));
   for (guint u = 0; u < 6; u++) {
      GdkTexture *p_now = ggaze_viewer_get_frame(fx.p_viewer);
      g_assert_true(wait_frame_change(fx.p_viewer, p_now));
   }
   g_assert_true(ggaze_viewer_get_texture(fx.p_viewer) == p_tex);
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   gsize   u_len_after = 0;
   guint8 *p_after     = pixels_of(p_tex, &u_len_after);
   g_assert_cmpmem(p_before, u_len, p_after, u_len_after);
   g_free(p_before);
   g_free(p_after);
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
   fx_open_fixture(&fx, "anim.gif", ANIM_W, ANIM_H);
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
   ggtest_drain_main(150); /* a couple of frames drawn with the overlay up */
   g_assert_cmpuint(u_draws, >=, 1);
   ggaze_viewer_set_overlay(fx.p_viewer, NULL, NULL, NULL);
   fx_close(&fx);
}

/* Static images are unchanged: nothing attached, nothing scheduled, and
 * what is drawn IS the texture. */
static void
test_still_image_does_not_animate(void) {
   ViewerFx fx;
   fx_open_fixture(&fx, "plain.jpg", PLAIN_JPG_W, PLAIN_JPG_H);
   g_assert_null(animation_lookup(ggaze_viewer_get_texture(fx.p_viewer)));
   g_assert_false(ggaze_viewer_is_animating(fx.p_viewer));
   ggtest_drain_main(100);
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
   fx_open_fixture(&fx, "anim.gif", ANIM_W, ANIM_H);
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   GtkStack *p_stack = ggaze_window_get_stack(fx.p_win);
   gtk_stack_set_visible_child_name(p_stack, "grid");
   wait_mapped(fx.p_viewer, FALSE);
   g_assert_false(ggaze_viewer_is_animating(fx.p_viewer));
   ggtest_drain_main(50);
   g_assert_false(ggaze_viewer_is_animating(fx.p_viewer));
   gtk_stack_set_visible_child_name(p_stack, "large");
   wait_mapped(fx.p_viewer, TRUE);
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   g_assert_true(wait_past_first_frame(fx.p_viewer));
   fx_close(&fx);
}

/* Pump the main loop for u_ms of the MONOTONIC clock and count how
 * often the frame on screen changes; *p_window_ms gets the window's real
 * length (an iteration can overrun under load) and *p_ticks how many
 * times the viewer's tick callback ran in it. A window measured by the
 * clock, and changes counted along the way, is what keeps these tests
 * honest on a loaded machine: a fixed-iteration drain stretches to
 * seconds there, and comparing only the first and last frame of a
 * LOOPING animation can land on the same frame by chance. */
static guint
count_frame_changes(GgazeViewer *p_v, guint u_ms, gint64 *p_window_ms,
                    guint *p_ticks) {
   guint       u_changes = 0;
   guint       u_ticks0  = ggaze_viewer_get_tick_count(p_v);
   GdkTexture *p_last    = ggaze_viewer_get_frame(p_v);
   gint64      i_start   = g_get_monotonic_time();
   while (g_get_monotonic_time() - i_start < (gint64)u_ms * 1000) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      GdkTexture *p_now = ggaze_viewer_get_frame(p_v);
      if (p_now != p_last) {
         u_changes++;
         p_last = p_now;
      }
      g_usleep(500);
   }
   *p_window_ms = (g_get_monotonic_time() - i_start) / 1000;
   *p_ticks     = ggaze_viewer_get_tick_count(p_v) - u_ticks0;
   return (u_changes);
}

/* A 10 ms frame delay (fastdelay.gif; glycin hands it over as 10 ms,
 * gdk-pixbuf 2.42 raises it to 20 itself) plays at GGAZE_ANIM_MIN_DELAY_MS,
 * not as fast as the frame clock ticks: over a window of W ms the frame
 * on screen changes at most W / 20 ms + 2 times -- the dues are at least
 * the clamp apart, plus one for the fencepost and one for a first change
 * whose due fell up to a tick before the window opened. The frames must
 * also keep changing (at least one change), or "no spin" would be
 * satisfied by not playing at all.
 *
 * An unclamped player would change the frame on every tick, so the bound
 * only tells the two apart when the frame clock ticked MORE often than
 * the bound allows -- above 50 Hz. The tick callback stays on the clock
 * for the whole window (20 ms is inside GGAZE_ANIM_TICK_LEAD_MS), so the
 * viewer's tick count IS the clock's measured rate; at or below 50 Hz (a
 * starved main loop, a slow virtual display) the discriminating assertion
 * is skipped with a message. The clamp itself is proven by the unit tests
 * (tests/test_animation.c, animation_frame_delay_ms); this subtest shows
 * the viewer schedules by the clamped delay rather than per tick. (A 0 ms
 * delay is no test of the clamp: the decoders make it 100 ms.) */
static void
test_fast_delay_plays_at_the_clamp(void) {
   ViewerFx fx;
   fx_open_fixture(&fx, "fastdelay.gif", ANIM_W, ANIM_H);
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   g_assert_true(wait_past_first_frame(fx.p_viewer));
   gint64 i_window_ms = 0;
   guint  u_ticks     = 0;
   guint  u_changes =
      count_frame_changes(fx.p_viewer, 1000, &i_window_ms, &u_ticks);
   guint u_bound = (guint)(i_window_ms / GGAZE_ANIM_MIN_DELAY_MS) + 2u;
   g_test_message("%u changes, %u ticks in %" G_GINT64_FORMAT " ms", u_changes,
                  u_ticks, i_window_ms);
   g_assert_cmpuint(u_changes, >=, 1);
   if (u_ticks > u_bound) {
      g_assert_cmpuint(u_changes, <=, u_bound);
   } else {
      g_test_message("frame clock ticked %u times, not above the clamp "
                     "bound of %u: clamped and unclamped look alike here",
                     u_ticks, u_bound);
   }
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   fx_close(&fx);
}

/* Pump until nothing is scheduled any more (or 3 s). TRUE when it
 * happened. */
static gboolean
wait_animation_ends(GgazeViewer *p_v) {
   for (guint u = 0; u < 3000 && ggaze_viewer_is_animating(p_v); u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   return (!ggaze_viewer_is_animating(p_v));
}

/* A file that plays once -- c_fixture: four 100 ms frames -- ends on its
 * LAST frame (green 75, tests/fixtures/gen.py) after ~400 ms and stays
 * there with nothing scheduled, the texture still the first frame
 * (review finding 1 of yb2: a GIF without a loop extension used to loop
 * for ever). A remap would play it once more (viewer.c). */
static void
assert_plays_once(ViewerFx *p_fx) {
   GdkTexture *p_tex = ggaze_viewer_get_texture(p_fx->p_viewer);
   g_assert_cmpuint(animation_get_plays(animation_lookup(p_tex)), ==, 1);
   g_assert_true(wait_animation_ends(p_fx->p_viewer));
   GdkTexture *p_last = ggaze_viewer_get_frame(p_fx->p_viewer);
   g_assert_true(p_last == animation_get_frame(animation_lookup(p_tex), 3));
   g_assert_cmpuint(ABS((gint)green_of(p_last) - 75), <=, 2);
   ggtest_drain_main(300); /* three more frame periods: nothing moves */
   g_assert_false(ggaze_viewer_is_animating(p_fx->p_viewer));
   g_assert_true(ggaze_viewer_get_frame(p_fx->p_viewer) == p_last);
   g_assert_true(ggaze_viewer_get_texture(p_fx->p_viewer) == p_tex);
}

static void
test_play_once_gif_holds_last_frame(void) {
   ViewerFx fx;
   fx_open_fixture(&fx, "once.gif", ANIM_W, ANIM_H);
   assert_plays_once(&fx);
   fx_close(&fx);
}

/* The same through a WebP's ANIM count of 1, where the machine's webp
 * module decodes frames (optional, like everywhere in the suites). With no
 * webp module at all (CI's fedora:40 installs no webp-pixbuf-loader) the
 * load can only fail, so the skip must come BEFORE the open: the readiness
 * wait inside fx_open_fixture() would otherwise time out and abort. */
static void
test_play_once_webp_holds_last_frame(void) {
   if (!ggtest_pixbuf_module_available("webp")) {
      g_test_skip("no webp gdk-pixbuf module here");
      return;
   }
   ViewerFx fx;
   fx_open_fixture(&fx, "once.webp", ANIM_W, ANIM_H);
   if (animation_lookup(ggaze_viewer_get_texture(fx.p_viewer)) == NULL) {
      g_test_skip("no frame-capable webp gdk-pixbuf module here");
   } else {
      assert_plays_once(&fx);
   }
   fx_close(&fx);
}

/* Between the frames of a slow animation (slow.gif: 500 ms) the tick
 * callback is off the frame clock (review finding 4 of yb2: a tick
 * callback makes GDK draw every vblank). Over a window of W ms at most
 * W / 500 + 2 frames fall due, and each keeps the tick for at most the
 * GGAZE_ANIM_TICK_LEAD_MS of 40 ms before it plus the due tick: a handful
 * of ticks per frame even on a 120 Hz clock, so the bound below is eight
 * per frame -- where a tick on every vblank of a 60 Hz clock would be
 * about 30 per frame period. W is the MEASURED window, not the nominal
 * 1200 ms, so a window that overran under load raises the bound only by
 * the frames that really fell due in it and the ratio -- eight ticks per
 * 500 ms against thirty -- stays what the assertion discriminates on.
 * At least one frame must change in the window (with frames 500 ms apart
 * two fall due in any 1200 ms), so "few ticks" cannot be satisfied by not
 * playing; the changes are counted as they happen, since the looping
 * clip may be back on the frame it started from when the window ends. */
static void
test_slow_animation_ticks_only_near_frames(void) {
   ViewerFx fx;
   fx_open_fixture(&fx, "slow.gif", ANIM_W, ANIM_H);
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   gint64 i_window_ms = 0;
   guint  u_ticks     = 0;
   guint  u_changes =
      count_frame_changes(fx.p_viewer, 1200, &i_window_ms, &u_ticks);
   g_test_message("%u changes, %u ticks in %" G_GINT64_FORMAT " ms", u_changes,
                  u_ticks, i_window_ms);
   g_assert_cmpuint(u_changes, >=, 1);
   g_assert_cmpuint(u_ticks, >=, 1);
   g_assert_cmpuint(u_ticks, <=, ((guint)(i_window_ms / 500) + 2u) * 8u);
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   fx_close(&fx);
}

/* A still set after an animation stops the playback: nothing scheduled,
 * and what is drawn is the still itself, not a frame of the animation
 * that played before. */
static void
test_still_after_animation_stops_playback(void) {
   ViewerFx fx;
   fx_open_fixture(&fx, "anim.gif", ANIM_W, ANIM_H);
   g_assert_true(wait_past_first_frame(fx.p_viewer));
   GdkTexture *p_still = gdk_texture_new_from_filename(fx.c_img, NULL);
   g_assert_nonnull(p_still);
   g_assert_null(animation_lookup(p_still)); /* GDK's own decode */
   ggaze_viewer_set_texture(fx.p_viewer, p_still);
   g_assert_false(ggaze_viewer_is_animating(fx.p_viewer));
   ggtest_drain_main(150);
   g_assert_false(ggaze_viewer_is_animating(fx.p_viewer));
   g_assert_true(ggaze_viewer_get_frame(fx.p_viewer) == p_still);
   g_object_unref(p_still);
   fx_close(&fx);
}

/* A tool's hold: playback stops on the FIRST frame at once, stays there
 * across a set_texture of the same animation (a render landing under the
 * tool), and resumes from the first frame on release. */
static void
test_hold_first_frame_pauses_and_resumes(void) {
   ViewerFx fx;
   fx_open_fixture(&fx, "anim.gif", ANIM_W, ANIM_H);
   GdkTexture *p_tex = ggaze_viewer_get_texture(fx.p_viewer);
   g_assert_true(wait_past_first_frame(fx.p_viewer));
   ggaze_viewer_hold_first_frame(fx.p_viewer, TRUE);
   g_assert_false(ggaze_viewer_is_animating(fx.p_viewer));
   g_assert_true(ggaze_viewer_get_frame(fx.p_viewer) == p_tex);
   ggaze_viewer_set_texture(fx.p_viewer, p_tex);
   ggtest_drain_main(250); /* more than two frame periods */
   g_assert_false(ggaze_viewer_is_animating(fx.p_viewer));
   g_assert_true(ggaze_viewer_get_frame(fx.p_viewer) == p_tex);
   ggaze_viewer_hold_first_frame(fx.p_viewer, FALSE);
   g_assert_true(ggaze_viewer_is_animating(fx.p_viewer));
   g_assert_true(ggaze_viewer_get_frame(fx.p_viewer) == p_tex);
   g_assert_true(wait_past_first_frame(fx.p_viewer));
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
   ggtest_drain_main(60);
   g_assert_false(ggaze_viewer_is_animating(GGAZE_VIEWER(p_v)));
   g_assert_true(ggaze_viewer_get_frame(GGAZE_VIEWER(p_v)) == p_tex);
   /* A hold and its release do not start anything on an unmapped viewer
    * either. */
   ggaze_viewer_hold_first_frame(GGAZE_VIEWER(p_v), TRUE);
   ggaze_viewer_hold_first_frame(GGAZE_VIEWER(p_v), FALSE);
   g_assert_false(ggaze_viewer_is_animating(GGAZE_VIEWER(p_v)));
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

/* The yb2 animation subtests, registered apart so main() stays short
 * (c-best-practices). */
static void
_add_animation_tests(void) {
   g_test_add_func("/viewer/animation_plays_and_keeps_first_frame_texture",
                   test_animation_plays_and_keeps_first_frame_texture);
   g_test_add_func("/viewer/animation_survives_zoom_pan_and_overlay",
                   test_animation_survives_zoom_pan_and_overlay);
   g_test_add_func("/viewer/still_image_does_not_animate",
                   test_still_image_does_not_animate);
   g_test_add_func("/viewer/animation_pauses_while_unmapped",
                   test_animation_pauses_while_unmapped);
   g_test_add_func("/viewer/fast_delay_plays_at_the_clamp",
                   test_fast_delay_plays_at_the_clamp);
   g_test_add_func("/viewer/play_once_gif_holds_last_frame",
                   test_play_once_gif_holds_last_frame);
   g_test_add_func("/viewer/play_once_webp_holds_last_frame",
                   test_play_once_webp_holds_last_frame);
   g_test_add_func("/viewer/slow_animation_ticks_only_near_frames",
                   test_slow_animation_ticks_only_near_frames);
   g_test_add_func("/viewer/still_after_animation_stops_playback",
                   test_still_after_animation_stops_playback);
   g_test_add_func("/viewer/hold_first_frame_pauses_and_resumes",
                   test_hold_first_frame_pauses_and_resumes);
   g_test_add_func("/viewer/unmapped_viewer_holds_first_frame",
                   test_unmapped_viewer_holds_first_frame);
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
   g_test_add_func("/viewer/scaled_texture_keeps_the_image_geometry",
                   test_scaled_texture_keeps_the_image_geometry);
   g_test_add_func("/viewer/texel_scale_follows_the_zoom",
                   test_texel_scale_follows_the_zoom);
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
   _add_animation_tests();
   return (g_test_run());
}
