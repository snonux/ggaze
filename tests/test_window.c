/*:*
 * ggaze — window smoke test (integration)
 *
 * Constructs a GgazeWindow on a real display -- never presented, but not
 * offscreen: there is no offscreen/headless GTK4 backend, so this suite needs
 * a display and skips cleanly without one (see main()). Asserts the two-view
 * stack exists (children "grid" and "large", default visible = "grid"), and
 * that ggaze_window_open titles the window with the file's basename.
 *
 * Also covers gu0: the info overlay (`i`) must never keep showing a
 * PREVIOUS file's EXIF/dimensions after navigation displays a new one. See
 * test_info_hides_on_navigation() below, plus test_info_hides_on_reopen()
 * for the ggaze_window_open() gap (File->Open / drag-and-drop / single-
 * instance re-activation into a different folder) that the "changed" signal
 * alone does not cover.
 *
 * And 0c2: the card carries an RGB/luminance histogram of the displayed
 * texture, gathered in the same async request as the EXIF text. See
 * test_info_shows_histogram() -- a plot for the image on screen, none from
 * the grid, a different plot after navigating to another image, and no plot
 * left behind by `i` toggling the card off; test_info_no_plot_while_loading()
 * -- `i` during a texturecache-miss decode (the previous picture still on
 * screen) must not pair the new file's text with the old file's plot, and
 * the plot follows the decode once it lands under the card;
 * test_status_clears_plot() -- a status line taking over the card carries no
 * plot under it; and test_toggle_view_follows_card() -- `t` to the grid
 * takes the plot off a card that stays up, `t` back fills it in. (The plot
 * following hold-Space and a landing preset is the GEGL lane's
 * /enhance_flow/info_plots_preview; the overlay's own edge cases -- a
 * dropped late plot, texture_changed(NULL), the auto-hide timer, dispose --
 * and the plot widget's draw path are tests/test_info_overlay.c.)
 *
 * The window is built with g_object_new() (no "application" property): the
 * stack/header are constructed in ggaze_window_init, independent of the app
 * association. Setting GtkWindow:application requires the GApplication
 * ::startup signal to have fired (it does in production via activate/open;
 * not in this isolated smoke test). Needing a display is why this lives in
 * the `integration` suite, which CI runs under `xvfb-run` -- i.e. on the X11
 * backend (.woodpecker/ci.yml).
 *
 * That backend is also why this suite is is_parallel : false in
 * tests/meson.build (5w0): the `a` subtest opens the GEGL preset popover, and
 * an autohide popover takes the display-global X11 seat grab. See
 * tests/meson.build, "suites that need exclusive use of the display's seat
 * grab".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "window.h"

#include "ggaze-config.h"
#include "histogram-view.h"
#include "histogram.h"
#include "loader/animation.h"
#include "viewer.h"

#if GGAZE_HAVE_GEGL
#include <gegl.h>
#endif

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <string.h>

/* Windows built here are torn down with gtk_window_destroy(), never a plain
 * g_object_unref(): GTK4 hands the caller's reference to its internal
 * toplevel list and only destroy() takes the entry back out (it drops that
 * reference too, so the window still finalizes). Full rationale in
 * tests/helpers/gtk_helpers.h, "window teardown". */
static GgazeWindow *
new_window(void) {
   return (GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL)));
}

/* --- gu0 helpers: navigate-away-hides-info regression -------------------- */

static void
copy_fixture(const char *c_dir, const char *c_name) {
   const gchar *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char   *c_src = g_build_filename(c_fx, c_name, NULL);
   char   *c_dst = g_build_filename(c_dir, c_name, NULL);
   GFile  *p_src = g_file_new_for_path(c_src);
   GFile  *p_dst = g_file_new_for_path(c_dst);
   GError *p_err = NULL;
   g_assert_true(g_file_copy(p_src, p_dst, G_FILE_COPY_OVERWRITE, NULL, NULL,
                             NULL, &p_err));
   g_assert_no_error(p_err);
   g_object_unref(p_src);
   g_object_unref(p_dst);
   g_free(c_src);
   g_free(c_dst);
}

/* Flat (no subdirs) temp-dir cleanup -- the fixtures this test copies never
 * nest, unlike test_delete_nav.c's version of this helper. */
static void
cleanup_temp_dir(char *c_dir) {
   GFile           *p_dir = g_file_new_for_path(c_dir);
   GFileEnumerator *p_e   = g_file_enumerate_children(
      p_dir, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (p_e != NULL) {
      GFileInfo *p_info;
      while ((p_info = g_file_enumerator_next_file(p_e, NULL, NULL)) != NULL) {
         GFile *p_child = g_file_get_child(p_dir, g_file_info_get_name(p_info));
         g_file_delete(p_child, NULL, NULL);
         g_object_unref(p_child);
         g_object_unref(p_info);
      }
      g_object_unref(p_e);
   }
   g_file_delete(p_dir, NULL, NULL);
   g_object_unref(p_dir);
   g_free(c_dir);
}

static void
drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

static GdkTexture *
viewer_texture(GgazeWindow *p_win) {
   GtkStack  *p_stack = ggaze_window_get_stack(p_win);
   GtkWidget *p_large = gtk_stack_get_child_by_name(p_stack, "large");
   return (ggaze_viewer_get_texture(GGAZE_VIEWER(p_large)));
}

static void
wait_for_load(GgazeWindow *p_win) {
   for (guint u = 0; u < 3000 && viewer_texture(p_win) == NULL; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(viewer_texture(p_win));
   drain_main(200);
}

static void
fire(GgazeWindow *p_win, const char *c_action) {
   gtk_widget_activate_action(GTK_WIDGET(p_win), c_action, NULL);
}

/* _show_info gathers the info off the main thread (a GTask) and applies it
 * when it lands, so the info label is not visible the instant win.info is
 * fired. Drain the main loop until the overlay label appears (the decode is
 * fast for the tiny fixtures, but the round-trip still needs the loop to run).
 */
static void
wait_for_info(GgazeWindow *p_win) {
   GtkWidget *p_lbl = ggaze_window_get_info_label(p_win);
   for (guint u = 0; u < 3000 && !gtk_widget_get_visible(p_lbl); u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_true(gtk_widget_get_visible(p_lbl));
   drain_main(50);
}

static void
test_stack_has_two_views(void) {
   GgazeWindow *p_win = new_window();
   g_assert_nonnull(p_win);

   GtkWidget *p_child = GTK_WIDGET(ggaze_window_get_stack(p_win));
   g_assert_nonnull(p_child);
   g_assert_true(GTK_IS_STACK(p_child));

   GtkStack   *p_stack = GTK_STACK(p_child);
   GListModel *p_pages = G_LIST_MODEL(gtk_stack_get_pages(p_stack));
   /* grid, large, and the empty-state page shown until something is open. */
   g_assert_cmpint(g_list_model_get_n_items(p_pages), ==, 3);
   g_assert_nonnull(gtk_stack_get_child_by_name(p_stack, "grid"));
   g_assert_nonnull(gtk_stack_get_child_by_name(p_stack, "large"));
   g_assert_nonnull(gtk_stack_get_child_by_name(p_stack, "empty"));
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "empty");
   g_object_unref(p_pages);

   gtk_window_destroy(GTK_WINDOW(p_win));
}

static void
test_open_titles_window(void) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar       *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GgazeWindow *p_win  = new_window();
   GFile       *p_file = g_file_new_for_path(c_path);
   ggaze_window_open(p_win, p_file);
   /* M2: opening a file lists its parent folder; the header title carries
    * the current filename + "n/total". */
   const gchar *c_title = gtk_window_get_title(GTK_WINDOW(p_win));
   g_assert_nonnull(c_title);
   g_assert_nonnull(g_strstr_len(c_title, -1, "plain.jpg"));
   g_assert_nonnull(g_strstr_len(c_title, -1, "/"));
   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_free(c_path);
}

/* gu0 regression: _show_info's overlay used to stay visible with the OLD
 * current file's EXIF/dimensions until its 5s auto-hide timer expired, even
 * after navigation had already moved the displayed image on to a different
 * file. Shows info for A (plain.jpg, 6x3), navigates to B (rot6.jpg, 8x4)
 * via win.next well before the timer would fire, and asserts the overlay is
 * hidden immediately rather than lingering with A's stale "6x3" text. win.next
 * -> Navigator's "changed" -> nav_changed_cb, the single choke point every
 * navigation path (arrows, slideshow, grid, trash/move/delete advance, ...)
 * funnels through, so this one path is representative of all of them. */
static void
test_info_hides_on_navigation(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-info-nav-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg"); /* A: 6x3, sorts first */
   copy_fixture(c_dir, "rot6.jpg");  /* B: 8x4, sorts second */

   char        *c_p0  = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile       *p_f0  = g_file_new_for_path(c_p0);
   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_f0);
   wait_for_load(p_win);

   fire(p_win, "win.info");
   GtkWidget *p_lbl = ggaze_window_get_info_label(p_win);
   wait_for_info(p_win);
   g_assert_nonnull(
      g_strstr_len(gtk_label_get_text(GTK_LABEL(p_lbl)), -1, "6×3"));

   fire(p_win, "win.next"); /* -> B, long before the 5s timer would fire */
   g_assert_false(gtk_widget_get_visible(p_lbl));
   wait_for_load(p_win);
   g_assert_false(gtk_widget_get_visible(p_lbl)); /* still hidden after load */

   g_object_unref(p_f0);
   g_free(c_p0);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* gu0 fresh-context review gap: ggaze_window_open() (File->Open dialog,
 * drag-and-drop, single-instance re-activation onto an existing window)
 * builds a brand-new Navigator and can change the displayed file WITHOUT
 * Navigator's "changed" ever firing -- the signal nav_changed_cb (and hence
 * the fix above) relies on. Here the second folder holds a single file, so
 * navigator_new() already defaults i_current to 0 before
 * navigator_set_current_file() resolves that same file to index 0 too;
 * since i_current already equals the resolved index, navigator_set_current_
 * file() returns early without emitting "changed" (see its early-return
 * guard in navigator.c). This reproduces the reported case (b): a file that
 * sorts first in its folder. ggaze_window_open() must dismiss the overlay
 * unconditionally instead of relying on that signal. */
static void
test_info_hides_on_reopen(void) {
   GError *p_err  = NULL;
   char   *c_dir1 = g_dir_make_tmp("ggaze-info-open1-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir1, "plain.jpg"); /* A: 6x3 */

   char *c_dir2 = g_dir_make_tmp("ggaze-info-open2-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir2, "rot6.jpg"); /* B: 8x4, sole file -> index 0 */

   char        *c_p0  = g_build_filename(c_dir1, "plain.jpg", NULL);
   GFile       *p_f0  = g_file_new_for_path(c_p0);
   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_f0);
   wait_for_load(p_win);

   fire(p_win, "win.info");
   GtkWidget *p_lbl = ggaze_window_get_info_label(p_win);
   wait_for_info(p_win);

   char  *c_p1 = g_build_filename(c_dir2, "rot6.jpg", NULL);
   GFile *p_f1 = g_file_new_for_path(c_p1);
   /* Simulates File->Open / drag-and-drop targeting a different folder on an
    * already-open window. */
   ggaze_window_open(p_win, p_f1);
   g_assert_false(gtk_widget_get_visible(p_lbl));
   wait_for_load(p_win);
   g_assert_false(gtk_widget_get_visible(p_lbl)); /* still hidden after load */

   g_object_unref(p_f1);
   g_free(c_p1);
   g_object_unref(p_f0);
   g_free(c_p0);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir1);
   cleanup_temp_dir(c_dir2);
}

/* Drain until the plot is up holding a histogram built from exactly
 * u_samples pixels (the fixtures are small enough that every pixel is read,
 * so the sample count IS the dimensions and pins the plot to the image on
 * screen), and return it. The plot lands asynchronously: with the card's
 * text for a fresh `i`, on its own for a texture change under the card. */
static const Histogram *
wait_for_plot(GgazeWindow *p_win, guint64 u_samples) {
   GtkWidget       *p_plot = ggaze_window_get_info_histogram(p_win);
   const Histogram *p_hist = NULL;
   for (guint u = 0; u < 3000; u++) {
      p_hist = ggaze_histogram_view_get_histogram(GGAZE_HISTOGRAM_VIEW(p_plot));
      if (p_hist != NULL && p_hist->u_samples == u_samples) {
         break;
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(p_hist);
   g_assert_cmpuint(p_hist->u_samples, ==, u_samples);
   g_assert_cmpuint(p_hist->u_peak, >, 0);
   g_assert_true(gtk_widget_get_visible(p_plot));
   return (p_hist);
}

/* Drain until the plot holds a histogram of u_samples pixels, asserting at
 * every iteration on the way that it holds nothing else: no histogram at
 * all (plot down) or the wanted one -- never another image's. Fails after
 * 3 s. */
static void
wait_for_plot_never_other(GgazeWindow *p_win, guint64 u_samples) {
   GtkWidget *p_plot = ggaze_window_get_info_histogram(p_win);
   for (guint u = 0; u < 3000; u++) {
      const Histogram *p_hist =
         ggaze_histogram_view_get_histogram(GGAZE_HISTOGRAM_VIEW(p_plot));
      if (p_hist != NULL) {
         g_assert_cmpuint(p_hist->u_samples, ==, u_samples);
         g_assert_true(gtk_widget_get_visible(p_plot));
         return;
      }
      g_assert_false(gtk_widget_get_visible(p_plot));
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_not_reached(); /* no plot within 3 s */
}

/* Fire `i`, wait for the card and return its plot's histogram (see
 * wait_for_plot for what u_samples pins down). */
static const Histogram *
show_info_expect_plot(GgazeWindow *p_win, guint64 u_samples) {
   fire(p_win, "win.info");
   wait_for_info(p_win);
   return (wait_for_plot(p_win, u_samples));
}

/* From the grid there is no displayed texture to judge, so `i` brings the
 * card up (dimensions, EXIF) without a plot rather than with the viewer's
 * leftovers. Expects the card to be down on entry. */
static void
assert_grid_card_has_no_plot(GgazeWindow *p_win) {
   GtkWidget *p_plot = ggaze_window_get_info_histogram(p_win);
   fire(p_win, "win.toggle-view");
   g_assert_cmpstr(
      gtk_stack_get_visible_child_name(ggaze_window_get_stack(p_win)), ==,
      "grid");
   fire(p_win, "win.info");
   wait_for_info(p_win);
   g_assert_false(gtk_widget_get_visible(p_plot));
   g_assert_null(
      ggaze_histogram_view_get_histogram(GGAZE_HISTOGRAM_VIEW(p_plot)));
}

/* The plot is down and holds no histogram (the card may still be up). */
static void
assert_no_plot(GgazeWindow *p_win) {
   GtkWidget *p_plot = ggaze_window_get_info_histogram(p_win);
   g_assert_false(gtk_widget_get_visible(p_plot));
   g_assert_null(
      ggaze_histogram_view_get_histogram(GGAZE_HISTOGRAM_VIEW(p_plot)));
}

/* Open a fresh two-image folder (A plain.jpg 6x3, B rot6.jpg 4x8 upright)
 * on a new window showing A. *pc_dir receives the temp dir to clean up. */
static GgazeWindow *
open_two_image_folder(char **pc_dir) {
   GError *p_err = NULL;
   *pc_dir       = g_dir_make_tmp("ggaze-info-hist-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(*pc_dir, "plain.jpg"); /* A: 6x3, sorts first */
   copy_fixture(*pc_dir, "rot6.jpg");  /* B: 4x8 upright, sorts second */
   char        *c_p0  = g_build_filename(*pc_dir, "plain.jpg", NULL);
   GFile       *p_f0  = g_file_new_for_path(c_p0);
   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_f0);
   wait_for_load(p_win);
   g_object_unref(p_f0);
   g_free(c_p0);
   return (p_win);
}

static void
close_window_and_folder(GgazeWindow *p_win, char *c_dir) {
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* 0c2: `i` in the large view shows a histogram of the displayed texture,
 * built from the texture's actual pixels (plain.jpg is 6x3 -> 18 samples,
 * rot6.jpg lands upright as 4x8 -> 32). Navigating away hides it with the
 * card (gu0), and `i` on the new image yields the new image's plot, not a
 * stale one; from the grid the card carries no plot at all. */
static void
test_info_shows_histogram(void) {
   char        *c_dir = NULL;
   GgazeWindow *p_win = open_two_image_folder(&c_dir);
   GtkWidget   *p_lbl = ggaze_window_get_info_label(p_win);
   g_assert_true(
      GGAZE_IS_HISTOGRAM_VIEW(ggaze_window_get_info_histogram(p_win)));
   assert_no_plot(p_win);

   Histogram hist_a = *show_info_expect_plot(p_win, 6 * 3); /* A's bins */

   fire(p_win, "win.next"); /* -> B: the card and its plot go with A */
   g_assert_false(gtk_widget_get_visible(p_lbl));
   assert_no_plot(p_win);
   wait_for_load(p_win);

   const Histogram *p_b = show_info_expect_plot(p_win, 4 * 8);
   g_assert_cmpint(memcmp(hist_a.u_bins, p_b->u_bins, sizeof(hist_a.u_bins)),
                   !=, 0);

   /* `i` again toggles B's card off, and the plot goes with it: the widget
    * holds no histogram, not merely a hidden one. */
   fire(p_win, "win.info");
   g_assert_false(gtk_widget_get_visible(p_lbl));
   assert_no_plot(p_win);
   assert_grid_card_has_no_plot(p_win);

   close_window_and_folder(p_win, c_dir);
}

/* yb2: on an animated GIF the `i` card plots the FIRST frame -- the one
 * texture the window, the cache and the viewer agree on -- and keeps that
 * plot while the frames play, because the frames never become "the
 * texture". anim.gif's first frame is solid (0, 255, 0): all 48 samples
 * in green's top bin and red's bottom one, before and after playback has
 * moved on to another frame (which would bin very differently). */
static void
test_info_plots_animation_first_frame(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-info-anim-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "anim.gif");
   char        *c_path = g_build_filename(c_dir, "anim.gif", NULL);
   GFile       *p_file = g_file_new_for_path(c_path);
   GgazeWindow *p_win  = new_window();
   gtk_window_present(GTK_WINDOW(p_win)); /* mapped: the frames play */
   ggaze_window_open(p_win, p_file);
   wait_for_load(p_win);
   g_object_unref(p_file);
   g_free(c_path);

   GdkTexture *p_tex = viewer_texture(p_win);
   g_assert_nonnull(animation_lookup(p_tex));
   const Histogram *p_h = show_info_expect_plot(p_win, 8 * 6);
   g_assert_cmpuint(p_h->u_bins[HISTOGRAM_CHANNEL_G][HISTOGRAM_BINS - 1], ==,
                    48);
   g_assert_cmpuint(p_h->u_bins[HISTOGRAM_CHANNEL_R][0], ==, 48);

   GtkStack    *p_stack = ggaze_window_get_stack(p_win);
   GgazeViewer *p_v =
      GGAZE_VIEWER(gtk_stack_get_child_by_name(p_stack, "large"));
   for (guint u = 0; u < 3000 && ggaze_viewer_get_frame(p_v) == p_tex; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_true(ggaze_viewer_get_frame(p_v) != p_tex); /* playing */
   g_assert_true(viewer_texture(p_win) == p_tex);
   p_h = wait_for_plot(p_win, 8 * 6);
   g_assert_cmpuint(p_h->u_bins[HISTOGRAM_CHANNEL_G][HISTOGRAM_BINS - 1], ==,
                    48);

   close_window_and_folder(p_win, c_dir);
}

/* Review finding on 0c2: on a texturecache miss viewload keeps the PREVIOUS
 * picture on screen until the new decode lands, so `i` fired in that window
 * used to pair B's EXIF text with A's histogram -- and kept it after B
 * landed. Now the card comes up without a plot while B is decoding, and
 * once B lands the plot follows the picture (second review: the card's plot
 * tracks the displayed texture for as long as the card is up) -- B's own
 * bins, without a second `i`. Toggling the card off and on plots B too. */
static void
test_info_no_plot_while_loading(void) {
   char        *c_dir = NULL;
   GgazeWindow *p_win = open_two_image_folder(&c_dir);
   GtkWidget   *p_lbl = ggaze_window_get_info_label(p_win);
   GdkTexture  *p_a   = g_object_ref(viewer_texture(p_win)); /* see wait */

   ggaze_window_clear_texture_cache(p_win); /* B must decode async */
   fire(p_win, "win.next");
   g_assert_true(viewer_texture(p_win) == p_a); /* A still up: the race */
   fire(p_win, "win.info");
   /* The card's text and B's decode both land within milliseconds and in
    * either order, so the assertion is the invariant, not an instant: from
    * here on the plot is NULL for as long as A is the picture, and B's
    * (32 samples) once B landed -- A's (18) at no point in between. */
   wait_for_plot_never_other(p_win, 4 * 8);
   g_assert_true(viewer_texture(p_win) != p_a); /* B did land, upright */
   g_assert_cmpint(gdk_texture_get_width(viewer_texture(p_win)), ==, 4);
   /* B's text (rot6.jpg stores 8x4; the card prints stored dimensions). */
   g_assert_true(gtk_widget_get_visible(p_lbl));
   g_assert_nonnull(
      g_strstr_len(gtk_label_get_text(GTK_LABEL(p_lbl)), -1, "8×4"));

   fire(p_win, "win.info"); /* off ... */
   g_assert_false(gtk_widget_get_visible(p_lbl));
   show_info_expect_plot(p_win, 4 * 8); /* ... and on: B's own plot */

   g_object_unref(p_a);
   close_window_and_folder(p_win, c_dir);
}

/* A status line reuses the card while it is up (here "Trash is already
 * empty" from `E` on a folder without a .Trash): the text is no longer about
 * the file, so the plot must go with it rather than sit under the status. */
static void
test_status_clears_plot(void) {
   char        *c_dir = NULL;
   GgazeWindow *p_win = open_two_image_folder(&c_dir);
   GtkWidget   *p_lbl = ggaze_window_get_info_label(p_win);

   show_info_expect_plot(p_win, 6 * 3);
   fire(p_win, "win.empty-trash");
   g_assert_true(gtk_widget_get_visible(p_lbl));
   g_assert_nonnull(g_strstr_len(gtk_label_get_text(GTK_LABEL(p_lbl)), -1,
                                 "Trash is already empty"));
   assert_no_plot(p_win);

   close_window_and_folder(p_win, c_dir);
}

/* Third review on 0c2: `t` with a file card up used to leave the large
 * view's plot on the card over the grid for the rest of its 5 s, though the
 * grid card has no plot (docs/ui-and-interactions.md). The view switch now
 * syncs the plot like a texture change does: to the grid it goes down at
 * once, the text stays; back to the large view it fills in again from the
 * picture that is still on screen, without a second `i`. */
static void
test_toggle_view_follows_card(void) {
   char        *c_dir = NULL;
   GgazeWindow *p_win = open_two_image_folder(&c_dir);
   GtkWidget   *p_lbl = ggaze_window_get_info_label(p_win);

   show_info_expect_plot(p_win, 6 * 3);
   fire(p_win, "win.toggle-view"); /* -> grid, card still up */
   g_assert_cmpstr(
      gtk_stack_get_visible_child_name(ggaze_window_get_stack(p_win)), ==,
      "grid");
   g_assert_true(gtk_widget_get_visible(p_lbl));
   assert_no_plot(p_win);
   drain_main(100); /* nothing lands later either */
   assert_no_plot(p_win);

   fire(p_win, "win.toggle-view"); /* -> large: the plot comes back */
   g_assert_cmpstr(
      gtk_stack_get_visible_child_name(ggaze_window_get_stack(p_win)), ==,
      "large");
   g_assert_true(gtk_widget_get_visible(p_lbl));
   wait_for_plot(p_win, 6 * 3);

   close_window_and_folder(p_win, c_dir);
}

/* tu0 requirement 8: every new enhance UI entry point must stay safe when
 * GEGL is not built in, and say so clearly rather than silently no-op'ing or
 * crashing. Built (and run) in BOTH lanes (unlike tests/test_enhance_flow.c,
 * which is gegl-only) so the disabled behavior is actually exercised by the
 * minimal-lane CI run, not just asserted by inspection. */
static void
test_enhance_a_is_safe_with_and_without_gegl(void) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar       *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile       *p_file = g_file_new_for_path(c_path);
   GgazeWindow *p_win  = new_window();
   ggaze_window_open(p_win, p_file);
   wait_for_load(p_win);

   g_assert_false(ggaze_window_enhance_is_dirty(p_win));
   fire(p_win, "win.enhance");
#if GGAZE_HAVE_GEGL
   /* GEGL built in: `a` opens the preset popover -- no crash, and nothing
    * was toggled, so still not dirty.
    *
    * This popover is also the ONLY reason the whole suite runs
    * is_parallel : false. In the minimal lanes the #else branch runs
    * instead, no popover opens, and nothing here contends for the X seat
    * grab -- so the serialisation buys those two lanes nothing and costs
    * them 2.3 s. Kept anyway; tests/meson.build (at the `window` test) has
    * the reasoning, so it does not need deriving again. */
   g_assert_false(ggaze_window_enhance_is_dirty(p_win));
#else
   /* GEGL not built in: `a` must be a safe no-op that clearly reports its
    * unavailability (docs/gegl.md, AGENTS.md "Optional features") rather
    * than silently doing nothing or crashing. */
   GtkWidget *p_lbl = ggaze_window_get_info_label(p_win);
   g_assert_true(gtk_widget_get_visible(p_lbl));
   g_assert_nonnull(g_strstr_len(gtk_label_get_text(GTK_LABEL(p_lbl)), -1,
                                 "GEGL not built in"));
   g_assert_false(ggaze_window_enhance_is_dirty(p_win));
#endif
   /* Hold-Space and win.enhance-save must also be safe no-ops either way. */
   ggaze_window_set_hold_original(p_win, TRUE);
   ggaze_window_set_hold_original(p_win, FALSE);
   fire(p_win, "win.enhance-save");
   /* wb2: the crop / straighten / rotate tools. Both lanes: the hooks are
    * defined either way and a full turn (] then [) is the identity, so the
    * window ends clean. */
   fire(p_win, "win.rotate-cw");
   fire(p_win, "win.rotate-ccw");
   g_assert_false(ggaze_window_enhance_is_dirty(p_win));
   g_assert_false(ggaze_window_tool_key(p_win, GDK_KEY_h, 0)); /* no tool */
   ggaze_window_tool_drag(p_win, GGAZE_VIEWER_DRAG_BEGIN, 1.0, 1.0);
   ggaze_window_tool_drag(p_win, GGAZE_VIEWER_DRAG_END, 2.0, 2.0);
   fire(p_win, "win.crop");
#if GGAZE_HAVE_GEGL
   g_assert_cmpint(ggaze_window_get_tool(p_win), ==, GGAZE_TOOL_CROP);
   fire(p_win, "win.back"); /* Esc: cancels the tool, nothing else */
   g_assert_cmpint(ggaze_window_get_tool(p_win), ==, GGAZE_TOOL_NONE);
   fire(p_win, "win.straighten");
   g_assert_cmpint(ggaze_window_get_tool(p_win), ==, GGAZE_TOOL_STRAIGHTEN);
   g_assert_true(ggaze_window_tool_key(p_win, GDK_KEY_Escape, 0));
   g_assert_cmpint(ggaze_window_get_tool(p_win), ==, GGAZE_TOOL_NONE);
#else
   /* Without GEGL every tool key reports itself unavailable, like `a`,
    * and no tool can ever be active. */
   g_assert_cmpint(ggaze_window_get_tool(p_win), ==, GGAZE_TOOL_NONE);
   g_assert_nonnull(g_strstr_len(gtk_label_get_text(GTK_LABEL(p_lbl)), -1,
                                 "GEGL not built in"));
   fire(p_win, "win.straighten");
   g_assert_cmpint(ggaze_window_get_tool(p_win), ==, GGAZE_TOOL_NONE);
   g_assert_false(ggaze_window_tool_key(p_win, GDK_KEY_Return, 0));
#endif
   g_assert_false(ggaze_window_enhance_is_dirty(p_win));

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_free(c_path);
   drain_main(200);
}

int
main(int i_argc, char **c_argv) {
#if GGAZE_HAVE_GEGL
   /* Production always calls gegl_init() at GApplication startup (app.c) well
    * before any window/enhance action exists; this bare test window (built via
    * g_object_new, bypassing GgazeApp) needs the same explicit call
    * test_enhancer.c and test_enhance_flow.c already make.
    *
    * Newly required here: the `a` subtest used to only open the preset
    * popover, which touched no GEGL. The popover now populates per-preset
    * preview thumbnails, and that work runs enhancer_load() on a GTask thread
    * pool -- where an uninitialised operation registry makes GEGL's own
    * gegl_operations_update_visible() abort on a NULL hash table, off the main
    * thread and with no hint that the registry was the problem. */
   gegl_init(&i_argc, &c_argv);
#endif
   g_test_init(&i_argc, &c_argv, NULL);

   /* Tolerate host GTK WARNINGs (e.g. an unknown gtk-modules key delivered
    * by the live GNOME session via XSettings) that g_test_init makes fatal.
    * Keep CRITICALs fatal — those are real bugs. */
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);

   if (!gtk_init_check()) {
      /* Exit 77 is meson's "skipped". Returning g_test_run() here
       * instead exits 0 after a "1..0" plan -- a lane that reports OK
       * while running nothing, which is how displayless runs used to
       * hide the GDK backend leaks. See tests/meson.build "Lane
       * determinism" (1w0). */
      g_print("1..0 # SKIP no display available (run under xvfb)\n");
      return (77);
   }

   g_test_add_func("/window/stack_has_two_views", test_stack_has_two_views);
   g_test_add_func("/window/open_titles_window", test_open_titles_window);
   g_test_add_func("/window/info_hides_on_navigation",
                   test_info_hides_on_navigation);
   g_test_add_func("/window/info_hides_on_reopen", test_info_hides_on_reopen);
   g_test_add_func("/window/info_shows_histogram", test_info_shows_histogram);
   g_test_add_func("/window/info_plots_animation_first_frame",
                   test_info_plots_animation_first_frame);
   g_test_add_func("/window/info_no_plot_while_loading",
                   test_info_no_plot_while_loading);
   g_test_add_func("/window/status_clears_plot", test_status_clears_plot);
   g_test_add_func("/window/toggle_view_follows_card",
                   test_toggle_view_follows_card);
   g_test_add_func("/window/enhance_a_is_safe_with_and_without_gegl",
                   test_enhance_a_is_safe_with_and_without_gegl);
   return (g_test_run());
}