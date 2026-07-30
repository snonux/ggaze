/*:*
 * ggaze — delete-safety regression integration test
 *
 * Regression for eu0: _action_delete opened a GtkAlertDialog for >1 marks and
 * _delete_confirm_cb re-read p_win->p_nav marks after the async dialog
 * resolved. Because ggaze is single-instance, another open / drop can replace
 * the folder while the dialog is pending; confirming then permanently deleted
 * marks from the NEW folder rather than the files named by the prompt.
 *
 * The fix captures an immutable target list + owning directory at prompt time
 * and refuses to delete if the folder was replaced (window.h exposes
 * ggaze_window_delete_targets_still_current + ggaze_window_delete_captured so
 * the post-dialog decision is testable without driving the async native
 * dialog). This test exercises both branches directly:
 *
 *   1. folder-replaced: capture folder A's dir + targets, open folder B, then
 *      ggaze_window_delete_captured(win, dirA, capturedA) -> FALSE and NO file
 *      in either folder is deleted.
 *   2. happy path: capture folder A's targets, call
 *      ggaze_window_delete_captured(win, dirA, capturedA) while A is still
 *      open -> exactly the captured files are gone, the rest remain.
 *
 * Driving the real GtkAlertDialog programmatically is impractical (GTK4
 * exposes no API to inject a response), so the post-dialog decision function
 * is exercised directly — it is the exact code the real callback runs.
 *
 * Needs a display (integration suite; CI uses xvfb).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "viewer.h"
#include "window.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

/* --- helpers ------------------------------------------------------------ */

/* Windows built here are torn down with gtk_window_destroy(), never a plain
 * g_object_unref(): GTK4 hands the caller's reference to its internal
 * toplevel list and only destroy() takes the entry back out (it drops that
 * reference too, so the window still finalizes). Full rationale in
 * tests/helpers/gtk_helpers.h, "window teardown". */
static GgazeWindow *
new_window(void) {
   return (GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL)));
}

static void
drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

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

static void
cleanup_temp_dir(char *c_dir) {
   GFile           *p_dir = g_file_new_for_path(c_dir);
   GFileEnumerator *p_e =
      g_file_enumerate_children(p_dir, "standard::name,standard::type",
                                G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (p_e != NULL) {
      GFileInfo *p_info;
      while ((p_info = g_file_enumerator_next_file(p_e, NULL, NULL)) != NULL) {
         GFile *p_child = g_file_get_child(p_dir, g_file_info_get_name(p_info));
         if (g_file_info_get_file_type(p_info) == G_FILE_TYPE_DIRECTORY) {
            GFileEnumerator *p_e2 = g_file_enumerate_children(
               p_child, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (p_e2 != NULL) {
               GFileInfo *p_i2;
               while ((p_i2 = g_file_enumerator_next_file(p_e2, NULL, NULL)) !=
                      NULL) {
                  GFile *p_c2 =
                     g_file_get_child(p_child, g_file_info_get_name(p_i2));
                  g_file_delete(p_c2, NULL, NULL);
                  g_object_unref(p_c2);
                  g_object_unref(p_i2);
               }
               g_object_unref(p_e2);
            }
         }
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

static gboolean
path_exists(const char *c_path) {
   GFile   *p_f = g_file_new_for_path(c_path);
   gboolean b   = g_file_query_exists(p_f, NULL);
   g_object_unref(p_f);
   return (b);
}

/* Build an owned GFile* list from c_dir + c_names (transfer-full per item). */
static GList *
build_file_list(const char *c_dir, const char *const *c_names, guint u_n) {
   GList *p_list = NULL;
   for (guint i = 0; i < u_n; i++) {
      char  *c_p = g_build_filename(c_dir, c_names[i], NULL);
      GFile *p_f = g_file_new_for_path(c_p);
      p_list     = g_list_prepend(p_list, p_f);
      g_free(c_p);
   }
   return (g_list_reverse(p_list));
}

/* --- subtests ----------------------------------------------------------- */

/* Folder-replaced branch: capture folder A's dir + targets, open folder B
 * (replacing p_nav, as a single-instance open / drop would), then call the
 * post-dialog decision. It must refuse (FALSE) and delete NOTHING in either
 * folder. This is the data-loss scenario from eu0. */
static void
test_delete_refused_after_folder_replaced(void) {
   GError *p_err  = NULL;
   char   *c_dirA = g_dir_make_tmp("ggaze-delsafe-a-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   char *c_dirB = g_dir_make_tmp("ggaze-delsafe-b-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dirA, "plain.jpg");
   copy_fixture(c_dirA, "rot6.jpg");
   copy_fixture(c_dirA, "small.png");
   copy_fixture(c_dirB, "plain.jpg");
   copy_fixture(c_dirB, "rot6.jpg");

   char        *c_a0  = g_build_filename(c_dirA, "plain.jpg", NULL);
   GFile       *p_a0  = g_file_new_for_path(c_a0);
   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_a0);
   wait_for_load(p_win);

   /* Mark all 3 in folder A (the bulk-delete prompt condition: >1 marks). */
   fire(p_win, "win.mark-all");
   drain_main(100);

   /* Capture the prompt-time context: the owning dir + the target list. */
   GFile            *p_dirA      = g_file_new_for_path(c_dirA);
   const char *const c_a_names[] = {"plain.jpg", "rot6.jpg", "small.png"};
   GList            *p_captured =
      build_file_list(c_dirA, c_a_names, G_N_ELEMENTS(c_a_names));

   /* The folder is still A: still-current must be TRUE before replacement. */
   g_assert_true(ggaze_window_delete_targets_still_current(p_win, p_dirA));

   /* Replace the folder while the (notional) dialog is pending, exactly as a
    * single-instance open / drop does. */
   char  *c_b0 = g_build_filename(c_dirB, "plain.jpg", NULL);
   GFile *p_b0 = g_file_new_for_path(c_b0);
   ggaze_window_open(p_win, p_b0); /* swaps p_nav to folder B */
   drain_main(200);
   GFile *p_dirB = g_file_new_for_path(c_dirB);
   g_assert_true(ggaze_window_delete_targets_still_current(p_win, p_dirB));
   /* The captured folder-A dir is no longer current -> must be FALSE. */
   g_assert_false(ggaze_window_delete_targets_still_current(p_win, p_dirA));

   /* Confirm the dialog affirmatively: the safety check must refuse. */
   gboolean b_proceeded =
      ggaze_window_delete_captured(p_win, p_dirA, p_captured);
   g_assert_false(b_proceeded);
   drain_main(200);

   /* No file in either folder was deleted. */
   for (guint i = 0; i < G_N_ELEMENTS(c_a_names); i++) {
      char *c_p = g_build_filename(c_dirA, c_a_names[i], NULL);
      g_assert_true(path_exists(c_p));
      g_free(c_p);
   }
   const char *const c_b_names[] = {"plain.jpg", "rot6.jpg"};
   for (guint i = 0; i < G_N_ELEMENTS(c_b_names); i++) {
      char *c_p = g_build_filename(c_dirB, c_b_names[i], NULL);
      g_assert_true(path_exists(c_p));
      g_free(c_p);
   }

   g_list_free_full(p_captured, (GDestroyNotify)g_object_unref);
   g_object_unref(p_dirA);
   g_object_unref(p_dirB);
   g_object_unref(p_b0);
   g_free(c_b0);
   g_object_unref(p_a0);
   g_free(c_a0);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dirA);
   cleanup_temp_dir(c_dirB);
}

/* Happy path: with the folder still open, ggaze_window_delete_captured deletes
 * EXACTLY the captured files (not a re-read of marks) and leaves the rest. */
static void
test_delete_captured_deletes_only_targets(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-delsafe-happy-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   copy_fixture(c_dir, "rot6.jpg");
   copy_fixture(c_dir, "small.png");

   char        *c_p0  = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile       *p_f0  = g_file_new_for_path(c_p0);
   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_f0);
   wait_for_load(p_win);

   /* Capture two of the three files as the prompt-time targets. */
   GFile            *p_dir    = g_file_new_for_path(c_dir);
   const char *const c_del[]  = {"plain.jpg", "rot6.jpg"};
   const char *const c_keep[] = {"small.png"};
   GList *p_captured = build_file_list(c_dir, c_del, G_N_ELEMENTS(c_del));

   g_assert_true(ggaze_window_delete_targets_still_current(p_win, p_dir));
   gboolean b_proceeded =
      ggaze_window_delete_captured(p_win, p_dir, p_captured);
   g_assert_true(b_proceeded);
   drain_main(300);

   /* The captured files are gone; the un-captured file remains. */
   for (guint i = 0; i < G_N_ELEMENTS(c_del); i++) {
      char *c_p = g_build_filename(c_dir, c_del[i], NULL);
      g_assert_false(path_exists(c_p));
      g_free(c_p);
   }
   for (guint i = 0; i < G_N_ELEMENTS(c_keep); i++) {
      char *c_p = g_build_filename(c_dir, c_keep[i], NULL);
      g_assert_true(path_exists(c_p));
      g_free(c_p);
   }

   g_list_free_full(p_captured, (GDestroyNotify)g_object_unref);
   g_object_unref(p_dir);
   g_object_unref(p_f0);
   g_free(c_p0);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* NULL-nav branch: with nothing open, no captured dir is "still current" and
 * ggaze_window_delete_captured refuses without touching anything. */
static void
test_delete_targets_still_current_no_nav(void) {
   GgazeWindow *p_win = new_window();
   GFile       *p_any = g_file_new_for_path("/tmp");
   g_assert_false(ggaze_window_delete_targets_still_current(p_win, p_any));
   g_assert_false(ggaze_window_delete_captured(p_win, p_any, NULL));
   g_object_unref(p_any);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(100);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
   if (!gtk_init_check()) {
      g_test_skip("no display available (run under xvfb)");
      return (g_test_run());
   }
   g_test_add_func("/delete_safety/refused_after_folder_replaced",
                   test_delete_refused_after_folder_replaced);
   g_test_add_func("/delete_safety/captured_deletes_only_targets",
                   test_delete_captured_deletes_only_targets);
   g_test_add_func("/delete_safety/still_current_no_nav",
                   test_delete_targets_still_current_no_nav);
   return (g_test_run());
}