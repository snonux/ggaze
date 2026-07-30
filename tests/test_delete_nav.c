/*:*
 * ggaze — delete-then-navigate integration test (fu0)
 *
 * Regression for fu0: after a bulk permanent delete of adjacent marked files,
 * _do_delete_files marked every target removed then called navigator_next
 * only once, and navigator _advance did not skip removed entries. With
 * adjacent marked files the cursor could land on another deleted path, the
 * load failed, stale pixels stayed displayed, and actions targeted a
 * nonexistent file (violating the last-write-wins invariant).
 *
 * The fix makes navigator_next/prev/first/last skip removed (dimmed) entries
 * and park the cursor at -1 when no live entry remains. This test drives the
 * real _do_delete_files path via ggaze_window_delete_captured (the exact code
 * the confirm-dialog callback runs) and asserts, through the public UI
 * surface, that the viewer lands on a LIVE file (its filename still exists on
 * disk) and that the grid still lists the dimmed entries. Needs a display
 * (integration suite; CI uses xvfb).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "gridview.h"
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

/* The basename shown in the window title (the current file), or NULL when the
 * title carries no filename (no current cursor — all entries removed). The
 * title is "<basename>  \u00b7  i/remaining [\u00b7  N marked]"; the basename
 * precedes the first middle dot. */
static char *
title_basename(GgazeWindow *p_win) {
   const gchar *c_title = gtk_window_get_title(GTK_WINDOW(p_win));
   if (c_title == NULL) {
      return (NULL);
   }
   const char *c_dot = g_strstr_len(c_title, -1, "\u00b7");
   if (c_dot == NULL) {
      return (g_strdup(c_title));
   }
   char *c_base = g_strndup(c_title, (gsize)(c_dot - c_title));
   g_strstrip(c_base);
   if (c_base[0] == '\0') {
      g_free(c_base);
      return (NULL);
   }
   return (c_base);
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

/* Bulk-delete a contiguous block of adjacent marked files while the cursor
 * sits on one of them: a single navigator_next must skip the adjacent removed
 * entry and land on a LIVE file (one that still exists on disk), not on the
 * removed neighbour. The grid still lists every entry (the removed ones
 * dimmed). This is the fu0 data-loss/stale-pixels scenario. */
static void
test_bulk_delete_lands_on_live(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-delnav-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   /* Sorted by name: plain.jpg, rot6.jpg, rgba.png, small.png (idx 0..3). */
   copy_fixture(c_dir, "plain.jpg");
   copy_fixture(c_dir, "rot6.jpg");
   copy_fixture(c_dir, "rgba.png");
   copy_fixture(c_dir, "small.png");

   char        *c_p0  = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile       *p_f0  = g_file_new_for_path(c_p0);
   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_f0);
   wait_for_load(p_win); /* current = plain.jpg (idx 0) */

   /* Sorted by name: plain.jpg, rgba.png, rot6.jpg, small.png (idx 0..3).
    * Move the cursor onto rgba.png (idx 1) — the first of the adjacent block
    * to be deleted. */
   fire(p_win, "win.next");
   drain_main(100);
   char *c_before = title_basename(p_win);
   g_assert_cmpstr(c_before, ==, "rgba.png");
   g_free(c_before);

   /* Bulk-delete the adjacent block rgba + rot6 via the captured path (the
    * exact code the confirm-dialog callback runs). The cursor is on rgba
    * (now removed); a single navigator_next must skip rot6 (also removed) and
    * land on small.png (live). */
   GFile            *p_dir    = g_file_new_for_path(c_dir);
   const char *const c_del[]  = {"rgba.png", "rot6.jpg"};
   const char *const c_live[] = {"plain.jpg", "small.png"};
   GList *p_captured = build_file_list(c_dir, c_del, G_N_ELEMENTS(c_del));
   g_assert_true(ggaze_window_delete_targets_still_current(p_win, p_dir));
   gboolean b_proceeded =
      ggaze_window_delete_captured(p_win, p_dir, p_captured);
   g_assert_true(b_proceeded);
   g_list_free_full(p_captured, (GDestroyNotify)g_object_unref);

   /* Wait for the cursor to settle on a live file (the title transiently
    * shows the removed file while the delete loop emits "changed", then the
    * final navigator_next skips to the live target). */
   for (guint u = 0; u < 3000; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
      char *c_now = title_basename(p_win);
      if (c_now != NULL && (g_strcmp0(c_now, "small.png") == 0 ||
                            g_strcmp0(c_now, "plain.jpg") == 0)) {
         g_free(c_now);
         break;
      }
      g_free(c_now);
   }
   drain_main(300);

   /* The cursor landed on a LIVE file: its basename still exists on disk. */
   char *c_now = title_basename(p_win);
   g_assert_nonnull(c_now);
   char *c_nowpath = g_build_filename(c_dir, c_now, NULL);
   g_assert_true(path_exists(c_nowpath));
   g_free(c_nowpath);
   /* Specifically small.png (the live skip target after the removed block),
    * never the removed rot6.jpg. */
   g_assert_cmpstr(c_now, ==, "small.png");
   g_free(c_now);
   g_assert_nonnull(viewer_texture(p_win));

   /* The deleted files are gone from disk; the live ones remain. */
   for (guint i = 0; i < G_N_ELEMENTS(c_del); i++) {
      char *c_p = g_build_filename(c_dir, c_del[i], NULL);
      g_assert_false(path_exists(c_p));
      g_free(c_p);
   }
   for (guint i = 0; i < G_N_ELEMENTS(c_live); i++) {
      char *c_p = g_build_filename(c_dir, c_live[i], NULL);
      g_assert_true(path_exists(c_p));
      g_free(c_p);
   }
   /* The grid still lists all four entries (the deleted ones dimmed). */
   g_assert_cmpint(ggaze_grid_get_count(window_grid(p_win)), ==, 4);

   g_object_unref(p_dir);
   g_object_unref(p_f0);
   g_free(c_p0);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* Bulk-delete every image: no live entry remains, so the cursor parks at -1,
 * the viewer clears (no stale pixels), and the grid still lists every entry
 * dimmed. */
static void
test_bulk_delete_all_clears_viewer(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-delnav-all-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   copy_fixture(c_dir, "rot6.jpg");
   copy_fixture(c_dir, "small.png");

   char        *c_p0  = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile       *p_f0  = g_file_new_for_path(c_p0);
   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_f0);
   wait_for_load(p_win);

   GFile            *p_dir   = g_file_new_for_path(c_dir);
   const char *const c_all[] = {"plain.jpg", "rot6.jpg", "small.png"};
   GList *p_captured = build_file_list(c_dir, c_all, G_N_ELEMENTS(c_all));
   g_assert_true(ggaze_window_delete_captured(p_win, p_dir, p_captured));
   g_list_free_full(p_captured, (GDestroyNotify)g_object_unref);
   drain_main(500);

   /* No current cursor: the title falls back to "ggaze" (no filename) and
    * the viewer cleared (no stale pixels for a nonexistent file). */
   g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(p_win)), ==, "ggaze");
   g_assert_null(viewer_texture(p_win));
   /* The grid still lists all three entries (dimmed). */
   g_assert_cmpint(ggaze_grid_get_count(window_grid(p_win)), ==, 3);

   g_object_unref(p_dir);
   g_object_unref(p_f0);
   g_free(c_p0);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
   if (!gtk_init_check()) {
      g_test_skip("no display available (run under xvfb)");
      return (g_test_run());
   }
   g_test_add_func("/delete_nav/bulk_delete_lands_on_live",
                   test_bulk_delete_lands_on_live);
   g_test_add_func("/delete_nav/bulk_delete_all_clears_viewer",
                   test_bulk_delete_all_clears_viewer);
   return (g_test_run());
}