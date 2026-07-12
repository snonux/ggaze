/*:*
 * ggaze — window smoke test (integration)
 *
 * Constructs a GgazeWindow offscreen and asserts the two-view stack exists
 * (children "grid" and "large", default visible = "grid"), and that
 * ggaze_window_open titles the window with the file's basename.
 *
 * The window is built with g_object_new() (no "application" property): the
 * stack/header are constructed in ggaze_window_init, independent of the app
 * association. Setting GtkWindow:application requires the GApplication
 * ::startup signal to have fired (it does in production via activate/open;
 * not in this isolated smoke test). Needs a display, so this lives in the
 * `integration` suite (CI runs it under xvfb); skipped cleanly when no
 * display is available.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "window.h"

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

static GgazeWindow *
new_window(void) {
   return (GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL)));
}

static void
test_stack_has_two_views(void) {
   GgazeWindow *p_win = new_window();
   g_assert_nonnull(p_win);

   GtkWidget *p_child = gtk_window_get_child(GTK_WINDOW(p_win));
   g_assert_nonnull(p_child);
   g_assert_true(GTK_IS_STACK(p_child));

   GtkStack   *p_stack = GTK_STACK(p_child);
   GListModel *p_pages = G_LIST_MODEL(gtk_stack_get_pages(p_stack));
   g_assert_cmpint(g_list_model_get_n_items(p_pages), ==, 2);
   g_assert_nonnull(gtk_stack_get_child_by_name(p_stack, "grid"));
   g_assert_nonnull(gtk_stack_get_child_by_name(p_stack, "large"));
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "grid");
   g_object_unref(p_pages);

   g_object_unref(p_win);
}

static void
test_open_titles_window(void) {
   GgazeWindow *p_win = new_window();

   GFile *p_file = g_file_new_for_path("/tmp/IMG_0001.jpg");
   ggaze_window_open(p_win, p_file);

   g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(p_win)), ==, "IMG_0001.jpg");

   g_object_unref(p_file);
   g_object_unref(p_win);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);

   /* Tolerate host GTK WARNINGs (e.g. an unknown gtk-modules key delivered
    * by the live GNOME session via XSettings) that g_test_init makes fatal.
    * Keep CRITICALs fatal — those are real bugs. */
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);

   if (!gtk_init_check()) {
      g_test_skip("no display available (run under xvfb)");
      return (g_test_run());
   }

   g_test_add_func("/window/stack_has_two_views", test_stack_has_two_views);
   g_test_add_func("/window/open_titles_window", test_open_titles_window);
   return (g_test_run());
}