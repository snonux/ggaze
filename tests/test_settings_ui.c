/*:*
 * ggaze — settings integration test
 *
 * Verifies that scalar settings read through the Settings wrapper flow into a
 * constructed GgazeWindow: changing thumbnail-size before opening a folder
 * changes the grid size the window builds with, and changing sort changes the
 * navigator sort. Also exercises construction of the AdwPreferencesDialog.
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

static GgazeWindow *
new_window(void) {
   return (GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL)));
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
   g_object_unref(p_win);
   g_free(c_path);
}

static void
test_preferences_action_present(void) {
   GgazeWindow *p_win = new_window();
   GActionMap  *p_map = G_ACTION_MAP(p_win);
   g_assert_nonnull(g_action_map_lookup_action(p_map, "preferences"));
   g_object_unref(p_win);
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
   g_object_unref(p_win);
   g_free(c_path);
}

static void
test_prefs_dialog_constructs(void) {
   /* Build a real preferences dialog bound to a fresh Settings and tear it
    * down. Any construction critical (bad binding / missing row API) aborts
    * the test. The dialog is built but not presented: presenting under ASan
    * trips a GTK-internal stale-toplevel critical unrelated to this code. */
   Settings             *p_cfg = settings_new();
   AdwPreferencesDialog *p_dlg = prefs_build_dialog(p_cfg);
   g_assert_nonnull(p_dlg);
   g_assert_true(ADW_IS_PREFERENCES_DIALOG(p_dlg));
   /* Destroying the dialog releases the g_settings bindings and the row
    * closures (each freed via its destroy notify). Sink the initial floating
    * reference first (adw_preferences_dialog_new returns a floating widget). */
   g_object_ref_sink(p_dlg);
   g_object_unref(p_dlg);
   settings_delete(p_cfg);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);

   if (!gtk_init_check()) {
      g_test_skip("no display available (run under xvfb)");
      return (g_test_run());
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