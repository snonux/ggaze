/*:*
 * ggaze — grid thumbnail cancellation lifecycle test
 *
 * Proves that a grid whose thumbnail requests are in flight can be detached
 * (by re-opening another folder) and disposed without crashing or touching
 * freed picture widgets. The grid owns a GCancellable that detach/dispose
 * cancel; the thumbnail worker and the main-thread finish callback honor it,
 * so outstanding requests drop their picture refs instead of painting into
 * orphaned widgets. Needs a display (integration suite; CI uses xvfb).
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

static GgazeWindow *
new_window(void) {
   return (GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL)));
}

static GdkTexture *
viewer_texture(GgazeWindow *p_win) {
   GtkStack  *p_stack = ggaze_window_get_stack(p_win);
   GtkWidget *p_large = gtk_stack_get_child_by_name(p_stack, "large");
   return (ggaze_viewer_get_texture(GGAZE_VIEWER(p_large)));
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

/* The grid built on open (the stack's "grid" child). */
static GgazeGrid *
window_grid(GgazeWindow *p_win) {
   GtkStack *p_stack = ggaze_window_get_stack(p_win);
   return (GGAZE_GRID(gtk_stack_get_child_by_name(p_stack, "grid")));
}

/* Build a temp folder with several fixtures so the grid has multiple cells
 * that fire thumbnail requests when realized. Returns the temp dir path
 * (owned) and, if p_first_out != NULL, the first image's GFile (owned). */
static char *
make_folder(guint u_n_fixtures, GFile **p_first_out) {
   static const char *c_names[] = {"plain.jpg", "rot6.jpg", "small.png"};
   g_assert_cmpuint(u_n_fixtures, <=, G_N_ELEMENTS(c_names));
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-gridcancel-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   for (guint u = 0; u < u_n_fixtures; u++) {
      copy_fixture(c_dir, c_names[u]);
   }
   if (p_first_out != NULL) {
      char  *c_path = g_build_filename(c_dir, c_names[0], NULL);
      GFile *p_file = g_file_new_for_path(c_path);
      g_free(c_path);
      *p_first_out = p_file;
   }
   return (c_dir);
}

/* --- subtests ----------------------------------------------------------- */

/* Open as a folder (starts in the grid view, realizing cells and firing
 * thumbnail requests), then immediately re-open a different folder while the
 * first grid's requests are still in flight. The old grid is detached and
 * its cancellable cancelled; outstanding requests must drop their picture refs
 * instead of painting into freed widgets. Asserts no crash / no UAF. */
static void
test_grid_detach_with_inflight(void) {
   char *c_dir1 = make_folder(3, NULL);
   char *c_dir2 = make_folder(2, NULL);

   GFile *p_dir1 = g_file_new_for_path(c_dir1);
   GFile *p_dir2 = g_file_new_for_path(c_dir2);

   GgazeWindow *p_win = new_window();
   /* Folder arg → starts in the grid view; cells realize on the next layout
    * and fire thumbnail requests. */
   ggaze_window_open(p_win, p_dir1);
   drain_main(50);
   g_assert_cmpint(ggaze_grid_get_count(window_grid(p_win)), ==, 3);

   /* Replace the grid by re-opening another folder while the first grid's
    * thumbnail requests are still in flight (or just completed). detach +
    * dispose of the old grid must cancel its cancellable and free cleanly. */
   ggaze_window_open(p_win, p_dir2);
   drain_main(50);
   g_assert_cmpint(ggaze_grid_get_count(window_grid(p_win)), ==, 2);

   /* A second replacement back to the first folder exercises the new grid's
    * cancellable lifecycle once more. */
   ggaze_window_open(p_win, p_dir1);
   drain_main(50);
   g_assert_cmpint(ggaze_grid_get_count(window_grid(p_win)), ==, 3);

   g_object_unref(p_win);
   drain_main(300);

   g_object_unref(p_dir1);
   g_object_unref(p_dir2);
   cleanup_temp_dir(c_dir1);
   cleanup_temp_dir(c_dir2);
}

/* Toggle to the grid view (realizing cells and firing requests), then toggle
 * back to the large view and trash/undo, exercising grid refresh (which
 * rotates the cancellable) mid-flight. Asserts no crash. */
static void
test_grid_refresh_with_inflight(void) {
   GFile *p_first = NULL;
   char  *c_dir   = make_folder(3, &p_first);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_first);
   for (guint u = 0; u < 3000 && viewer_texture(p_win) == NULL; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(viewer_texture(p_win));

   /* large → grid: realizes cells, fires thumbnail requests. */
   gtk_widget_activate_action(GTK_WIDGET(p_win), "win.toggle-view", NULL);
   drain_main(100);
   GtkStack *p_stack = ggaze_window_get_stack(p_win);
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "grid");

   /* Trash the current file (structural nav change → grid refresh, which
    * cancels the in-flight requests for the old cells and rebuilds). */
   gtk_widget_activate_action(GTK_WIDGET(p_win), "win.trash", NULL);
   drain_main(200);
   /* Undo restores the file (another structural change → refresh). */
   gtk_widget_activate_action(GTK_WIDGET(p_win), "win.undo", NULL);
   drain_main(200);

   g_object_unref(p_first);
   g_object_unref(p_win);
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
   g_test_add_func("/grid/detach_with_inflight",
                   test_grid_detach_with_inflight);
   g_test_add_func("/grid/refresh_with_inflight",
                   test_grid_refresh_with_inflight);
   return (g_test_run());
}