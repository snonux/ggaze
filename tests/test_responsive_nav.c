/*:*
 * ggaze — responsive navigation integration test
 *
 * Fires 10 rapid next-actions (async loads) and asserts last-write-wins: after
 * the main loop drains, the viewer shows the final current image, not a stale
 * intermediate. Also confirms the rapid navigation did not block (the loop
 * completes and the final texture arrives within a timeout). Needs a display
 * (integration suite; CI uses xvfb).
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

static gboolean
_dims_are(gpointer p_data) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   GdkTexture  *p_t   = viewer_texture(p_win);
   if (p_t == NULL) {
      return (FALSE);
   }
   gint i_w = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(p_win), "w"));
   gint i_h = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(p_win), "h"));
   return (gdk_texture_get_width(p_t) == i_w &&
           gdk_texture_get_height(p_t) == i_h);
}

static gboolean
pump_until(gboolean (*p_pred)(gpointer), gpointer p_data, guint u_ms) {
   GMainContext *p_ctx = g_main_context_default();
   for (guint u = 0; u < u_ms; u++) {
      if (p_pred != NULL && p_pred(p_data)) {
         return (TRUE);
      }
      g_main_context_iteration(p_ctx, FALSE);
      g_usleep(1000);
   }
   return (p_pred != NULL && p_pred(p_data));
}

/* Name sort: plain.jpg (6x3), rot6.jpg (8x4 orient6 -> 4x8), small.png (5x2).
 * 10 rapid nexts from idx 0 -> (0+10) % 3 = 1 -> rot6.jpg (4x8). */
static void
test_rapid_next_last_write_wins(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-resp-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   copy_fixture(c_dir, "rot6.jpg");
   copy_fixture(c_dir, "small.png");

   char  *c_plain_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_plain      = g_file_new_for_path(c_plain_path);
   g_free(c_plain_path);
   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain); /* current = plain.jpg (idx 0) */

   /* Let the first load settle so the navigator is ready. */
   g_object_set_data(G_OBJECT(p_win), "w", GINT_TO_POINTER(6));
   g_object_set_data(G_OBJECT(p_win), "h", GINT_TO_POINTER(3));
   g_assert_true(pump_until(_dims_are, p_win, 2000));

   /* Fire 10 rapid nexts; async loads must not block, and the viewer must end
    * on the final current (rot6.jpg, 4x8), not a stale intermediate. */
   for (gint i = 0; i < 10; i++) {
      ggaze_window_next(p_win);
   }
   g_object_set_data(G_OBJECT(p_win), "w", GINT_TO_POINTER(4));
   g_object_set_data(G_OBJECT(p_win), "h", GINT_TO_POINTER(8));
   g_assert_true(pump_until(_dims_are, p_win, 3000));

   g_object_unref(p_win);
   drain_main(500);
   g_object_unref(p_plain);
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
   g_test_add_func("/responsive/rapid_next_last_write_wins",
                   test_rapid_next_last_write_wins);
   return (g_test_run());
}