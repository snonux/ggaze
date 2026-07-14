/*:*
 * ggaze — progressive JPEG integration test
 *
 * Opens a large sample JPEG via the window (async + progressive path) and
 * asserts the viewer shows a texture. The progressive low-res partial fires
 * first (via the loader's progress callback), then the full result replaces
 * it. Skip-if-absent (./sample-images not in CI). Needs a display.
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
test_progressive_sample(void) {
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
          g_str_has_suffix(c_name, ".JPG")) {
         break;
      }
   }
   char *c_path =
      (c_name != NULL) ? g_build_filename(c_dir, c_name, NULL) : NULL;
   g_dir_close(p_d);
   if (c_path == NULL) {
      g_test_skip("no JPEG sample found");
      return;
   }

   GgazeWindow *p_win  = new_window();
   GFile       *p_file = g_file_new_for_path(c_path);
   ggaze_window_open(p_win, p_file);
   g_object_unref(p_file);
   g_free(c_path);

   /* Pump until the viewer shows a texture (the progressive partial or the
    * full result). */
   for (guint u = 0; u < 3000 && viewer_texture(p_win) == NULL; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(viewer_texture(p_win));

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
   g_test_add_func("/progressive/sample", test_progressive_sample);
   return (g_test_run());
}