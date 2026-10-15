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
#include <glib/gstdio.h>
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
copy_fixture_as(const char *c_dir, const char *c_name,
                const char *c_dest_name) {
   const gchar *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char   *c_src = g_build_filename(c_fx, c_name, NULL);
   char   *c_dst = g_build_filename(c_dir, c_dest_name, NULL);
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

static char *
make_large_folder(guint u_count) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-gridviewport-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   for (guint u = 0; u < u_count; u++) {
      char *c_name = g_strdup_printf("image-%02u.jpg", u);
      copy_fixture_as(c_dir, "plain.jpg", c_name);
      g_free(c_name);
   }
   return (c_dir);
}

static GtkWidget *
find_picture(GtkWidget *p_root, guint u_target, guint *p_seen) {
   if (GTK_IS_PICTURE(p_root)) {
      if (*p_seen == u_target) {
         return (p_root);
      }
      (*p_seen)++;
      return (NULL);
   }
   GtkWidget *p_child = gtk_widget_get_first_child(p_root);
   while (p_child != NULL) {
      GtkWidget *p_found = find_picture(p_child, u_target, p_seen);
      if (p_found != NULL) {
         return (p_found);
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (NULL);
}

static GtkWidget *
grid_picture(GgazeGrid *p_grid, guint u_index) {
   guint u_seen = 0;
   return (find_picture(GTK_WIDGET(p_grid), u_index, &u_seen));
}

static gboolean
wait_for_paintable(GtkWidget *p_picture, guint u_timeout_ms) {
   for (guint u = 0; u < u_timeout_ms; u++) {
      if (gtk_picture_get_paintable(GTK_PICTURE(p_picture)) != NULL) {
         return (TRUE);
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   return (FALSE);
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

   gtk_window_destroy(GTK_WINDOW(p_win));
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
    * reconciles the cells in place while their requests are in flight). */
   gtk_widget_activate_action(GTK_WIDGET(p_win), "win.trash", NULL);
   drain_main(200);
   /* Undo restores the file (another structural change → refresh). */
   gtk_widget_activate_action(GTK_WIDGET(p_win), "win.undo", NULL);
   drain_main(200);

   g_object_unref(p_first);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* A GtkFlowBox maps every child, including cells below the viewport. Loading
 * on "map" therefore queues an entire large folder and leaves newly exposed
 * cells behind that backlog when the window is maximized. Only visible cells
 * should load initially; a later viewport resize must schedule the cells it
 * exposes. */
static void
test_grid_loads_newly_visible_after_resize(void) {
   const guint u_count = 8;
   char       *c_dir   = make_large_folder(u_count);
   GFile      *p_dir   = g_file_new_for_path(c_dir);
   Navigator  *p_nav   = navigator_new(p_dir, GGAZE_SORT_NAME, FALSE, FALSE);
   Thumbnail  *p_thumb = thumbnail_new();
   GgazeGrid  *p_grid  = GGAZE_GRID(ggaze_grid_new(p_nav, p_thumb, 128, FALSE));
   GtkWindow  *p_win   = GTK_WINDOW(gtk_window_new());
   gtk_window_set_child(p_win, GTK_WIDGET(p_grid));
   gtk_window_set_default_size(p_win, 300, 200);
   gtk_window_present(p_win);

   GtkWidget *p_first = grid_picture(p_grid, 0);
   GtkWidget *p_last  = grid_picture(p_grid, u_count - 1);
   g_assert_nonnull(p_first);
   g_assert_nonnull(p_last);
   g_assert_true(wait_for_paintable(p_first, 5000));
   /* A small fixture finishes quickly even in a loaded CI lane. Waiting here
    * makes this assert distinguish "never submitted" from merely "queued
    * behind the visible cells" under the old eager map-based scheduler. */
   drain_main(2000);
   g_assert_null(gtk_picture_get_paintable(GTK_PICTURE(p_last)));

   int i_initial_width  = gtk_widget_get_width(GTK_WIDGET(p_grid));
   int i_initial_height = gtk_widget_get_height(GTK_WIDGET(p_grid));
   gtk_window_set_default_size(p_win, 600, 500);
   for (guint u = 0;
        u < 3000 &&
        (gtk_widget_get_width(GTK_WIDGET(p_grid)) <= i_initial_width ||
         gtk_widget_get_height(GTK_WIDGET(p_grid)) <= i_initial_height);
        u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(p_grid)), >,
                   i_initial_width);
   g_assert_cmpint(gtk_widget_get_height(GTK_WIDGET(p_grid)), >,
                   i_initial_height);
   g_assert_true(wait_for_paintable(p_last, 5000));

   ggaze_grid_detach(p_grid);
   gtk_window_destroy(p_win);
   thumbnail_delete(p_thumb);
   navigator_delete(p_nav);
   g_object_unref(p_dir);
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* Pump until the window's grid shows u_images image cells and u_folders
 * folder cells (up to 3 s); asserts it does. A folder open is deferred (an
 * idle, then the save gate), so the grid is replaced a few turns later. */
static void
wait_for_grid(GgazeWindow *p_win, guint u_images, guint u_folders) {
   for (guint u = 0; u < 3000; u++) {
      GgazeGrid *p_grid = window_grid(p_win);
      if (p_grid != NULL && ggaze_grid_get_count(p_grid) == u_images &&
          ggaze_grid_get_folder_count(p_grid) == u_folders) {
         break;
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_cmpuint(ggaze_grid_get_count(window_grid(p_win)), ==, u_images);
   g_assert_cmpuint(ggaze_grid_get_folder_count(window_grid(p_win)), ==,
                    u_folders);
   drain_main(50);
}

/* A folder that holds only a sub-folder opens on the grid with a folder
 * cell -- not on the "No images" page -- and that cell is the way in:
 * Enter opens the sub-folder, whose grid has a ".." cell (and Backspace)
 * leading back, with the highlight back on the folder that was left. The
 * way up ends at the folder that was opened. */
static void
test_browse_sub_folders(void) {
   GError *p_err  = NULL;
   char   *c_root = g_dir_make_tmp("ggaze-browse-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   char *c_sub = g_build_filename(c_root, "sub", NULL);
   g_assert_cmpint(g_mkdir(c_sub, 0700), ==, 0);
   copy_fixture(c_sub, "plain.jpg");
   GFile *p_root = g_file_new_for_path(c_root);
   GFile *p_sub  = g_file_new_for_path(c_sub);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_root);
   wait_for_grid(p_win, 0, 1);
   GtkStack *p_stack = ggaze_window_get_stack(p_win);
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "grid");
   GFile *p_sel = ggaze_grid_get_selected_folder(window_grid(p_win));
   g_assert_nonnull(p_sel);
   g_assert_true(g_file_equal(p_sel, p_sub));

   gtk_widget_activate_action(GTK_WIDGET(p_win), "win.enter-large", NULL);
   wait_for_grid(p_win, 1, 1); /* plain.jpg and ".." */
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "grid");
   g_assert_null(ggaze_grid_get_selected_folder(window_grid(p_win)));

   /* Backspace in the large view returns to this folder's grid first;
    * only the following press climbs to the parent. */
   gtk_widget_activate_action(GTK_WIDGET(p_win), "win.enter-large", NULL);
   for (guint u = 0; u < 3000 && viewer_texture(p_win) == NULL; u++) {
      drain_main(1);
   }
   g_assert_nonnull(viewer_texture(p_win));
   drain_main(100);
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "large");
   gtk_widget_activate_action(GTK_WIDGET(p_win), "win.parent-folder", NULL);
   wait_for_grid(p_win, 1, 1);
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "grid");
   gtk_widget_activate_action(GTK_WIDGET(p_win), "win.parent-folder", NULL);
   wait_for_grid(p_win, 0, 1);
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "grid");
   p_sel = ggaze_grid_get_selected_folder(window_grid(p_win));
   g_assert_nonnull(p_sel);
   g_assert_true(g_file_equal(p_sel, p_sub));

   /* At the opened folder: nowhere further up to go. */
   gtk_widget_activate_action(GTK_WIDGET(p_win), "win.parent-folder", NULL);
   drain_main(200);
   wait_for_grid(p_win, 0, 1);

   g_object_unref(p_root);
   g_object_unref(p_sub);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_root);
   g_free(c_sub);
}

/* Thumbnails are made one page ahead of the viewport: a cell on the page
 * below the visible one is painted without any scrolling (paging down then
 * lands on pictures, not on empty cells), while the folder's far end still
 * is not decoded. */
static void
test_grid_loads_one_page_ahead(void) {
   const guint u_count = 24;
   char       *c_dir   = make_large_folder(u_count);
   GFile      *p_dir   = g_file_new_for_path(c_dir);
   Navigator  *p_nav   = navigator_new(p_dir, GGAZE_SORT_NAME, FALSE, FALSE);
   Thumbnail  *p_thumb = thumbnail_new();
   GgazeGrid  *p_grid  = GGAZE_GRID(ggaze_grid_new(p_nav, p_thumb, 128, FALSE));
   GtkWindow  *p_win   = GTK_WINDOW(gtk_window_new());
   gtk_window_set_child(p_win, GTK_WIDGET(p_grid));
   gtk_window_set_default_size(p_win, 300, 320);
   gtk_window_present(p_win);
   g_assert_true(wait_for_paintable(grid_picture(p_grid, 0), 5000));

   /* The first cell that starts below the viewport: on the page ahead. */
   float      f_view  = (float)gtk_widget_get_height(GTK_WIDGET(p_grid));
   GtkWidget *p_ahead = NULL;
   for (guint u = 0; u < u_count && p_ahead == NULL; u++) {
      graphene_rect_t r;
      GtkWidget      *p_pic = grid_picture(p_grid, u);
      g_assert_true(gtk_widget_compute_bounds(p_pic, GTK_WIDGET(p_grid), &r));
      if (r.origin.y >= f_view) {
         g_assert_cmpfloat(r.origin.y, <, 2.0f * f_view);
         p_ahead = p_pic;
      }
   }
   g_assert_nonnull(p_ahead);
   g_assert_true(wait_for_paintable(p_ahead, 5000));
   drain_main(1000);
   g_assert_null(gtk_picture_get_paintable(
      GTK_PICTURE(grid_picture(p_grid, u_count - 1))));

   ggaze_grid_detach(p_grid);
   gtk_window_destroy(p_win);
   thumbnail_delete(p_thumb);
   navigator_delete(p_nav);
   g_object_unref(p_dir);
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* The basename of navigator.current must be c_expected. */
static void
assert_current(Navigator *p_nav, const char *c_expected) {
   char *c_name = g_file_get_basename(navigator_get_current(p_nav));
   g_assert_cmpstr(c_name, ==, c_expected);
   g_free(c_name);
}

/* Page Down / Page Up move the cursor by the complete rows the viewport
 * shows (a full screenful of pictures),
 * stopping at the last / first row; Shift + a cursor key marks from where
 * the run began to the new cell, and gives marks back on the way home. */
static void
test_grid_page_and_shift_select(void) {
   const guint u_count = 12;
   char       *c_dir   = make_large_folder(u_count);
   GFile      *p_dir   = g_file_new_for_path(c_dir);
   Navigator  *p_nav   = navigator_new(p_dir, GGAZE_SORT_NAME, FALSE, FALSE);
   Thumbnail  *p_thumb = thumbnail_new();
   GgazeGrid  *p_grid  = GGAZE_GRID(ggaze_grid_new(p_nav, p_thumb, 128, FALSE));
   GtkWindow  *p_win   = GTK_WINDOW(gtk_window_new());
   gtk_window_set_child(p_win, GTK_WIDGET(p_grid));
   gtk_window_set_default_size(p_win, 300, 480);
   gtk_window_present(p_win);
   g_assert_true(wait_for_paintable(grid_picture(p_grid, 0), 5000));
   drain_main(200);

   /* The geometry the expectations below are computed from. */
   graphene_rect_t r0, r1;
   GtkWidget      *p_c0 =
      gtk_widget_get_parent(gtk_widget_get_parent(grid_picture(p_grid, 0)));
   GtkWidget *p_c1 =
      gtk_widget_get_parent(gtk_widget_get_parent(grid_picture(p_grid, 1)));
   g_assert_true(gtk_widget_compute_bounds(p_c0, GTK_WIDGET(p_grid), &r0));
   g_assert_true(gtk_widget_compute_bounds(p_c1, GTK_WIDGET(p_grid), &r1));
   guint u_cols = 1;
   while (u_cols < u_count) {
      graphene_rect_t r;
      GtkWidget      *p_c = gtk_widget_get_parent(
         gtk_widget_get_parent(grid_picture(p_grid, u_cols)));
      g_assert_true(gtk_widget_compute_bounds(p_c, GTK_WIDGET(p_grid), &r));
      if (r.origin.y > r0.origin.y) {
         break;
      }
      u_cols++;
   }
   guint u_rows =
      (guint)(gtk_widget_get_height(GTK_WIDGET(p_grid)) / r0.size.height);
   guint u_page = MAX(1u, u_rows) * u_cols;
   g_assert_cmpuint(u_page, <, u_count);

   ggaze_grid_move_page(p_grid, 1);
   drain_main(100);
   char *c_want = g_strdup_printf("image-%02u.jpg", u_page);
   assert_current(p_nav, c_want);
   g_free(c_want);
   for (guint u = 0; u < u_count; u++) {
      ggaze_grid_move_page(p_grid, 1); /* far past the end: the last row */
   }
   drain_main(100);
   g_assert_cmpint(navigator_get_current_index(p_nav), >=,
                   (gint)(u_count - u_cols));
   for (guint u = 0; u < u_count; u++) {
      ggaze_grid_move_page(p_grid, -1);
   }
   drain_main(100);
   assert_current(p_nav, "image-00.jpg");

   ggaze_grid_extend_selection(p_grid, 1, 0);
   ggaze_grid_extend_selection(p_grid, 1, 0);
   drain_main(50);
   g_assert_cmpuint(navigator_get_mark_count(p_nav), ==, 3);
   /* Marking must not resize a cell: the grid is homogeneous, so a marked
    * cell that grew (a border only marked cells had) grew every row and
    * the whole view jumped when a selection was started. */
   graphene_rect_t r0_marked;
   drain_main(100);
   g_assert_true(
      gtk_widget_compute_bounds(p_c0, GTK_WIDGET(p_grid), &r0_marked));
   g_assert_cmpfloat(r0_marked.size.width, ==, r0.size.width);
   g_assert_cmpfloat(r0_marked.size.height, ==, r0.size.height);
   assert_current(p_nav, "image-02.jpg");
   ggaze_grid_extend_selection(p_grid, -1, 0); /* back: one mark returned */
   drain_main(50);
   g_assert_cmpuint(navigator_get_mark_count(p_nav), ==, 2);
   ggaze_grid_extend_selection(p_grid, 0, 1); /* a row down: up to there */
   drain_main(50);
   g_assert_cmpuint(navigator_get_mark_count(p_nav), ==, 2 + u_cols);

   /* Released, the selection stays; the next move without Shift drops it --
    * but not a mark set outside the run. */
   GFile *p_last = navigator_get_file(p_nav, u_count - 1);
   navigator_toggle_mark(p_nav, p_last);
   drain_main(100);
   g_assert_cmpuint(navigator_get_mark_count(p_nav), ==, 3 + u_cols);
   ggaze_grid_move_cursor(p_grid, 1);
   drain_main(50);
   g_assert_cmpuint(navigator_get_mark_count(p_nav), ==, 1);
   g_assert_true(navigator_is_marked(p_nav, p_last));
   /* A new run starts where the highlight now is. */
   ggaze_grid_extend_selection(p_grid, -1, 0);
   drain_main(50);
   g_assert_cmpuint(navigator_get_mark_count(p_nav), ==, 3);

   ggaze_grid_detach(p_grid);
   gtk_window_destroy(p_win);
   thumbnail_delete(p_thumb);
   navigator_delete(p_nav);
   g_object_unref(p_dir);
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* Write a u_side x u_side noise PNG (slow to decode and scale next to the
 * small fixture) as c_dir/c_name. */
static void
write_big_png(const char *c_dir, const char *c_name, int u_side) {
   GdkPixbuf *p_pix =
      gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, u_side, u_side);
   guchar *p_px  = gdk_pixbuf_get_pixels(p_pix);
   gsize   u_len = (gsize)gdk_pixbuf_get_rowstride(p_pix) * (gsize)u_side;
   GRand  *p_rnd = g_rand_new_with_seed(7);
   for (gsize u = 0; u < u_len; u++) {
      p_px[u] = (guchar)g_rand_int_range(p_rnd, 0, 256);
   }
   g_rand_free(p_rnd);
   char   *c_path = g_build_filename(c_dir, c_name, NULL);
   GError *p_err  = NULL;
   g_assert_true(gdk_pixbuf_save(p_pix, c_path, "png", &p_err, NULL));
   g_assert_no_error(p_err);
   g_free(c_path);
   g_object_unref(p_pix);
}

/* The grid fills from the top: a thumbnail is never painted while a cell
 * before it is still waiting for its first one. The first file here decodes
 * far slower than the ones after it, so with the pool's parallel workers
 * the later results land first -- painted as they landed, the grid filled
 * in scattered order. (With a single worker the order holds trivially.) */
static void
test_grid_fills_from_the_top(void) {
   const guint u_count = 6;
   char       *c_dir   = make_large_folder(u_count);
   write_big_png(c_dir, "image-00.png", 3000); /* sorts first, decodes last */
   GFile     *p_dir   = g_file_new_for_path(c_dir);
   Navigator *p_nav   = navigator_new(p_dir, GGAZE_SORT_NAME, FALSE, FALSE);
   Thumbnail *p_thumb = thumbnail_new();
   GgazeGrid *p_grid  = GGAZE_GRID(ggaze_grid_new(p_nav, p_thumb, 128, FALSE));
   GtkWindow *p_win   = GTK_WINDOW(gtk_window_new());
   gtk_window_set_child(p_win, GTK_WIDGET(p_grid));
   gtk_window_set_default_size(p_win, 1100, 400); /* all seven visible */
   gtk_window_present(p_win);

   GtkWidget *p_pics[7];
   for (guint u = 0; u <= u_count; u++) {
      p_pics[u] = grid_picture(p_grid, u);
      g_assert_nonnull(p_pics[u]);
   }
   for (guint u_ms = 0; u_ms < 20000; u_ms++) {
      gboolean b_gap = FALSE;
      for (guint u = 0; u <= u_count; u++) {
         gboolean b_painted =
            gtk_picture_get_paintable(GTK_PICTURE(p_pics[u])) != NULL;
         g_assert_false(b_painted && b_gap); /* painted below an empty cell */
         b_gap = b_gap || !b_painted;
      }
      if (!b_gap) {
         break; /* every cell painted */
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(gtk_picture_get_paintable(GTK_PICTURE(p_pics[u_count])));

   ggaze_grid_detach(p_grid);
   gtk_window_destroy(p_win);
   thumbnail_delete(p_thumb);
   navigator_delete(p_nav);
   g_object_unref(p_dir);
   drain_main(300);
   cleanup_temp_dir(c_dir);
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
   g_test_add_func("/grid/detach_with_inflight",
                   test_grid_detach_with_inflight);
   g_test_add_func("/grid/refresh_with_inflight",
                   test_grid_refresh_with_inflight);
   g_test_add_func("/grid/loads_newly_visible_after_resize",
                   test_grid_loads_newly_visible_after_resize);
   g_test_add_func("/grid/fills_from_the_top", test_grid_fills_from_the_top);
   g_test_add_func("/grid/browse_sub_folders", test_browse_sub_folders);
   g_test_add_func("/grid/page_and_shift_select",
                   test_grid_page_and_shift_select);
   g_test_add_func("/grid/loads_one_page_ahead",
                   test_grid_loads_one_page_ahead);
   return (g_test_run());
}
