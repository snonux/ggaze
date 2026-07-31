/*:*
 * ggaze — move + undo integration test
 *
 * Exercises the `m` move-to-destination wiring in the window: the public
 * launch helper ggaze_window_move_index() moves the marks-or-current set via
 * mover_move (collision-suffixed), dims the moved files in the navigator/grid
 * (mirroring trash's navigator_mark_removed), and the popup it drives lists
 * the configured destinations with auto-assigned 1-9,0,a-z hotkeys and a key
 * controller (same pattern as `e`/`!`, see test_open_external.c). Also
 * exercises the unified `u` undo: it undoes whichever of the last trash or
 * the last move happened most recently (docs/ui-and-interactions.md
 * "Selection & moving"), that folder switches reset BOTH engines' undo
 * state so a stale record from a folder no longer open can never be
 * (re)undone, and negative cases (nothing to undo, every target colliding,
 * a destination that is really a regular file). Uses the memory GSettings
 * backend + build-tree schema (set in the meson env); display-gated,
 * skipped without a display.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "gridview.h"
#include "gtk_helpers.h"
#include "settings.h"
#include "window.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

/* --- helpers ------------------------------------------------------------ */

/* Windows built here are torn down with gtk_window_destroy(), never a plain
 * g_object_unref(): GTK4 hands the caller's reference to its internal
 * toplevel list and only destroy() takes the entry back out (it drops that
 * reference too, so the window still finalizes). Full rationale in
 * tests/helpers/gtk_helpers.h, "window teardown".
 *
 * The focus grab is not cosmetic (5w0): this suite pops up the `m`
 * destination popover, and a never-presented toplevel has no focus widget,
 * which walks GTK into an unguarded NULL on the X11 backend CI runs under.
 * See ggtest_focus_viewer() in tests/helpers/gtk_helpers.h for the full
 * mechanism. */
static GgazeWindow *
new_window(void) {
   GgazeWindow *p_win = GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL));
   ggtest_focus_viewer(p_win);
   return (p_win);
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

/* Recursively delete and free a temp directory (destinations can end up with
 * nested subfolders in principle; kept flat here but written defensively). */
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
            /* recursively clean a subdir (.Trash) so a file trapped in it
             * (e.g. by test_undo_folder_switch_does_not_undo_stale_move,
             * which deliberately never restores it) doesn't leave the
             * subdir -- and consequently this outer temp dir -- non-empty
             * and undeletable. */
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

/* Reset the destinations a(ss) key to empty via the shared memory backend, so
 * tests don't leak destinations into each other. */
static void
reset_destinations(void) {
   Settings *p_s = settings_new();
   g_assert_nonnull(p_s);
   g_settings_reset(settings_get_gsettings(p_s), "destinations");
   settings_delete(p_s);
}

/* Configure the named (name, absolute-path) destinations, in order. */
static void
set_destinations(const char *const *c_names, const char *const *c_paths,
                 guint u_n) {
   GPtrArray *p_pairs = g_ptr_array_new_with_free_func(settings_pair_free);
   for (guint i = 0; i < u_n; i++) {
      SettingsPair *pr = g_new(SettingsPair, 1);
      pr->c_name       = g_strdup(c_names[i]);
      pr->c_value      = g_strdup(c_paths[i]);
      g_ptr_array_add(p_pairs, pr);
   }
   Settings *p_s = settings_new();
   g_assert_nonnull(p_s);
   g_assert_cmpint(settings_set_destinations(p_s, p_pairs), ==, u_n);
   settings_delete(p_s);
   g_ptr_array_unref(p_pairs);
}

/* Fire a win.* action on the window. */
static void
fire(GgazeWindow *p_win, const char *c_action) {
   gtk_widget_activate_action(GTK_WIDGET(p_win), c_action, NULL);
}

/* A tmp dir with plain.jpg/rot6.jpg/small.png; *p_plain_out gets plain.jpg. */
static char *
setup_dir_with_three(GFile **p_plain_out) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-move-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   copy_fixture(c_dir, "rot6.jpg");
   copy_fixture(c_dir, "small.png");
   char  *c_path  = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_plain = g_file_new_for_path(c_path);
   g_free(c_path);
   *p_plain_out = p_plain;
   return (c_dir);
}

static gboolean
path_exists(const char *c_dir, const char *c_name) {
   char    *c_p = g_build_filename(c_dir, c_name, NULL);
   GFile   *p_f = g_file_new_for_path(c_p);
   gboolean b   = g_file_query_exists(p_f, NULL);
   g_object_unref(p_f);
   g_free(c_p);
   return (b);
}

/* Find the GtkPopover parented to the window's stack (the move popover is
 * gtk_widget_set_parent'd to p_stack, same as open-external/run-script). */
static GtkPopover *
find_move_popover(GgazeWindow *p_win) {
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

/* --- subtests ----------------------------------------------------------- */

/* No marks: `m` acts on the current file. It leaves the source folder and
 * lands in the chosen destination; the header's title count is unaffected
 * (nothing is marked to begin with). */
static void
test_move_current_no_marks(void) {
   reset_destinations();
   GFile  *p_plain = NULL;
   char   *c_dir   = setup_dir_with_three(&p_plain);
   GError *p_err   = NULL;
   char   *c_dest  = g_dir_make_tmp("ggaze-move-dest-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   const char *c_names[] = {"dest one"};
   const char *c_paths[] = {c_dest};
   set_destinations(c_names, c_paths, 1);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   drain_main(200);

   g_assert_true(path_exists(c_dir, "plain.jpg"));
   g_assert_true(ggaze_window_move_index(p_win, 0));
   drain_main(200);

   g_assert_false(path_exists(c_dir, "plain.jpg"));
   g_assert_true(path_exists(c_dest, "plain.jpg"));

   g_object_unref(p_plain);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
   cleanup_temp_dir(c_dest);
   reset_destinations();
}

/* Multiple marks: `m` moves every marked file, not just the current one. */
static void
test_move_multiple_marks(void) {
   reset_destinations();
   GFile  *p_plain = NULL;
   char   *c_dir   = setup_dir_with_three(&p_plain);
   GError *p_err   = NULL;
   char   *c_dest  = g_dir_make_tmp("ggaze-move-dest-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   const char *c_names[] = {"dest one"};
   const char *c_paths[] = {c_dest};
   set_destinations(c_names, c_paths, 1);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   drain_main(200);

   fire(p_win, "win.mark");
   fire(p_win, "win.next");
   drain_main(100);
   fire(p_win, "win.mark"); /* plain.jpg + rot6.jpg marked */
   drain_main(100);

   g_assert_true(ggaze_window_move_index(p_win, 0));
   drain_main(200);

   g_assert_false(path_exists(c_dir, "plain.jpg"));
   g_assert_false(path_exists(c_dir, "rot6.jpg"));
   g_assert_true(path_exists(c_dest, "plain.jpg"));
   g_assert_true(path_exists(c_dest, "rot6.jpg"));
   g_assert_true(path_exists(c_dir, "small.png")); /* untouched, unmarked */

   /* Moved files' marks are cleared (mirrors trash: navigator_mark_removed
    * drops the mark of every file it removes). */
   const gchar *c_title = gtk_window_get_title(GTK_WINDOW(p_win));
   g_assert_null(g_strstr_len(c_title, -1, " marked"));

   g_object_unref(p_plain);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
   cleanup_temp_dir(c_dest);
   reset_destinations();
}

/* A same-named file already at the destination: the moved file lands as
 * "plain-1.jpg" instead of overwriting (mover_move's collision suffixing). */
static void
test_move_collision_suffix(void) {
   reset_destinations();
   GFile  *p_plain = NULL;
   char   *c_dir   = setup_dir_with_three(&p_plain);
   GError *p_err   = NULL;
   char   *c_dest  = g_dir_make_tmp("ggaze-move-dest-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dest, "plain.jpg"); /* pre-existing collision */
   const char *c_names[] = {"dest one"};
   const char *c_paths[] = {c_dest};
   set_destinations(c_names, c_paths, 1);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   drain_main(200);

   g_assert_true(ggaze_window_move_index(p_win, 0));
   drain_main(200);

   g_assert_false(path_exists(c_dir, "plain.jpg"));
   g_assert_true(path_exists(c_dest, "plain.jpg"));   /* the pre-existing one */
   g_assert_true(path_exists(c_dest, "plain-1.jpg")); /* the moved-in one */

   g_object_unref(p_plain);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
   cleanup_temp_dir(c_dest);
   reset_destinations();
}

/* `u` undoes the last move: the file returns to its original path. */
static void
test_move_then_undo(void) {
   reset_destinations();
   GFile  *p_plain = NULL;
   char   *c_dir   = setup_dir_with_three(&p_plain);
   GError *p_err   = NULL;
   char   *c_dest  = g_dir_make_tmp("ggaze-move-dest-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   const char *c_names[] = {"dest one"};
   const char *c_paths[] = {c_dest};
   set_destinations(c_names, c_paths, 1);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   drain_main(200);

   g_assert_true(ggaze_window_move_index(p_win, 0));
   drain_main(200);
   g_assert_false(path_exists(c_dir, "plain.jpg"));

   fire(p_win, "win.undo");
   drain_main(300);
   g_assert_true(path_exists(c_dir, "plain.jpg"));
   g_assert_false(path_exists(c_dest, "plain.jpg"));

   g_object_unref(p_plain);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
   cleanup_temp_dir(c_dest);
   reset_destinations();
}

/* Unified undo: `d` (trash) then `m` (move) then two `u`s undo the move
 * first (most recent), then the trash — one level of undo per engine, most-
 * recent-first (docs/ui-and-interactions.md "Undo"). */
static void
test_undo_prefers_most_recent(void) {
   reset_destinations();
   GFile  *p_plain = NULL;
   char   *c_dir   = setup_dir_with_three(&p_plain);
   GError *p_err   = NULL;
   char   *c_dest  = g_dir_make_tmp("ggaze-move-dest-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   const char *c_names[] = {"dest one"};
   const char *c_paths[] = {c_dest};
   set_destinations(c_names, c_paths, 1);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   drain_main(200);

   /* Trash plain.jpg (current), then move rot6.jpg (advances to it first). */
   fire(p_win, "win.trash");
   drain_main(200);
   g_assert_true(path_exists(c_dir, ".Trash"));

   g_assert_true(ggaze_window_move_index(p_win, 0));
   drain_main(200);
   g_assert_false(path_exists(c_dir, "rot6.jpg"));
   g_assert_true(path_exists(c_dest, "rot6.jpg"));

   /* First `u`: undoes the move (most recent). */
   fire(p_win, "win.undo");
   drain_main(300);
   g_assert_true(path_exists(c_dir, "rot6.jpg"));
   g_assert_false(path_exists(c_dest, "rot6.jpg"));

   /* Second `u`: nothing left to undo for move, falls back to trash. */
   fire(p_win, "win.undo");
   drain_main(300);
   char  *c_trashpath = g_build_filename(c_dir, ".Trash", "plain.jpg", NULL);
   GFile *p_trashed   = g_file_new_for_path(c_trashpath);
   g_assert_false(g_file_query_exists(p_trashed, NULL));
   g_object_unref(p_trashed);
   g_free(c_trashpath);
   g_assert_true(path_exists(c_dir, "plain.jpg"));

   g_object_unref(p_plain);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
   cleanup_temp_dir(c_dest);
   reset_destinations();
}

/* Regression test for the folder-switch unified-undo bug: move a file in
 * folder A, trash a file in folder A (trash is now the preferred undo), then
 * switch to folder B and press `u`. Before the fix, ggaze_window_open() left
 * the mover's undo state (and e_last_destructive) untouched across the
 * folder switch, so trash's fresh (post-switch) can_undo()==FALSE fell
 * through to the STALE folder-A move and silently moved a file back into a
 * folder no longer on screen. After the fix, ggaze_window_open() clears both
 * engines' undo state (mover_clear_last + e_last_destructive reset), so `u`
 * in folder B must be a no-op: the moved file stays at its destination, and
 * nothing reappears in folder A. */
static void
test_undo_folder_switch_does_not_undo_stale_move(void) {
   reset_destinations();
   GFile  *p_plain = NULL;
   char   *c_dir_a = setup_dir_with_three(&p_plain);
   GError *p_err   = NULL;
   char   *c_dest  = g_dir_make_tmp("ggaze-move-dest-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   const char *c_names[] = {"dest one"};
   const char *c_paths[] = {c_dest};
   set_destinations(c_names, c_paths, 1);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   drain_main(200);

   /* Move plain.jpg (current), then trash whatever is current next
    * (rot6.jpg) -- trash becomes the preferred undo (most recent). */
   g_assert_true(ggaze_window_move_index(p_win, 0));
   drain_main(200);
   fire(p_win, "win.trash");
   drain_main(200);
   g_assert_false(path_exists(c_dir_a, "plain.jpg"));
   g_assert_true(path_exists(c_dest, "plain.jpg"));
   g_assert_true(path_exists(c_dir_a, ".Trash"));

   /* Switch to folder B. */
   char *c_dir_b = g_dir_make_tmp("ggaze-move-other-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir_b, "small.png");
   char  *c_path_b = g_build_filename(c_dir_b, "small.png", NULL);
   GFile *p_file_b = g_file_new_for_path(c_path_b);
   g_free(c_path_b);
   ggaze_window_open(p_win, p_file_b);
   drain_main(200);

   /* `u` in folder B: must not reach back into folder A's stale move. */
   fire(p_win, "win.undo");
   drain_main(300);

   g_assert_true(path_exists(c_dest, "plain.jpg"));   /* still moved-away */
   g_assert_false(path_exists(c_dir_a, "plain.jpg")); /* did NOT come back */
   g_assert_true(path_exists(c_dir_b, "small.png"));  /* folder B untouched */

   g_object_unref(p_plain);
   g_object_unref(p_file_b);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir_a);
   cleanup_temp_dir(c_dest);
   cleanup_temp_dir(c_dir_b);
   reset_destinations();
}

/* `u` with nothing to undo (fresh window, nothing moved or trashed yet):
 * a safe no-op, no crash, no state change on disk. */
static void
test_undo_nothing_to_undo(void) {
   reset_destinations();
   GFile *p_plain = NULL;
   char  *c_dir   = setup_dir_with_three(&p_plain);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   drain_main(200);

   fire(p_win, "win.undo");
   drain_main(200);

   g_assert_true(path_exists(c_dir, "plain.jpg"));
   g_assert_true(path_exists(c_dir, "rot6.jpg"));
   g_assert_true(path_exists(c_dir, "small.png"));
   g_assert_false(path_exists(c_dir, ".Trash"));

   g_object_unref(p_plain);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
   reset_destinations();
}

/* All marked targets collide with a pre-existing file at the destination:
 * every one of them must land collision-suffixed, not just the first. */
static void
test_move_all_targets_collide(void) {
   reset_destinations();
   GFile  *p_plain = NULL;
   char   *c_dir   = setup_dir_with_three(&p_plain);
   GError *p_err   = NULL;
   char   *c_dest  = g_dir_make_tmp("ggaze-move-dest-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   /* Pre-existing collision for every file about to be moved. */
   copy_fixture(c_dest, "plain.jpg");
   copy_fixture(c_dest, "rot6.jpg");
   copy_fixture(c_dest, "small.png");
   const char *c_names[] = {"dest one"};
   const char *c_paths[] = {c_dest};
   set_destinations(c_names, c_paths, 1);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   drain_main(200);

   fire(p_win, "win.mark-all"); /* mark all 3 */
   drain_main(100);

   g_assert_true(ggaze_window_move_index(p_win, 0));
   drain_main(200);

   g_assert_false(path_exists(c_dir, "plain.jpg"));
   g_assert_false(path_exists(c_dir, "rot6.jpg"));
   g_assert_false(path_exists(c_dir, "small.png"));

   g_assert_true(path_exists(c_dest, "plain.jpg"));   /* pre-existing */
   g_assert_true(path_exists(c_dest, "plain-1.jpg")); /* moved-in */
   g_assert_true(path_exists(c_dest, "rot6.jpg"));    /* pre-existing */
   g_assert_true(path_exists(c_dest, "rot6-1.jpg"));  /* moved-in */
   g_assert_true(path_exists(c_dest, "small.png"));   /* pre-existing */
   g_assert_true(path_exists(c_dest, "small-1.png")); /* moved-in */

   g_object_unref(p_plain);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
   cleanup_temp_dir(c_dest);
   reset_destinations();
}

/* A configured destination that is actually a regular file (not a
 * directory): mover_move's g_file_make_directory_with_parents() call sees
 * G_IO_ERROR_EXISTS (something is already there) and proceeds, but the
 * subsequent g_file_move() into a "directory" that is really a file fails
 * cleanly -- no crash, nothing moved, a failure status is reported. */
static void
test_move_destination_not_a_directory(void) {
   reset_destinations();
   GFile  *p_plain = NULL;
   char   *c_dir   = setup_dir_with_three(&p_plain);
   GError *p_err   = NULL;
   char   *c_tmp   = g_dir_make_tmp("ggaze-move-baddest-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   char *c_notdir = g_build_filename(c_tmp, "not-a-dir", NULL);
   g_assert_true(g_file_set_contents(c_notdir, "x", 1, &p_err));
   g_assert_no_error(p_err);
   const char *c_names[] = {"bad dest"};
   const char *c_paths[] = {c_notdir};
   set_destinations(c_names, c_paths, 1);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   drain_main(200);

   g_assert_false(ggaze_window_move_index(p_win, 0));
   drain_main(200);

   /* Nothing moved; the bogus "destination" file is untouched. */
   g_assert_true(path_exists(c_dir, "plain.jpg"));
   GFile *p_notdir = g_file_new_for_path(c_notdir);
   g_assert_cmpint(
      g_file_query_file_type(p_notdir, G_FILE_QUERY_INFO_NONE, NULL), ==,
      G_FILE_TYPE_REGULAR);
   g_object_unref(p_notdir);

   g_object_unref(p_plain);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   g_unlink(c_notdir);
   g_free(c_notdir);
   cleanup_temp_dir(c_dir);
   cleanup_temp_dir(c_tmp);
   reset_destinations();
}

/* The popup: firing win.move pops up a GtkPopover listing the configured
 * destinations as clickable rows with auto-assigned hotkey labels and a key
 * controller; firing it again toggles it closed. */
static void
test_move_popup_structure(void) {
   reset_destinations();
   GFile  *p_plain  = NULL;
   char   *c_dir    = setup_dir_with_three(&p_plain);
   GError *p_err    = NULL;
   char   *c_dest_a = g_dir_make_tmp("ggaze-move-dest-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   char *c_dest_b = g_dir_make_tmp("ggaze-move-dest-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   const char *c_names[] = {"irregular ninja", "alt irregular ninja"};
   const char *c_paths[] = {c_dest_a, c_dest_b};
   set_destinations(c_names, c_paths, 2);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   drain_main(200);

   g_assert_null(find_move_popover(p_win));
   fire(p_win, "win.move");
   drain_main(200);
   GtkPopover *p_pop = find_move_popover(p_win);
   g_assert_nonnull(p_pop);
   g_assert_cmpint(popover_button_count(p_pop), ==, 2);
   g_assert_cmpstr(popover_button_label(p_pop, 0), ==, "1  irregular ninja");
   g_assert_cmpstr(popover_button_label(p_pop, 1), ==,
                   "2  alt irregular ninja");

   /* Toggle: a second win.move closes it. */
   fire(p_win, "win.move");
   drain_main(200);
   g_assert_null(find_move_popover(p_win));

   g_object_unref(p_plain);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
   cleanup_temp_dir(c_dest_a);
   cleanup_temp_dir(c_dest_b);
   reset_destinations();
}

/* With no destinations configured, the helper is a safe no-op and the popup
 * shows a message pointing at Preferences instead of an empty list. */
static void
test_move_empty_no_destinations(void) {
   reset_destinations();
   GFile *p_plain = NULL;
   char  *c_dir   = setup_dir_with_three(&p_plain);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   drain_main(200);

   g_assert_false(ggaze_window_move_index(p_win, 0));

   fire(p_win, "win.move");
   drain_main(200);
   GtkPopover *p_pop = find_move_popover(p_win);
   g_assert_nonnull(p_pop);
   g_assert_cmpint(popover_button_count(p_pop), ==, 0); /* message, no rows */

   g_object_unref(p_plain);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
   reset_destinations();
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
   g_test_add_func("/move/current_no_marks", test_move_current_no_marks);
   g_test_add_func("/move/multiple_marks", test_move_multiple_marks);
   g_test_add_func("/move/collision_suffix", test_move_collision_suffix);
   g_test_add_func("/move/then_undo", test_move_then_undo);
   g_test_add_func("/move/undo_prefers_most_recent",
                   test_undo_prefers_most_recent);
   g_test_add_func("/move/undo_folder_switch_does_not_undo_stale_move",
                   test_undo_folder_switch_does_not_undo_stale_move);
   g_test_add_func("/move/undo_nothing_to_undo", test_undo_nothing_to_undo);
   g_test_add_func("/move/all_targets_collide", test_move_all_targets_collide);
   g_test_add_func("/move/destination_not_a_directory",
                   test_move_destination_not_a_directory);
   g_test_add_func("/move/popup_structure", test_move_popup_structure);
   g_test_add_func("/move/empty_no_destinations",
                   test_move_empty_no_destinations);
   return (g_test_run());
}
