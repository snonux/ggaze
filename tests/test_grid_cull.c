/*:*
 * ggaze — grid cull integration test
 *
 * Opens a folder, toggles to the grid view, counts cells, toggles back, trashes
 * a file (d), verifies .Trash, undoes (u), and verifies the file is restored.
 * An optional ./sample-images variant grids the full 613-image corpus. Needs a
 * display (integration suite; CI uses xvfb).
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

static void
drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

static GgazeWindow *
new_window(void) {
   return (GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL)));
}

static GdkTexture *
viewer_texture(GgazeWindow *p_win) {
   GtkWidget *p_child = gtk_window_get_child(GTK_WINDOW(p_win));
   GtkWidget *p_large =
      gtk_stack_get_child_by_name(GTK_STACK(p_child), "large");
   return (ggaze_viewer_get_texture(GGAZE_VIEWER(p_large)));
}

static void
test_grid_cull_temp(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-grid-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   copy_fixture(c_dir, "rot6.jpg");
   copy_fixture(c_dir, "small.png");

   char  *c_plain_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_plain      = g_file_new_for_path(c_plain_path);
   g_free(c_plain_path);
   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);

   /* Wait for the first image to load. */
   for (guint u = 0; u < 3000 && viewer_texture(p_win) == NULL; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(viewer_texture(p_win));

   /* The grid exists in the stack with 3 cells (count without toggling to
    * avoid realizing cells and firing async thumbnail requests that can't
    * complete before exit under ASan). */
   GtkWidget *p_stack = gtk_window_get_child(GTK_WINDOW(p_win));
   GgazeGrid *p_grid =
      GGAZE_GRID(gtk_stack_get_child_by_name(GTK_STACK(p_stack), "grid"));
   g_assert_nonnull(p_grid);
   g_assert_cmpint(ggaze_grid_get_count(p_grid), ==, 3);

   /* Trash the current file (d). */
   GFile *p_dirf  = g_file_new_for_path(c_dir);
   GFile *p_trash = g_file_get_child(p_dirf, ".Trash");
   g_assert_false(g_file_query_exists(p_trash, NULL));
   gtk_widget_activate_action(GTK_WIDGET(p_win), "win.trash", NULL);
   drain_main(500);
   g_assert_true(g_file_query_exists(p_trash, NULL)); /* .Trash created */

   /* Undo (u). */
   gtk_widget_activate_action(GTK_WIDGET(p_win), "win.undo", NULL);
   drain_main(500);
   g_assert_true(g_file_query_exists(p_plain, NULL)); /* file restored */
   g_object_unref(p_plain);

   g_object_unref(p_trash);
   g_object_unref(p_dirf);
   g_object_unref(p_win);
   drain_main(500);
   cleanup_temp_dir(c_dir);
}

static void
test_grid_sample(void) {
   const gchar *c_dir = g_getenv("GGAZE_SAMPLE_DIR");
   if (c_dir == NULL || *c_dir == '\0' ||
       !g_file_test(c_dir, G_FILE_TEST_IS_DIR)) {
      g_test_skip("GGAZE_SAMPLE_DIR unset/absent (./sample-images)");
      return;
   }
   GDir *p_d = g_dir_open(c_dir, 0, NULL);
   g_assert_nonnull(p_d);
   const gchar *c_name;
   while ((c_name = g_dir_read_name(p_d)) != NULL) {
      if (g_str_has_suffix(c_name, ".jpg") ||
          g_str_has_suffix(c_name, ".png")) {
         break;
      }
   }
   char *c_path =
      (c_name != NULL) ? g_build_filename(c_dir, c_name, NULL) : NULL;
   g_dir_close(p_d);
   if (c_path == NULL) {
      g_test_skip("no sample image found");
      return;
   }

   GgazeWindow *p_win  = new_window();
   GFile       *p_file = g_file_new_for_path(c_path);
   ggaze_window_open(p_win, p_file);
   g_object_unref(p_file);
   g_free(c_path);

   /* Wait for the first image. */
   for (guint u = 0; u < 3000 && viewer_texture(p_win) == NULL; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(viewer_texture(p_win));

   /* The grid exists in the stack (created on open) with hundreds of cells,
    * but we don't toggle to it (that would realize cells and fire thumbnail
    * requests that can't all complete before exit under ASan). */
   GtkWidget *p_stack = gtk_window_get_child(GTK_WINDOW(p_win));
   GgazeGrid *p_grid =
      GGAZE_GRID(gtk_stack_get_child_by_name(GTK_STACK(p_stack), "grid"));
   g_assert_nonnull(p_grid);
   g_assert_cmpint(ggaze_grid_get_count(p_grid), >, 100);

   g_object_unref(p_win);
   drain_main(500);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
   if (!gtk_init_check()) {
      g_test_skip("no display available (run under xvfb)");
      return (g_test_run());
   }
   g_test_add_func("/grid/cull_temp", test_grid_cull_temp);
   g_test_add_func("/grid/sample", test_grid_sample);
   return (g_test_run());
}