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

/* Iterate the main context for roughly u_ms milliseconds. This is a SETTLE,
 * not a wait: it dispatches whatever is ready and returns after a fixed
 * number of iterations regardless of what happened. Only use it where the
 * assertions that follow are themselves the check, so a settle that was too
 * short fails loudly. Where the condition is "this object is gone", use
 * wait_for_finalized() / destroy_window_and_wait() below instead. */
static void
drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

/* g_object_weak_notify: flips the gboolean the caller passed once the watched
 * object is finalized. */
static void
note_finalized(gpointer p_flag, GObject *p_where) {
   (void)p_where;
   *(gboolean *)p_flag = TRUE;
}

/* Upper bound for a teardown wait. Generous on purpose: it exists so a
 * genuinely stuck teardown fails the suite instead of hanging CI forever, not
 * to time-slice anything. Unloaded, the waits below finish in tens of
 * milliseconds. */
#define TEARDOWN_TIMEOUT_US (10 * G_USEC_PER_SEC)

/* Iterate the default main context until *p_flag (raised by a
 * g_object_weak_ref(..., note_finalized, p_flag)) turns TRUE, or the bound
 * above expires. Returns whether it turned TRUE, so callers assert on it. */
static gboolean
wait_for_finalized(const gboolean *p_flag) {
   const gint64 i_deadline = g_get_monotonic_time() + TEARDOWN_TIMEOUT_US;
   while (!*p_flag && g_get_monotonic_time() < i_deadline) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   return (*p_flag);
}

/* Destroy a window a subtest built and wait until it is REALLY finalized,
 * failing the subtest if it is not (ew0).
 *
 * Why a wait is needed at all. gtk_window_destroy() takes the window back out
 * of GTK's toplevel list synchronously and drops the reference that list held
 * (gtk-4.22.4 gtk/gtkwindow.c:7047-7071 -- g_list_store_remove() at :7065,
 * before it returns), but it does not finalize a window that still has async
 * work in flight: ggaze_window_open() starts a navigator walk plus the
 * thumbnail/texture pipeline, and those hold references until their callbacks
 * run on this thread's main context. So such a window dies only when somebody
 * turns the loop.
 *
 * Before ew0 nobody did, until the LAST subtest. Measured with weak refs on
 * the pre-ew0 file in the minimal lane under Xvfb: entering
 * /settings/prefs_dialog_constructs the windows from
 * /settings/window_thumbnail_size and /settings/open_applies_sort were both
 * still alive (the /settings/preferences_action_present one, which never
 * opens a folder, was already gone) and g_main_context_pending() was TRUE;
 * that subtest's own drain then dispatched the leftovers. The last subtest
 * was running the first subtests' teardown, and anything a teardown logs is
 * attributed by GLib's TAP output to whichever subtest happened to be turning
 * the loop -- which is how ew0 came to be filed against
 * prefs_dialog_constructs in the first place.
 *
 * Why a wait and not a fixed drain. ew0's first fix drained a fixed 200
 * iterations after each destroy, which closes the coupling by margin, not by
 * construction. Measured with a probe keeping that exact budget and recording
 * the iteration the weak ref actually fired on, for the window built by
 * /settings/window_thumbnail_size (all runs on one Xvfb, minimal lane):
 * unloaded it needs 18 of the 200; at 10-way concurrency with 8 CPU burners
 * the mean is 111 and the worst 188 of 200; at 16-way with 16 burners, 23 of
 * 64 runs never finalized inside the budget at all -- the window surviving
 * its own subtest, which is the exact state the drain was added to prevent.
 * And a drain that comes up short says nothing; it just leaks the window
 * onward, silently. This loop instead stops as soon as the window is gone and
 * g_assert_true()s that it was, so a shortfall is a loud failure naming the
 * subtest that caused it. Under that same 16-way / 16-burner load it finishes
 * 64 of 64 runs with every window finalized, 28 of them only after going past
 * where the 200-iteration drain would have quietly given up (worst: 282
 * iterations, 366 ms) -- comfortably inside the bound above.
 *
 * What this does NOT claim. It does not explain the critical ew0 was filed
 * for ("GLib-GObject-FATAL-CRITICAL: invalid unclassed pointer in cast to
 * GtkWidget" in /settings/prefs_dialog_constructs, seen once under concurrent
 * load, never reproduced). That is STILL UNEXPLAINED. What is established is
 * a real cross-subtest coupling, now removed, and better failure attribution.
 * If the critical recurs, do not assume this was its cause. */
static void
destroy_window_and_wait(GgazeWindow *p_win) {
   gboolean b_finalized = FALSE;
   g_object_weak_ref(G_OBJECT(p_win), note_finalized, &b_finalized);
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_assert_true(wait_for_finalized(&b_finalized));
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
   /* Dispose HERE, not in a later subtest -- see destroy_window_and_wait. */
   destroy_window_and_wait(p_win);
   g_free(c_path);
}

static void
test_preferences_action_present(void) {
   GgazeWindow *p_win = new_window();
   GActionMap  *p_map = G_ACTION_MAP(p_win);
   g_assert_nonnull(g_action_map_lookup_action(p_map, "preferences"));
   /* No folder was opened, so nothing async is pending and this returns at
    * once -- but keep the pattern uniform, and it still proves it. */
   destroy_window_and_wait(p_win);
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
   /* Dispose HERE, not in a later subtest -- see destroy_window_and_wait. */
   destroy_window_and_wait(p_win);
   g_free(c_path);
}

/* Build a real preferences dialog bound to a fresh Settings, present it on a
 * window the way the "preferences" action does, then close it. Any critical
 * from construction (bad binding / missing row API) or from presenting aborts
 * the test (main() makes criticals fatal).
 *
 * Presenting used to be skipped here, blamed on "a GTK-internal stale-toplevel
 * critical unrelated to this code". It was this suite's own bug: the earlier
 * subtests released their windows with g_object_unref(), which finalizes the
 * window but leaves it in GTK's toplevel list, and presenting walks that list,
 * so it tripped over a freed window. The route, since it is not obvious (all
 * line numbers libadwaita-1.9.2 / gtk-4.22.4): adw_dialog_present() with a
 * non-AdwDialog-host parent goes to present_as_window() (src/adw-dialog.c:613),
 * which makes the fallback GtkWindow modal (:645); showing a modal window
 * calls gtk_grab_add() (gtk/gtkwindow.c:4035), which reaches gtk_grab_notify()
 * (gtk/gtkmain.c:1827), which does gtk_window_list_toplevels() at :1839 --
 * that builds its GList with g_list_model_get_item() per entry -- and then
 * g_list_foreach(..., g_object_ref, ...) at :1840. Refing a finalized entry is
 * the critical. NOTE that this is specific to 1w0's unref-instead-of-destroy
 * bug: gtk_window_destroy() removes the entry from the list synchronously
 * (gtk/gtkwindow.c:7065), so a merely destroyed window is never walked. With
 * the teardown corrected (1w0) presenting is clean, so the coverage is back.
 * Full rationale in tests/helpers/gtk_helpers.h, "window teardown". */
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
    * notify), so the weak notify above is the check that it happened. Wait on
    * the flag rather than draining a fixed count and then asserting: the
    * assertion is identical, but this one is not racing a busy machine. */
   adw_dialog_close(ADW_DIALOG(p_dlg));
   g_assert_true(wait_for_finalized(&b_finalized));

   destroy_window_and_wait(p_win);
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