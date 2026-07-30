/*:*
 * ggaze — run-script rescan integration test (ru0)
 *
 * Exercises the `!` run-script wiring in the window: the public helper
 * ggaze_window_run_script_index() runs a configured script via /bin/sh -c with
 * %f (current file) and %d (folder) expanded, asynchronously; on completion it
 * rescans the navigator so script-created files appear, and reports status.
 * The popup it drives lists the configured scripts with auto-assigned
 * 1-9,0,a-z hotkeys and a key controller (mirrors the `e` open-external popup).
 *
 * The central correctness case (mirrors eu0's folder-identity guard): the
 * folder the script ran against is captured at launch time, and the async
 * completion callback rescans ONLY if the window still navigates that same
 * folder. If a single-instance open / drop replaced the folder while the script
 * ran, the new folder is NOT rescanned (the script never touched it). Uses the
 * memory GSettings backend + build-tree schema; display-gated, skipped without
 * a display.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "gridview.h"
#include "runner.h"
#include "settings.h"
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

/* Reset the scripts a(ss) key to empty via the shared memory backend, so tests
 * don't leak scripts into each other. */
static void
reset_scripts(void) {
   Settings *p_s = settings_new();
   g_assert_nonnull(p_s);
   g_settings_reset(settings_get_gsettings(p_s), "scripts");
   settings_delete(p_s);
}

/* Configure a single script with the given command. */
static void
set_one_script(const char *c_name, const char *c_command) {
   GPtrArray    *p_pairs = g_ptr_array_new_with_free_func(settings_pair_free);
   SettingsPair *pr      = g_new(SettingsPair, 1);
   pr->c_name            = g_strdup(c_name);
   pr->c_value           = g_strdup(c_command);
   g_ptr_array_add(p_pairs, pr);
   Settings *p_s = settings_new();
   g_assert_nonnull(p_s);
   g_assert_cmpint(settings_set_scripts(p_s, p_pairs), ==, 1);
   settings_delete(p_s);
   g_ptr_array_unref(p_pairs);
}

/* Configure N named scripts with harmless `true %f` commands. */
static void
set_named_scripts(guint u_n) {
   GPtrArray  *p_pairs   = g_ptr_array_new_with_free_func(settings_pair_free);
   const char *c_names[] = {"usbimport", "build contact sheet", "cleanup"};
   for (guint i = 0; i < u_n; i++) {
      SettingsPair *pr = g_new(SettingsPair, 1);
      pr->c_name       = g_strdup(c_names[i % G_N_ELEMENTS(c_names)]);
      pr->c_value      = g_strdup("true %f");
      g_ptr_array_add(p_pairs, pr);
   }
   Settings *p_s = settings_new();
   g_assert_nonnull(p_s);
   settings_set_scripts(p_s, p_pairs);
   settings_delete(p_s);
   g_ptr_array_unref(p_pairs);
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

static GgazeGrid *
window_grid(GgazeWindow *p_win) {
   GtkStack *p_stack = ggaze_window_get_stack(p_win);
   return (GGAZE_GRID(gtk_stack_get_child_by_name(p_stack, "grid")));
}

static gboolean
path_exists(const char *c_path) {
   GFile   *p_f = g_file_new_for_path(c_path);
   gboolean b   = g_file_query_exists(p_f, NULL);
   g_object_unref(p_f);
   return (b);
}

/* Wait until the grid lists u_expected entries (polling while draining the main
 * context so the async script completion + rescan land). Returns TRUE on
 * success, FALSE on timeout. */
static gboolean
wait_for_grid_count(GgazeWindow *p_win, guint u_expected, guint u_timeout_ms) {
   for (guint u = 0; u < u_timeout_ms; u += 20) {
      if (ggaze_grid_get_count(window_grid(p_win)) == u_expected) {
         return (TRUE);
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(20000);
   }
   return (ggaze_grid_get_count(window_grid(p_win)) == u_expected);
}

/* Find the GtkPopover parented to the window's stack (the run-script popover is
 * gtk_widget_set_parent'd to p_stack). Returns the first GtkPopover among the
 * stack's direct children, or NULL. */
static GtkPopover *
find_run_script_popover(GgazeWindow *p_win) {
   GtkStack  *p_stack = ggaze_window_get_stack(p_win);
   GtkWidget *p_child = gtk_widget_get_first_child(GTK_WIDGET(p_stack));
   while (p_child != NULL) {
      if (GTK_IS_POPOVER(p_child)) {
         return (GTK_POPOVER(p_child));
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (NULL);
}

static guint
popover_button_count(GtkPopover *p_pop) {
   GtkWidget *p_box = gtk_popover_get_child(p_pop);
   if (p_box == NULL) {
      return (0);
   }
   guint      u_n     = 0;
   GtkWidget *p_child = gtk_widget_get_first_child(p_box);
   while (p_child != NULL) {
      if (GTK_IS_BUTTON(p_child)) {
         u_n++;
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (u_n);
}

static const char *
popover_button_label(GtkPopover *p_pop, guint u_idx) {
   GtkWidget *p_box = gtk_popover_get_child(p_pop);
   g_assert_nonnull(p_box);
   GtkWidget *p_child = gtk_widget_get_first_child(p_box);
   guint      u_i     = 0;
   while (p_child != NULL) {
      if (GTK_IS_BUTTON(p_child)) {
         if (u_i == u_idx) {
            return (gtk_button_get_label(GTK_BUTTON(p_child)));
         }
         u_i++;
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (NULL);
}

static gboolean
popover_has_key_controller(GtkPopover *p_pop) {
   GListModel *p_ctrls = gtk_widget_observe_controllers(GTK_WIDGET(p_pop));
   gboolean    b_found = FALSE;
   guint       u_n     = g_list_model_get_n_items(p_ctrls);
   for (guint i = 0; i < u_n; i++) {
      GObject *p_obj = g_list_model_get_item(p_ctrls, i);
      if (GTK_IS_EVENT_CONTROLLER_KEY(p_obj)) {
         b_found = TRUE;
      }
      g_object_unref(p_obj);
   }
   g_object_unref(p_ctrls);
   return (b_found);
}

/* --- subtests ----------------------------------------------------------- */

/* A script that copies the current file to a new name in the folder, on
 * completion the navigator rescans and the new file appears in the grid. */
static void
test_runner_rescan_creates_file(void) {
   reset_scripts();
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-rr-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   char  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   /* cp %f %d/imported.jpg -> cp '/dir/plain.jpg' '/dir'/imported.jpg (sh
    * concatenates the adjacent quoted segments). Creates a real JPEG so the
    * navigator lists it after the rescan. */
   set_one_script("import", "cp %f %d/imported.jpg");

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_file);
   wait_for_load(p_win);
   g_assert_cmpint(ggaze_grid_get_count(window_grid(p_win)), ==, 1);

   g_assert_true(ggaze_window_run_script_index(p_win, 0));
   g_assert_true(wait_for_grid_count(p_win, 2, 4000));
   char *c_new = g_build_filename(c_dir, "imported.jpg", NULL);
   g_assert_true(path_exists(c_new));
   g_free(c_new);

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   g_free(c_path);
   cleanup_temp_dir(c_dir);
   reset_scripts();
}

/* A failing script (non-zero exit) does not crash, does not change the
 * listing, and the completion path is taken (no hang). */
static void
test_runner_failure_no_crash(void) {
   reset_scripts();
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-rr-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   char  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   set_one_script("bad", "false");

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_file);
   wait_for_load(p_win);
   g_assert_cmpint(ggaze_grid_get_count(window_grid(p_win)), ==, 1);

   g_assert_true(ggaze_window_run_script_index(p_win, 0));
   /* Drain long enough for the async completion + (refused-or-not) rescan. */
   g_assert_true(wait_for_grid_count(p_win, 1, 3000));
   g_assert_cmpint(ggaze_grid_get_count(window_grid(p_win)), ==, 1);

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   g_free(c_path);
   cleanup_temp_dir(c_dir);
   reset_scripts();
}

/* The popup: firing win.run-script pops up a GtkPopover listing the configured
 * scripts as clickable rows with auto-assigned hotkey labels (1, 2, 3 ...) and
 * a key controller; firing it again toggles it closed. */
static void
test_runner_popup_structure(void) {
   reset_scripts();
   set_named_scripts(3);
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-rr-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   char  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_file);
   wait_for_load(p_win);

   g_assert_null(find_run_script_popover(p_win));
   fire(p_win, "win.run-script");
   drain_main(200);
   GtkPopover *p_pop = find_run_script_popover(p_win);
   g_assert_nonnull(p_pop);
   g_assert_cmpint(popover_button_count(p_pop), ==, 3);
   g_assert_cmpstr(popover_button_label(p_pop, 0), ==, "1  usbimport");
   g_assert_cmpstr(popover_button_label(p_pop, 1), ==,
                   "2  build contact sheet");
   g_assert_cmpstr(popover_button_label(p_pop, 2), ==, "3  cleanup");
   g_assert_true(popover_has_key_controller(p_pop));

   /* Toggle: a second win.run-script closes it. */
   fire(p_win, "win.run-script");
   drain_main(200);
   g_assert_null(find_run_script_popover(p_win));

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   g_free(c_path);
   cleanup_temp_dir(c_dir);
   reset_scripts();
}

/* The empty-scripts case still pops up a (message-only) popover rather than
 * silently doing nothing, so the user sees why `!` did nothing. */
static void
test_runner_popup_empty_message(void) {
   reset_scripts();
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-rr-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   char  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_file);
   wait_for_load(p_win);
   fire(p_win, "win.run-script");
   drain_main(200);
   GtkPopover *p_pop = find_run_script_popover(p_win);
   g_assert_nonnull(p_pop);
   g_assert_cmpint(popover_button_count(p_pop), ==, 0); /* message, no rows */

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   g_free(c_path);
   cleanup_temp_dir(c_dir);
   reset_scripts();
}

/* With no scripts configured, the helper is a safe no-op (returns FALSE, no
 * crash, no subprocess). */
static void
test_runner_empty_no_scripts(void) {
   reset_scripts();
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-rr-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   char  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_file);
   wait_for_load(p_win);
   g_assert_false(ggaze_window_run_script_index(p_win, 0));
   g_assert_false(ggaze_window_run_script_index(p_win, 99));

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   g_free(c_path);
   cleanup_temp_dir(c_dir);
   reset_scripts();
}

/* The folder-identity guard (mirrors eu0): launch a slow script against folder
 * A (capturing dir=A), replace the folder with B while it runs, then let it
 * complete. The script still creates its file in A (the subprocess runs against
 * A's path), but the completion callback must NOT rescan the new folder B. So
 * B's listing stays at its original count and B never gains the new file. */
static void
test_runner_rescan_refused_after_folder_replaced(void) {
   reset_scripts();
   GError *p_err  = NULL;
   char   *c_dirA = g_dir_make_tmp("ggaze-rr-a-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   char *c_dirB = g_dir_make_tmp("ggaze-rr-b-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dirA, "plain.jpg");
   copy_fixture(c_dirB, "rot6.jpg");
   char  *c_a0 = g_build_filename(c_dirA, "plain.jpg", NULL);
   GFile *p_a0 = g_file_new_for_path(c_a0);
   char  *c_b0 = g_build_filename(c_dirB, "rot6.jpg", NULL);
   GFile *p_b0 = g_file_new_for_path(c_b0);
   /* Sleep briefly so the folder can be replaced before completion, then copy
    * the current file to a new name in the captured folder A. */
   set_one_script("slowimport", "sleep 0.3; cp %f %d/imported.jpg");

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_a0);
   wait_for_load(p_win);
   g_assert_cmpint(ggaze_grid_get_count(window_grid(p_win)), ==, 1);

   g_assert_true(ggaze_window_run_script_index(p_win, 0));
   drain_main(100); /* let the script start (capturing dir=A) */

   /* Replace the folder while the script is still running. */
   ggaze_window_open(p_win, p_b0); /* swaps p_nav to folder B */
   drain_main(200);
   g_assert_cmpint(ggaze_grid_get_count(window_grid(p_win)), ==, 1);

   /* Wait for the script to finish; the callback must NOT rescan B. B's grid
    * count stays 1 (only rot6.jpg) and B never gains imported.jpg. */
   g_assert_true(wait_for_grid_count(p_win, 1, 4000));
   char *c_b_new = g_build_filename(c_dirB, "imported.jpg", NULL);
   g_assert_false(path_exists(c_b_new));
   g_free(c_b_new);

   /* The script DID run against A: imported.jpg exists in A on disk (the
    * subprocess is independent of p_nav), even though A was never rescanned. */
   char *c_a_new = g_build_filename(c_dirA, "imported.jpg", NULL);
   g_assert_true(path_exists(c_a_new));
   g_free(c_a_new);

   g_object_unref(p_a0);
   g_object_unref(p_b0);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   g_free(c_a0);
   g_free(c_b0);
   cleanup_temp_dir(c_dirA);
   cleanup_temp_dir(c_dirB);
   reset_scripts();
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
   g_test_add_func("/runner_rescan/creates_file",
                   test_runner_rescan_creates_file);
   g_test_add_func("/runner_rescan/failure_no_crash",
                   test_runner_failure_no_crash);
   g_test_add_func("/runner_rescan/popup_structure",
                   test_runner_popup_structure);
   g_test_add_func("/runner_rescan/popup_empty_message",
                   test_runner_popup_empty_message);
   g_test_add_func("/runner_rescan/empty_no_scripts",
                   test_runner_empty_no_scripts);
   g_test_add_func("/runner_rescan/refused_after_folder_replaced",
                   test_runner_rescan_refused_after_folder_replaced);
   return (g_test_run());
}