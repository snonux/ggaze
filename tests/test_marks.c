/*:*
 * ggaze — marks integration test
 *
 * Exercises the mark UI wiring in the window + grid: `v` toggle, `V` range,
 * `Ctrl+a` mark-all, contextual `Esc` clear, persistence across a large/grid
 * view switch, and mark clearing on a successful trash (decision Q / du0).
 * Verifies through the public UI surface: the header title carries "N marked"
 * and the grid cells carry the "ggaze-marked" badge in sync. Needs a display
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

/* Fire a win.* action on the window. */
static void
fire(GgazeWindow *p_win, const char *c_action) {
   gtk_widget_activate_action(GTK_WIDGET(p_win), c_action, NULL);
}

/* The grid built on open (the stack's "grid" child), even when not visible. */
static GgazeGrid *
window_grid(GgazeWindow *p_win) {
   GtkStack *p_stack = ggaze_window_get_stack(p_win);
   return (GGAZE_GRID(gtk_stack_get_child_by_name(p_stack, "grid")));
}

/* Find the first GtkFlowBox descendant of p_root (the grid's flowbox lives
 * under the scrolled window, possibly via an internal GtkViewport). */
static GtkWidget *
find_flowbox(GtkWidget *p_root) {
   if (GTK_IS_FLOW_BOX(p_root)) {
      return (p_root);
   }
   GtkWidget *p_child = gtk_widget_get_first_child(p_root);
   while (p_child != NULL) {
      GtkWidget *p_found = find_flowbox(p_child);
      if (p_found != NULL) {
         return (p_found);
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (NULL);
}

/* Count grid cells currently showing the "ggaze-marked" badge. Walks the
 * GtkFlowBox (grid -> scrolled -> flowbox) and checks each cell's box. */
static guint
grid_marked_count(GgazeGrid *p_grid) {
   GtkWidget *p_flow = find_flowbox(GTK_WIDGET(p_grid));
   g_assert_nonnull(p_flow);
   guint      u_count = 0;
   GtkWidget *p_child = gtk_widget_get_first_child(p_flow);
   while (p_child != NULL) {
      GtkWidget *p_box =
         gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(p_child));
      if (p_box != NULL && gtk_widget_has_css_class(p_box, "ggaze-marked")) {
         u_count++;
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (u_count);
}

/* The mark count shown in the window title ("N marked"), or 0 if the title
 * carries no "marked" suffix (which is how 0 marks is rendered). */
static guint
title_mark_count(GgazeWindow *p_win) {
   const gchar *c_title = gtk_window_get_title(GTK_WINDOW(p_win));
   if (c_title == NULL) {
      return (0);
   }
   const char *c_p = g_strstr_len(c_title, -1, " marked");
   if (c_p == NULL) {
      return (0);
   }
   /* walk back over the digits immediately preceding " marked" */
   const char *c_num = c_p;
   while (c_num > c_title && g_ascii_isdigit(*(c_num - 1))) {
      c_num--;
   }
   if (c_num == c_p) {
      return (0);
   }
   return ((guint)g_ascii_strtoull(c_num, NULL, 10));
}

static void
assert_marks(GgazeWindow *p_win, guint u_expect) {
   drain_main(100);
   g_assert_cmpint(title_mark_count(p_win), ==, u_expect);
   g_assert_cmpint(grid_marked_count(window_grid(p_win)), ==, u_expect);
}

static char *
setup_dir_with_three(GFile **p_plain_out) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-marks-XXXXXX", &p_err);
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

static void
wait_for_load(GgazeWindow *p_win) {
   for (guint u = 0; u < 3000 && viewer_texture(p_win) == NULL; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(viewer_texture(p_win));
   drain_main(200);
}

/* --- subtests ----------------------------------------------------------- */

/* `v` toggles a mark on the current (large-view) image; the header shows
 * "1 marked" and the grid badge appears on exactly one cell. `v` again
 * untoggles it. */
static void
test_mark_toggle(void) {
   GFile *p_plain = NULL;
   char  *c_dir   = setup_dir_with_three(&p_plain);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   wait_for_load(p_win);

   g_assert_cmpint(ggaze_grid_get_count(window_grid(p_win)), ==, 3);
   assert_marks(p_win, 0);

   fire(p_win, "win.mark");
   assert_marks(p_win, 1);

   fire(p_win, "win.mark"); /* untoggle */
   assert_marks(p_win, 0);

   g_object_unref(p_plain);
   g_object_unref(p_win);
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* `Ctrl+a` marks every image; `Esc` (win.back) clears all marks contextually
 * (no fullscreen / no large->grid back-step happens while marks exist). */
static void
test_mark_all_and_clear(void) {
   GFile *p_plain = NULL;
   char  *c_dir   = setup_dir_with_three(&p_plain);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   wait_for_load(p_win);

   fire(p_win, "win.mark-all");
   assert_marks(p_win, 3);

   /* Esc clears marks first (contextual back), rather than leaving large view.
    */
   GtkStack *p_stack = ggaze_window_get_stack(p_win);
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "large");
   fire(p_win, "win.back");
   assert_marks(p_win, 0);
   /* Still in large view: Esc only cleared marks, did not back out. */
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "large");

   g_object_unref(p_plain);
   g_object_unref(p_win);
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* `V` range-marks from the last `v`-toggled anchor to the current image.
 * Mark a (anchor), advance to b, range -> 2 marked; advance to c, range
 * -> 3 marked. */
static void
test_mark_range(void) {
   GFile *p_plain = NULL;
   char  *c_dir   = setup_dir_with_three(&p_plain);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   wait_for_load(p_win);

   /* current = plain.jpg (idx 0). Mark it -> anchor. */
   fire(p_win, "win.mark");
   assert_marks(p_win, 1);

   /* advance to rot6.jpg (idx 1), range from anchor -> a, b marked. */
   fire(p_win, "win.next");
   drain_main(100);
   fire(p_win, "win.mark-range");
   assert_marks(p_win, 2);

   /* advance to small.png (idx 2), range from anchor -> a, b, c marked. */
   fire(p_win, "win.next");
   drain_main(100);
   fire(p_win, "win.mark-range");
   assert_marks(p_win, 3);

   g_object_unref(p_plain);
   g_object_unref(p_win);
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* Marks persist across a large > grid > large view switch: the grid shows
 * the same badges and the header keeps the same count. */
static void
test_marks_persist_across_view_switch(void) {
   GFile *p_plain = NULL;
   char  *c_dir   = setup_dir_with_three(&p_plain);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   wait_for_load(p_win);

   fire(p_win, "win.mark");
   fire(p_win, "win.next");
   drain_main(100);
   fire(p_win, "win.mark"); /* mark 2 images */
   assert_marks(p_win, 2);

   /* large -> grid: badges reflect the 2 marks. */
   GtkStack *p_stack = ggaze_window_get_stack(p_win);
   fire(p_win, "win.toggle-view");
   drain_main(200);
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "grid");
   g_assert_cmpint(grid_marked_count(window_grid(p_win)), ==, 2);
   g_assert_cmpint(title_mark_count(p_win), ==, 2);

   /* grid -> large: marks still present. */
   fire(p_win, "win.toggle-view");
   drain_main(200);
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "large");
   assert_marks(p_win, 2);

   g_object_unref(p_plain);
   g_object_unref(p_win);
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* Trashing the current image clears its mark (decision Q / du0): mark all 3,
 * trash the current, the mark count drops to 2 and the grid badges drop one. */
static void
test_marks_clear_on_trash(void) {
   GFile *p_plain = NULL;
   char  *c_dir   = setup_dir_with_three(&p_plain);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   wait_for_load(p_win);

   fire(p_win, "win.mark-all");
   assert_marks(p_win, 3);

   fire(p_win, "win.trash");
   drain_main(300);
   /* The trashed file's mark is cleared; 2 marks remain. */
   g_assert_cmpint(title_mark_count(p_win), ==, 2);
   g_assert_cmpint(grid_marked_count(window_grid(p_win)), ==, 2);

   /* .Trash was created with the binned file. */
   char  *c_trashpath = g_build_filename(c_dir, ".Trash", NULL);
   GFile *p_trash     = g_file_new_for_path(c_trashpath);
   g_assert_true(g_file_query_exists(p_trash, NULL));
   g_object_unref(p_trash);
   g_free(c_trashpath);

   g_object_unref(p_plain);
   g_object_unref(p_win);
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* A middle-click GtkGestureClick is attached to the grid flowbox, wired to
 * toggle the mark on the clicked cell (pointer-accessible marks). GTK 4.22
 * exposes no public API to synthesize a button event, so this is a structural
 * check that the gesture is registered with button == middle, analogous to the
 * shortcut-controller scope check in test_shortcut.c. */
static void
test_pointer_middle_click_wired(void) {
   GFile *p_plain = NULL;
   char  *c_dir   = setup_dir_with_three(&p_plain);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);
   wait_for_load(p_win);

   GgazeGrid *p_grid = window_grid(p_win);
   GtkWidget *p_flow = find_flowbox(GTK_WIDGET(p_grid));
   g_assert_nonnull(p_flow);
   GListModel *p_ctrls = gtk_widget_observe_controllers(p_flow);
   gboolean    b_found = FALSE;
   guint       u_n     = g_list_model_get_n_items(p_ctrls);
   for (guint i = 0; i < u_n; i++) {
      GObject *p_obj = g_list_model_get_item(p_ctrls, i);
      if (GTK_IS_GESTURE_CLICK(p_obj)) {
         if (gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(p_obj)) ==
             GDK_BUTTON_MIDDLE) {
            b_found = TRUE;
         }
      }
      g_object_unref(p_obj);
   }
   g_object_unref(p_ctrls);
   g_assert_true(b_found);

   g_object_unref(p_plain);
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
   g_test_add_func("/marks/toggle", test_mark_toggle);
   g_test_add_func("/marks/all_and_clear", test_mark_all_and_clear);
   g_test_add_func("/marks/range", test_mark_range);
   g_test_add_func("/marks/persist_across_view_switch",
                   test_marks_persist_across_view_switch);
   g_test_add_func("/marks/clear_on_trash", test_marks_clear_on_trash);
   g_test_add_func("/marks/pointer_middle_click_wired",
                   test_pointer_middle_click_wired);
   return (g_test_run());
}