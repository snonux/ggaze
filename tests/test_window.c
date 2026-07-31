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
#include "viewer.h"

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

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

static void
test_stack_has_two_views(void) {
   GgazeWindow *p_win = new_window();
   g_assert_nonnull(p_win);

   GtkWidget *p_child = GTK_WIDGET(ggaze_window_get_stack(p_win));
   g_assert_nonnull(p_child);
   g_assert_true(GTK_IS_STACK(p_child));

   GtkStack   *p_stack = GTK_STACK(p_child);
   GListModel *p_pages = G_LIST_MODEL(gtk_stack_get_pages(p_stack));
   g_assert_cmpint(g_list_model_get_n_items(p_pages), ==, 2);
   g_assert_nonnull(gtk_stack_get_child_by_name(p_stack, "grid"));
   g_assert_nonnull(gtk_stack_get_child_by_name(p_stack, "large"));
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "grid");
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
   g_assert_true(gtk_widget_get_visible(p_lbl));
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
   g_assert_true(gtk_widget_get_visible(p_lbl));

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
    * was toggled, so still not dirty. */
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

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_free(c_path);
   drain_main(200);
}

int
main(int i_argc, char **c_argv) {
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
   g_test_add_func("/window/enhance_a_is_safe_with_and_without_gegl",
                   test_enhance_a_is_safe_with_and_without_gegl);
   return (g_test_run());
}