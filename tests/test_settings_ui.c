/*:*
 * ggaze — settings integration test
 *
 * Verifies that scalar settings read through the Settings wrapper flow into a
 * constructed GgazeWindow: changing thumbnail-size before opening a folder
 * changes the grid size the window builds with, and changing sort changes the
 * navigator sort. Also exercises the AdwPreferencesDialog end to end: build,
 * present on a window, close, and check it is really destroyed.
 * Uses the memory GSettings backend + build-tree schema (set in the meson
 * env), so no dconf is touched. Display-gated; skipped cleanly without a
 * display.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
:*/

#include "gridview.h"
#include "prefs.h"
#include "settings.h"
#include "window.h"

#include <adwaita.h>
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

/* Iterate the main context for roughly u_ms milliseconds, so presenting and
 * closing a dialog (both driven by idles/frame clock) can settle. */
static void
drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

static void
test_window_uses_thumbnail_size(void) {
   /* Set thumbnail-size to a non-default value via a fresh Settings wrapper
    * sharing the same (memory) backend, then build a window and open a
    * folder: the grid is created with that size. */
   Settings *p_cfg = settings_new();
   g_assert_nonnull(p_cfg);
   settings_set_thumbnail_size(p_cfg, 192);
   g_assert_cmpint(settings_get_thumbnail_size(p_cfg), ==, 192);
   settings_delete(p_cfg);

   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar       *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GgazeWindow *p_win  = new_window();
   GFile       *p_file = g_file_new_for_path(c_path);
   ggaze_window_open(p_win, p_file);
   GtkStack  *p_stack = ggaze_window_get_stack(p_win);
   GtkWidget *p_grid  = gtk_stack_get_child_by_name(p_stack, "grid");
   g_assert_nonnull(p_grid);
   g_assert_true(GGAZE_IS_GRID(p_grid));
   g_assert_cmpint(ggaze_grid_get_thumbnail_size(GGAZE_GRID(p_grid)), ==, 192);
   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_free(c_path);
}

static void
test_preferences_action_present(void) {
   GgazeWindow *p_win = new_window();
   GActionMap  *p_map = G_ACTION_MAP(p_win);
   g_assert_nonnull(g_action_map_lookup_action(p_map, "preferences"));
   gtk_window_destroy(GTK_WINDOW(p_win));
}

static void
test_open_applies_sort_setting(void) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);

   /* Configure sort=size via the shared memory backend. */
   Settings *p_cfg = settings_new();
   settings_set_sort(p_cfg, GGAZE_SORT_SIZE);
   settings_delete(p_cfg);

   gchar       *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GgazeWindow *p_win  = new_window();
   GFile       *p_file = g_file_new_for_path(c_path);
   /* Opening must not abort even though settings are non-default; the
    * navigator is built with the configured sort. */
   ggaze_window_open(p_win, p_file);
   const gchar *c_title = gtk_window_get_title(GTK_WINDOW(p_win));
   g_assert_nonnull(c_title);
   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_free(c_path);
}

/* g_object_weak_notify: flips the gboolean the caller passed once the watched
 * object is finalized. */
static void
note_finalized(gpointer p_flag, GObject *p_where) {
   (void)p_where;
   *(gboolean *)p_flag = TRUE;
}

/* Build a real preferences dialog bound to a fresh Settings, present it on a
 * window the way the "preferences" action does, then close it. Any critical
 * from construction (bad binding / missing row API) or from presenting aborts
 * the test (main() makes criticals fatal).
 *
 * Presenting used to be skipped here, blamed on "a GTK-internal stale-toplevel
 * critical unrelated to this code". It was this suite's own bug: the earlier
 * subtests released their windows with g_object_unref(), which finalizes the
 * window but leaves it in GTK's toplevel list, and presenting walks that list
 * -- every walk transiently refs each entry via g_list_model_get_item() -- so
 * it tripped over a freed window. With the teardown corrected (1w0)
 * presenting is clean, so the coverage is back. Full rationale in
 * tests/helpers/gtk_helpers.h, "window teardown". */
static void
test_prefs_dialog_constructs(void) {
   Settings             *p_cfg = settings_new();
   AdwPreferencesDialog *p_dlg = prefs_build_dialog(p_cfg);
   g_assert_nonnull(p_dlg);
   g_assert_true(ADW_IS_PREFERENCES_DIALOG(p_dlg));

   /* adw_dialog_present() sinks the floating reference the builder returned
    * and makes the presenting widget the dialog's owner. */
   gboolean     b_finalized = FALSE;
   GgazeWindow *p_win       = new_window();
   g_object_weak_ref(G_OBJECT(p_dlg), note_finalized, &b_finalized);
   adw_dialog_present(ADW_DIALOG(p_dlg), GTK_WIDGET(p_win));
   drain_main(200);
   /* Presenting really happened. NOT :visible -- an AdwDialog is already
    * visible=TRUE at construction (measured), so asserting that can never
    * fail and would still pass if present() silently did nothing. These two
    * are both false beforehand (measured: mapped=0, root=NULL) and only
    * present() flips them: it puts the dialog into a host -- here the
    * fallback GtkWindow libadwaita creates because p_win is not itself an
    * AdwDialog host -- and maps it. */
   g_assert_nonnull(gtk_widget_get_root(GTK_WIDGET(p_dlg)));
   g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(p_dlg)));

   /* Closing must actually destroy the dialog -- that is what releases the
    * g_settings bindings and the row closures (each freed via its destroy
    * notify), so the weak notify above is the check that it happened. */
   adw_dialog_close(ADW_DIALOG(p_dlg));
   drain_main(200);
   g_assert_true(b_finalized);

   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(100);
   settings_delete(p_cfg);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
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

   g_test_add_func("/settings/window_thumbnail_size",
                   test_window_uses_thumbnail_size);
   g_test_add_func("/settings/preferences_action_present",
                   test_preferences_action_present);
   g_test_add_func("/settings/open_applies_sort",
                   test_open_applies_sort_setting);
   g_test_add_func("/settings/prefs_dialog_constructs",
                   test_prefs_dialog_constructs);
   return (g_test_run());
}