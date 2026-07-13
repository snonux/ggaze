/*:*
 * ggaze — walk-the-folder integration test
 *
 * Drives the real window wiring: open a file (its parent folder becomes the
 * navigator), then walk prev/next/first/last and assert the viewer's texture
 * dimensions update and the header carries "n/total". Uses committed fixtures
 * copied into a temp dir; an optional ./sample-images variant skips when the
 * corpus is absent. Needs a display (integration suite; CI uses xvfb).
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
#include <string.h>

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

/* A fresh GFile handle for a file created by copy_fixture (transfer full). */
static GFile *
file_in(const char *c_dir, const char *c_name) {
   char  *c_path = g_build_filename(c_dir, c_name, NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);
   return (p_file);
}

/* Recursively remove a flat temp dir tree, then free the path string. */
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

static GgazeWindow *
new_window(void) {
   return (GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL)));
}
/* Drain in-flight async loads so their callbacks (which hold a ref on the
 * window) fire and release before the process exits. */
static void
drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

static GdkTexture *
viewer_texture(GgazeWindow *p_win) {
   GtkWidget *p_child = gtk_window_get_child(GTK_WINDOW(p_win));
   GtkWidget *p_large =
      gtk_stack_get_child_by_name(GTK_STACK(p_child), "large");
   return (ggaze_viewer_get_texture(GGAZE_VIEWER(p_large)));
}

static void
assert_dims(GgazeWindow *p_win, int i_w, int i_h) {
   /* Loads are async; pump the main loop until the viewer shows i_w x i_h. */
   GtkWidget *p_child = gtk_window_get_child(GTK_WINDOW(p_win));
   GtkWidget *p_large =
      gtk_stack_get_child_by_name(GTK_STACK(p_child), "large");
   GdkTexture *p_tex = NULL;
   for (guint u = 0; u < 3000; u++) {
      p_tex = ggaze_viewer_get_texture(GGAZE_VIEWER(p_large));
      if (p_tex != NULL && gdk_texture_get_width(p_tex) == i_w &&
          gdk_texture_get_height(p_tex) == i_h) {
         break;
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(p_tex);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, i_w);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, i_h);
}

static void
assert_title_has(GgazeWindow *p_win, const char *c_substr) {
   const gchar *c_title = gtk_window_get_title(GTK_WINDOW(p_win));
   g_assert_nonnull(c_title);
   g_assert_nonnull(g_strstr_len(c_title, -1, c_substr));
}

/* Name sort: plain.jpg (6x3), rot6.jpg (8x4 orient6 -> 4x8), small.png (5x2).
 */
static void
test_walk_temp(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-walk-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   copy_fixture(c_dir, "rot6.jpg");
   copy_fixture(c_dir, "small.png");
   GFile *p_plain = file_in(c_dir, "plain.jpg");

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain); /* current = plain.jpg */
   assert_dims(p_win, 6, 3);
   assert_title_has(p_win, "1/3");

   ggaze_window_next(p_win);
   assert_dims(p_win, 4, 8); /* rot6.jpg */
   assert_title_has(p_win, "2/3");

   ggaze_window_next(p_win);
   assert_dims(p_win, 5, 2); /* small.png */
   assert_title_has(p_win, "3/3");

   ggaze_window_prev(p_win);
   assert_dims(p_win, 4, 8); /* back to rot6.jpg */

   ggaze_window_first(p_win);
   assert_dims(p_win, 6, 3);
   ggaze_window_last(p_win);
   assert_dims(p_win, 5, 2);

   g_object_unref(p_win);
   drain_main(500);
   g_object_unref(p_plain);
   cleanup_temp_dir(c_dir);
}

static void
test_walk_sample(void) {
   const gchar *c_dir = g_getenv("GGAZE_SAMPLE_DIR");
   if (c_dir == NULL || *c_dir == '\0' ||
       !g_file_test(c_dir, G_FILE_TEST_IS_DIR)) {
      g_test_skip("GGAZE_SAMPLE_DIR unset/absent (./sample-images)");
      return;
   }
   /* Pick the first JPEG. */
   GError *p_err = NULL;
   GDir   *p_d   = g_dir_open(c_dir, 0, &p_err);
   g_assert_no_error(p_err);
   const gchar *c_name = NULL;
   while ((c_name = g_dir_read_name(p_d)) != NULL) {
      if (g_str_has_suffix(c_name, ".jpg") ||
          g_str_has_suffix(c_name, ".JPG") ||
          g_str_has_suffix(c_name, ".png")) {
         break;
      }
   }
   /* c_name is borrowed from p_d; build the path before closing the dir. */
   char *c_path =
      (c_name != NULL) ? g_build_filename(c_dir, c_name, NULL) : NULL;
   g_dir_close(p_d);
   if (c_path == NULL) {
      g_test_skip("no sample image found in corpus");
      return;
   }
   GgazeWindow *p_win  = new_window();
   GFile       *p_file = g_file_new_for_path(c_path);
   ggaze_window_open(p_win, p_file);
   g_object_unref(p_file);
   /* wait for the async load to land a texture */
   for (guint u = 0; u < 3000 && viewer_texture(p_win) == NULL; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(viewer_texture(p_win));

   /* Walk a few; each step must produce a (possibly new) texture. */
   for (int i = 0; i < 5; i++) {
      ggaze_window_next(p_win);
   }
   for (guint u = 0; u < 3000 && viewer_texture(p_win) == NULL; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(viewer_texture(p_win));

   g_object_unref(p_win);
   drain_main(500);
   g_free(c_path);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   /* Tolerate host GTK WARNINGs; keep CRITICALs fatal. */
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);

   if (!gtk_init_check()) {
      g_test_skip("no display available (run under xvfb)");
      return (g_test_run());
   }
   g_test_add_func("/walk/temp", test_walk_temp);
   g_test_add_func("/walk/sample", test_walk_sample);
   return (g_test_run());
}