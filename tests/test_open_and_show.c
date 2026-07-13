/*:*
 * ggaze — open-and-show integration test
 *
 * Exercises the window wiring: ggaze_window_open() loads a fixture via the
 * loader, sets the viewer texture, and switches the stack to "large". Asserts
 * the stack is on "large" and the viewer holds a texture of the right size.
 *
 * An optional second test opens an image from ./sample-images (the realistic
 * local corpus, not git-tracked) and is skipped if $GGAZE_SAMPLE_DIR is unset
 * or the directory is absent — so CI (which only has tests/fixtures/) stays
 * green. Needs a display (integration suite; CI runs under xvfb).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "window.h"
#include "viewer.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

static GFile *
fixture_file(const gchar *c_name) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar *c_path = g_build_filename(c_dir, c_name, NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);
   return (p_file);
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

static void
assert_shown_large_with_dims(GgazeWindow *p_win, int i_w, int i_h) {
   GtkWidget *p_child = gtk_window_get_child(GTK_WINDOW(p_win));
   g_assert_true(GTK_IS_STACK(p_child));
   GtkWidget *p_large =
      gtk_stack_get_child_by_name(GTK_STACK(p_child), "large");
   g_assert_true(GGAZE_IS_VIEWER(p_large));
   /* Loads are async; pump until the viewer shows i_w x i_h. */
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
   g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(p_child)), ==,
                   "large");
   g_assert_nonnull(p_tex);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, i_w);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, i_h);
}

static void
test_open_fixture_shows_large(void) {
   GgazeWindow *p_win  = new_window();
   GFile       *p_file = fixture_file("plain.jpg");
   ggaze_window_open(p_win, p_file);
   assert_shown_large_with_dims(p_win, 6, 3);
   g_object_unref(p_file);
   g_object_unref(p_win);
   drain_main(500);
}

static void
test_open_rotated_fixture(void) {
   GgazeWindow *p_win  = new_window();
   GFile       *p_file = fixture_file("rot6.jpg");
   ggaze_window_open(p_win, p_file);
   assert_shown_large_with_dims(p_win, 4, 8);
   g_object_unref(p_file);
   g_object_unref(p_win);
   drain_main(500);
}

static void
test_open_sample_image(void) {
   const gchar *c_dir = g_getenv("GGAZE_SAMPLE_DIR");
   if (c_dir == NULL || *c_dir == '\0') {
      g_test_skip("GGAZE_SAMPLE_DIR unset (./sample-images not available)");
      return;
   }
   if (!g_file_test(c_dir, G_FILE_TEST_IS_DIR)) {
      g_test_skip("GGAZE_SAMPLE_DIR is not a directory");
      return;
   }
   /* Pick the first JPEG in the corpus. */
   GError *p_err = NULL;
   GDir   *p_dir = g_dir_open(c_dir, 0, &p_err);
   g_assert_no_error(p_err);
   const gchar *c_name = NULL;
   while ((c_name = g_dir_read_name(p_dir)) != NULL) {
      if (g_str_has_suffix(c_name, ".jpg") ||
          g_str_has_suffix(c_name, ".JPG") ||
          g_str_has_suffix(c_name, ".png")) {
         break;
      }
   }
   if (c_name == NULL) {
      g_test_skip("no sample image found in corpus");
      g_dir_close(p_dir);
      return;
   }
   gchar *c_path = g_build_filename(c_dir, c_name, NULL);
   g_dir_close(p_dir);

   GgazeWindow *p_win  = new_window();
   GFile       *p_file = g_file_new_for_path(c_path);
   ggaze_window_open(p_win, p_file);
   GtkWidget *p_child = gtk_window_get_child(GTK_WINDOW(p_win));
   GtkWidget *p_large =
      gtk_stack_get_child_by_name(GTK_STACK(p_child), "large");
   /* async load: pump until a texture lands */
   GdkTexture *p_tex = NULL;
   for (guint u = 0; u < 3000; u++) {
      p_tex = ggaze_viewer_get_texture(GGAZE_VIEWER(p_large));
      if (p_tex != NULL) {
         break;
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(p_child)), ==,
                   "large");
   g_assert_nonnull(p_tex); /* loaded, whatever its dims */

   g_object_unref(p_file);
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

   g_test_add_func("/open/fixture_shows_large", test_open_fixture_shows_large);
   g_test_add_func("/open/rotated_fixture", test_open_rotated_fixture);
   g_test_add_func("/open/sample_image", test_open_sample_image);
   return (g_test_run());
}