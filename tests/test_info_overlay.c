/*:*
 * ggaze — info overlay + histogram view direct test (integration)
 *
 * Drives the InfoOverlay on a bare GtkOverlay in a never-presented
 * GtkWindow, without a GgazeWindow in between, so the paths the window
 * suites cannot reach on any lane (0c2 third review) execute for real:
 *
 *   - a plot-only refresh landing on a card that is no longer that file's
 *     (a status line took the card over while the binning ran) is dropped,
 *   - texture_changed(NULL) on a plotted card takes the plot down at once
 *     and cancels the binning so nothing lands later,
 *   - the auto-hide timer takes a file card and its plot down,
 *   - show_for_file / texture_changed / show_status on a disposed overlay are
 *     no-ops, and a request in flight at dispose time is dropped;
 *
 * and the GgazeHistogramView draw path (measure, snapshot, _draw_channel)
 * runs in a PRESENTED window in the Xvfb lane, with the render tree it
 * produces asserted node by node: one floor rect plus exactly one colour
 * node per non-empty bin, nothing at all for no histogram or an all-zero one.
 *
 * Textures are hand-built GdkMemoryTextures (as in tests/test_histogram.c)
 * so the expected bins are known exactly; the file is the committed
 * plain.jpg fixture, read-only, because info_new() wants a real file.
 *
 * Display-gated like every integration suite: main() returns 77 without one.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "info-overlay.h"

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

#include "histogram-view.h"
#include "histogram.h"

/* Bin index of an 8-bit value, the same mapping histogram.c uses. */
#define BIN(v) ((v) * HISTOGRAM_BINS / 256)

/* The fixture the card describes (read-only): 6x3 plain.jpg. */
static GFile *
fixture_file(void) {
   const gchar *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char  *c_path = g_build_filename(c_fx, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);
   return (p_file);
}

/* A 2x2 RGBA texture of one solid colour: every channel bins into exactly
 * one bucket with 4 samples, so a plot's provenance is one comparison. */
static GdkTexture *
solid_texture(guint8 u_r, guint8 u_g, guint8 u_b) {
   guint8 a_px[16];
   for (guint u = 0; u < 4; u++) {
      a_px[u * 4 + 0] = u_r;
      a_px[u * 4 + 1] = u_g;
      a_px[u * 4 + 2] = u_b;
      a_px[u * 4 + 3] = 255;
   }
   GBytes     *p_b = g_bytes_new(a_px, sizeof(a_px));
   GdkTexture *p_t =
      gdk_memory_texture_new(2, 2, GDK_MEMORY_R8G8B8A8, p_b, 2 * 4);
   g_bytes_unref(p_b);
   return (p_t);
}

/* Assert p_hist is the histogram of solid_texture(u_r, u_g, u_b): 4 samples
 * and each primary piled into its own bin. */
static void
assert_solid_bins(const Histogram *p_hist, guint8 u_r, guint8 u_g, guint8 u_b) {
   g_assert_nonnull(p_hist);
   g_assert_cmpuint(p_hist->u_samples, ==, 4);
   g_assert_cmpuint(p_hist->u_peak, ==, 4);
   g_assert_cmpuint(p_hist->u_bins[HISTOGRAM_CHANNEL_R][BIN(u_r)], ==, 4);
   g_assert_cmpuint(p_hist->u_bins[HISTOGRAM_CHANNEL_G][BIN(u_g)], ==, 4);
   g_assert_cmpuint(p_hist->u_bins[HISTOGRAM_CHANNEL_B][BIN(u_b)], ==, 4);
}

/* Iterate the main context for roughly u_ms milliseconds (a settle, not a
 * wait: the assertions that follow are the check). */
static void
drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

/* The overlay under test plus the toplevel that hosts it. The window is
 * never presented (nothing reaches a desktop), except by the draw subtest
 * which needs a real allocation. Torn down with gtk_window_destroy() --
 * tests/helpers/gtk_helpers.h, "window teardown". */
typedef struct {
   GtkWidget   *p_win;
   GtkWidget   *p_overlay;
   InfoOverlay *p_io;
   GFile       *p_file;
} OverlayFx;

static void
fx_open(OverlayFx *p_fx) {
   p_fx->p_win     = gtk_window_new();
   p_fx->p_overlay = gtk_overlay_new();
   gtk_overlay_set_child(GTK_OVERLAY(p_fx->p_overlay), gtk_label_new("x"));
   gtk_window_set_child(GTK_WINDOW(p_fx->p_win), p_fx->p_overlay);
   p_fx->p_io   = info_overlay_new(GTK_OVERLAY(p_fx->p_overlay));
   p_fx->p_file = fixture_file();
   g_assert_nonnull(p_fx->p_io);
}

/* Mirrors the window's teardown order: dispose before the widgets go
 * (no widget touch afterwards), delete once nothing can call back. */
static void
fx_close(OverlayFx *p_fx) {
   info_overlay_dispose(p_fx->p_io);
   gtk_window_destroy(GTK_WINDOW(p_fx->p_win));
   drain_main(100);
   info_overlay_delete(p_fx->p_io);
   g_object_unref(p_fx->p_file);
}

static GtkWidget *
label_of(OverlayFx *p_fx) {
   return (info_overlay_get_label(p_fx->p_io));
}

static const Histogram *
plot_of(OverlayFx *p_fx) {
   GtkWidget *p_plot = info_overlay_get_histogram(p_fx->p_io);
   return (ggaze_histogram_view_get_histogram(GGAZE_HISTOGRAM_VIEW(p_plot)));
}

/* Drain until the plot holds a histogram (the gather is async), 3 s max. */
static const Histogram *
wait_for_plot(OverlayFx *p_fx) {
   for (guint u = 0; u < 3000 && plot_of(p_fx) == NULL; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(plot_of(p_fx));
   g_assert_true(
      gtk_widget_get_visible(info_overlay_get_histogram(p_fx->p_io)));
   g_assert_true(gtk_widget_get_visible(label_of(p_fx)));
   return (plot_of(p_fx));
}

/* The plot is down and holds nothing (the card may still be up). */
static void
assert_no_plot(OverlayFx *p_fx) {
   g_assert_null(plot_of(p_fx));
   g_assert_false(
      gtk_widget_get_visible(info_overlay_get_histogram(p_fx->p_io)));
}

/* show_for_file with a texture brings the card up with that texture's
 * bins; a second texture_changed re-bins the new texture under the same
 * text; a repeat of the texture already plotted changes nothing. */
static void
test_show_then_follow_texture(void) {
   OverlayFx   fx;
   GdkTexture *p_red = solid_texture(255, 0, 0);
   GdkTexture *p_grn = solid_texture(0, 255, 0);
   fx_open(&fx);
   assert_no_plot(&fx);

   info_overlay_show_for_file(fx.p_io, fx.p_file, p_red);
   assert_solid_bins(wait_for_plot(&fx), 255, 0, 0);
   /* The card's text is the file's (info_format prints the stored
    * dimensions; plain.jpg is 6x3), not a leftover. */
   const char *c_text = gtk_label_get_text(GTK_LABEL(label_of(&fx)));
   g_assert_nonnull(g_strstr_len(c_text, -1, "6\u00d73"));

   info_overlay_texture_changed(fx.p_io, p_grn);
   assert_no_plot(&fx); /* the red plot is gone at once ... */
   g_assert_true(gtk_widget_get_visible(label_of(&fx)));
   assert_solid_bins(wait_for_plot(&fx), 0, 255, 0); /* ... green lands */

   const Histogram *p_same = plot_of(&fx);
   info_overlay_texture_changed(fx.p_io, p_grn); /* already plotting it */
   /* Checked BEFORE any drain: a re-bin clears the plot synchronously, so
    * a non-NULL plot right here proves none started. The identity check
    * after the drain alone would not -- a freed-and-reallocated Histogram
    * could land on the same address. */
   g_assert_nonnull(plot_of(&fx));
   drain_main(100);
   g_assert_true(plot_of(&fx) == p_same);

   g_object_unref(p_red);
   g_object_unref(p_grn);
   fx_close(&fx);
}

/* A plot-only refresh whose binning is still running when a status line
 * takes the card over must be dropped when it lands: the card is no longer
 * the file's, and a plot must not come up under "Copied image". */
static void
test_status_drops_landing_plot(void) {
   OverlayFx   fx;
   GdkTexture *p_red = solid_texture(255, 0, 0);
   GdkTexture *p_blu = solid_texture(0, 0, 255);
   fx_open(&fx);

   info_overlay_show_for_file(fx.p_io, fx.p_file, p_red);
   assert_solid_bins(wait_for_plot(&fx), 255, 0, 0);

   info_overlay_texture_changed(fx.p_io, p_blu);      /* binning starts ... */
   info_overlay_show_status(fx.p_io, "Copied image"); /* ... card taken */
   drain_main(300); /* the blue binning lands in here and is dropped */
   g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label_of(&fx))), ==,
                   "Copied image");
   g_assert_true(gtk_widget_get_visible(label_of(&fx)));
   assert_no_plot(&fx);

   /* A texture change under the status line is ignored too. */
   info_overlay_texture_changed(fx.p_io, p_red);
   drain_main(100);
   assert_no_plot(&fx);

   g_object_unref(p_red);
   g_object_unref(p_blu);
   fx_close(&fx);
}

/* texture_changed(NULL) on a plotted card (the screen shows nothing the
 * card may plot any more) takes the plot down at once and cancels a binning
 * still in flight, so nothing lands later; the text stays. */
static void
test_null_texture_clears_and_cancels(void) {
   OverlayFx   fx;
   GdkTexture *p_red = solid_texture(255, 0, 0);
   GdkTexture *p_grn = solid_texture(0, 255, 0);
   fx_open(&fx);

   info_overlay_show_for_file(fx.p_io, fx.p_file, p_red);
   assert_solid_bins(wait_for_plot(&fx), 255, 0, 0);

   info_overlay_texture_changed(fx.p_io, NULL);
   assert_no_plot(&fx);
   g_assert_true(gtk_widget_get_visible(label_of(&fx)));
   drain_main(200);
   assert_no_plot(&fx);

   /* The same with a binning in flight: green starts, NULL cancels it. */
   info_overlay_texture_changed(fx.p_io, p_grn);
   info_overlay_texture_changed(fx.p_io, NULL);
   drain_main(300);
   assert_no_plot(&fx);
   g_assert_true(gtk_widget_get_visible(label_of(&fx)));

   /* And the card still follows a texture after that. */
   info_overlay_texture_changed(fx.p_io, p_grn);
   assert_solid_bins(wait_for_plot(&fx), 0, 255, 0);

   g_object_unref(p_red);
   g_object_unref(p_grn);
   fx_close(&fx);
}

/* Upper bound for the auto-hide wait below, in wall-clock time. The card is
 * armed with g_timeout_add_seconds(5), and that API only promises a fire
 * somewhere in a 4.75-5.75 s window: GLib rounds second-granularity
 * timeouts to a shared 1 s grid (+-250 ms of slack) so they wake the
 * process together. 7 s clears the late edge with margin; measured by the
 * clock, not by iteration count, because a fixed number of ~1 ms
 * iterations lasts a different real time on every host. */
#define HIDE_WAIT_US (7 * G_USEC_PER_SEC)

/* The auto-hide timer takes the file card down with its plot. The card
 * stays up for 5 s (info-overlay.h), so this subtest waits that long once,
 * deliberately: a test seam for the delay would put test-only state into
 * the module for one assertion, and ~5 s in one integration binary is
 * cheaper than that. Afterwards a texture change finds no card to follow. */
static void
test_timer_hides_card_and_plot(void) {
   OverlayFx   fx;
   GdkTexture *p_red = solid_texture(255, 0, 0);
   fx_open(&fx);

   info_overlay_show_for_file(fx.p_io, fx.p_file, p_red);
   assert_solid_bins(wait_for_plot(&fx), 255, 0, 0);

   const gint64 i_deadline = g_get_monotonic_time() + HIDE_WAIT_US;
   while (gtk_widget_get_visible(label_of(&fx)) &&
          g_get_monotonic_time() < i_deadline) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_false(gtk_widget_get_visible(label_of(&fx)));
   assert_no_plot(&fx);

   info_overlay_texture_changed(fx.p_io, p_red); /* hidden: ignored */
   drain_main(100);
   assert_no_plot(&fx);
   g_assert_false(gtk_widget_get_visible(label_of(&fx)));

   g_object_unref(p_red);
   fx_close(&fx);
}

/* After dispose every entry point is a no-op: no widget is touched (the
 * card is gone with its window) and a request in flight at dispose time is
 * dropped by its completion. The label/plot getters return NULL by then,
 * so the assertions are "no crash, no CRITICAL" (fatal under g_test). */
static void
test_disposed_overlay_is_inert(void) {
   OverlayFx   fx;
   GdkTexture *p_red = solid_texture(255, 0, 0);
   fx_open(&fx);

   info_overlay_show_for_file(fx.p_io, fx.p_file, p_red); /* in flight */
   info_overlay_dispose(fx.p_io);
   drain_main(200); /* the gather lands on a disposed overlay: dropped */

   info_overlay_show_for_file(fx.p_io, fx.p_file, p_red);
   info_overlay_texture_changed(fx.p_io, p_red);
   info_overlay_show_status(fx.p_io, "nothing");
   info_overlay_toggle_for_file(fx.p_io, fx.p_file, p_red);
   info_overlay_dismiss(fx.p_io);
   drain_main(200);
   g_assert_null(info_overlay_get_label(fx.p_io));
   g_assert_null(info_overlay_get_histogram(fx.p_io));

   g_object_unref(p_red);
   fx_close(&fx); /* dispose twice is fine; delete drops the last ref */
}

/* --- histogram view draw path -------------------------------------------- */

/* Run the view's snapshot vfunc into a fresh GtkSnapshot and return the
 * render tree (NULL when nothing was appended). GtkWidgetClass is public
 * API, so this drives exactly what GTK's frame does, without waiting for a
 * frame whose timing the test cannot see. */
static GskRenderNode *
snapshot_view(GtkWidget *p_view) {
   GtkSnapshot *p_snap = gtk_snapshot_new();
   GTK_WIDGET_GET_CLASS(p_view)->snapshot(p_view, p_snap);
   return (gtk_snapshot_free_to_node(p_snap));
}

/* Number of non-empty bins over all four channels: one colour node each. */
static guint
count_bars(const Histogram *p_hist) {
   guint u_bars = 0;
   for (guint u_ch = 0; u_ch < HISTOGRAM_CHANNEL_COUNT; u_ch++) {
      for (guint u = 0; u < HISTOGRAM_BINS; u++) {
         u_bars += p_hist->u_bins[u_ch][u] != 0 ? 1 : 0;
      }
   }
   return (u_bars);
}

/* The view is presented in a real window (Xvfb in the CI lane) so it has an
 * allocation to draw into: measure reports its fixed natural size, a
 * histogram snapshots into floor + one node per non-empty bin, and both the
 * NULL and the all-zero (u_peak == 0) histogram draw nothing. The presented
 * frame clock also runs the same snapshot on GTK's own schedule. */
static void
test_histogram_view_draws(void) {
   GtkWidget *p_win  = gtk_window_new();
   GtkWidget *p_view = ggaze_histogram_view_new();
   gtk_window_set_child(GTK_WINDOW(p_win), p_view);
   gtk_window_present(GTK_WINDOW(p_win));
   for (guint u = 0; u < 3000 && gtk_widget_get_width(p_view) == 0; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_cmpint(gtk_widget_get_width(p_view), >, 0);
   g_assert_cmpint(gtk_widget_get_height(p_view), >, 0);

   int i_min = 0, i_nat = 0;
   gtk_widget_measure(p_view, GTK_ORIENTATION_HORIZONTAL, -1, &i_min, &i_nat,
                      NULL, NULL);
   g_assert_cmpint(i_min, ==, HISTOGRAM_BINS * 3);
   g_assert_cmpint(i_nat, ==, i_min);
   gtk_widget_measure(p_view, GTK_ORIENTATION_VERTICAL, -1, &i_min, &i_nat,
                      NULL, NULL);
   g_assert_cmpint(i_min, ==, 56);

   g_assert_null(snapshot_view(p_view)); /* no histogram: nothing drawn */

   GgazeHistogramView *p_hv = GGAZE_HISTOGRAM_VIEW(p_view);
   ggaze_histogram_view_set_histogram(p_hv, histogram_new()); /* peak 0 */
   g_assert_null(snapshot_view(p_view));

   GdkTexture *p_tex = solid_texture(255, 128, 0);
   ggaze_histogram_view_set_histogram(p_hv, histogram_new_from_texture(p_tex));
   const Histogram *p_hist = ggaze_histogram_view_get_histogram(p_hv);
   g_assert_cmpuint(count_bars(p_hist), ==, 4); /* R, G, B, lum: one each */
   GskRenderNode *p_node = snapshot_view(p_view);
   g_assert_nonnull(p_node);
   g_assert_cmpint(gsk_render_node_get_node_type(p_node), ==,
                   GSK_CONTAINER_NODE);
   g_assert_cmpuint(gsk_container_node_get_n_children(p_node), ==,
                    1 + count_bars(p_hist));
   gsk_render_node_unref(p_node);
   drain_main(100); /* let GTK's own frame draw it once, too */

   ggaze_histogram_view_set_histogram(p_hv, NULL);
   g_assert_null(snapshot_view(p_view));

   g_object_unref(p_tex);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(200);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   /* Host GTK WARNINGs are tolerated; CRITICALs stay fatal (real bugs). */
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
   if (!gtk_init_check()) {
      /* 77 = meson SKIP; see tests/meson.build "Lane determinism". */
      g_print("1..0 # SKIP no display available (run under xvfb)\n");
      return (77);
   }
   g_test_add_func("/info_overlay/show_then_follow_texture",
                   test_show_then_follow_texture);
   g_test_add_func("/info_overlay/status_drops_landing_plot",
                   test_status_drops_landing_plot);
   g_test_add_func("/info_overlay/null_texture_clears_and_cancels",
                   test_null_texture_clears_and_cancels);
   g_test_add_func("/info_overlay/timer_hides_card_and_plot",
                   test_timer_hides_card_and_plot);
   g_test_add_func("/info_overlay/disposed_overlay_is_inert",
                   test_disposed_overlay_is_inert);
   g_test_add_func("/info_overlay/histogram_view_draws",
                   test_histogram_view_draws);
   return (g_test_run());
}
