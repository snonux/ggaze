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
 * The "gd2" section covers ggaze_window_open_files: one pass for a start
 * file that is not first-sorted, and the edge cases of a first entry that
 * decides the folder -- a folder itself, a missing path, a non-image, a
 * RAW sidecar the hide-raw preference prunes. All of it runs in the minimal
 * lane too (no GEGL involved).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "temp_dir.h"
#include "viewer.h"
#include "window.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
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

/* Windows built here are torn down with gtk_window_destroy(), never a plain
 * g_object_unref(): GTK4 hands the caller's reference to its internal
 * toplevel list and only destroy() takes the entry back out (it drops that
 * reference too, so the window still finalizes). Full rationale in
 * tests/helpers/gtk_helpers.h, "window teardown". */
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

/* The large view's viewer (the "large" stack child), whichever child is
 * showing: an open lands its load there even when the grid is up. */
static GgazeViewer *
large_viewer(GgazeWindow *p_win) {
   GtkStack  *p_stack = ggaze_window_get_stack(p_win);
   GtkWidget *p_large = gtk_stack_get_child_by_name(p_stack, "large");
   g_assert_true(GGAZE_IS_VIEWER(p_large));
   return (GGAZE_VIEWER(p_large));
}

/* Loads are async: pump until the large view holds an i_w x i_h texture and
 * assert it does. Which fixture landed is told by its dimensions (plain.jpg
 * 6x3, rot6.jpg 4x8 upright, small.png 1x1). */
static void
wait_for_large_dims(GgazeWindow *p_win, int i_w, int i_h) {
   GgazeViewer *p_viewer = large_viewer(p_win);
   GdkTexture  *p_tex    = NULL;
   for (guint u = 0; u < 3000; u++) {
      p_tex = ggaze_viewer_get_texture(p_viewer);
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
assert_shown_large_with_dims(GgazeWindow *p_win, int i_w, int i_h) {
   wait_for_large_dims(p_win, i_w, i_h);
   GtkStack *p_stack = ggaze_window_get_stack(p_win);
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "large");
}

static void
test_open_fixture_shows_large(void) {
   GgazeWindow *p_win  = new_window();
   GFile       *p_file = fixture_file("plain.jpg");
   ggaze_window_open(p_win, p_file);
   assert_shown_large_with_dims(p_win, 6, 3);
   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(500);
}

static void
test_open_rotated_fixture(void) {
   GgazeWindow *p_win  = new_window();
   GFile       *p_file = fixture_file("rot6.jpg");
   ggaze_window_open(p_win, p_file);
   assert_shown_large_with_dims(p_win, 4, 8);
   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
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
   GtkWidget *p_child = GTK_WIDGET(ggaze_window_get_stack(p_win));
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
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(500);
   g_free(c_path);
}

/* --- gd2: a multi-file open is one pass ----------------------------------
 *
 * ggaze_window_open_files used to open the first file's FOLDER (a full
 * open: navigator, grid, a load of the first-sorted file) and only then
 * place the cursor on the file, which emits "changed" for any file not at
 * index 0 and ran the choke point and the load a second time -- two loads,
 * one of them cancelled. Now the start file travels with the folder, so the
 * cursor is placed before the "changed" handler exists and the load count
 * says one. The start file here is rot6.jpg, which sorts SECOND among the
 * copied fixtures (plain.jpg < rot6.jpg < small.png): the exact case that
 * used to double up. Its 4x8 decode landing (not plain.jpg's 6x3) proves
 * the one load was of the start file. */

/* A fresh folder holding copies of the three named fixtures. */
static gchar *
make_three_fixture_dir(void) {
   static const char *const c_names[] = {"plain.jpg", "rot6.jpg", "small.png"};
   GError                  *p_err     = NULL;
   gchar *c_dir = g_dir_make_tmp("ggaze-open-many-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   for (guint u = 0; u < G_N_ELEMENTS(c_names); u++) {
      GFile *p_src = fixture_file(c_names[u]);
      gchar *c_dst = g_build_filename(c_dir, c_names[u], NULL);
      GFile *p_dst = g_file_new_for_path(c_dst);
      g_assert_true(g_file_copy(p_src, p_dst, G_FILE_COPY_OVERWRITE, NULL, NULL,
                                NULL, &p_err));
      g_assert_no_error(p_err);
      g_object_unref(p_src);
      g_object_unref(p_dst);
      g_free(c_dst);
   }
   return (c_dir);
}

/* A small non-image (or a fake RAW) next to the fixtures. */
static void
write_text_file(const gchar *c_dir, const gchar *c_name) {
   GError *p_err  = NULL;
   gchar  *c_path = g_build_filename(c_dir, c_name, NULL);
   g_assert_true(g_file_set_contents(c_path, "x", 1, &p_err));
   g_assert_no_error(p_err);
   g_free(c_path);
}

static const gchar *
status_text(GgazeWindow *p_win) {
   return (gtk_label_get_text(GTK_LABEL(ggaze_window_get_info_label(p_win))));
}

/* Open {c_dir/c_first, c_dir/rot6.jpg} as a multi-file open and check the
 * shape every edge case shares: the gate ran synchronously (nothing was
 * dirty), so exactly one load was asked for by the time the call returns,
 * and the two-entry open lands in the grid. */
static GgazeWindow *
open_pair_in_grid(const gchar *c_dir, const gchar *c_first) {
   GgazeWindow *p_win = new_window();
   GFile       *pp_files[2];
   pp_files[0] = g_file_new_build_filename(c_dir, c_first, NULL);
   pp_files[1] = g_file_new_build_filename(c_dir, "rot6.jpg", NULL);
   g_assert_cmpuint(ggaze_window_load_count(p_win), ==, 0);
   ggaze_window_open_files(p_win, pp_files, 2);
   g_assert_cmpuint(ggaze_window_load_count(p_win), ==, 1);
   g_assert_cmpstr(
      gtk_stack_get_visible_child_name(ggaze_window_get_stack(p_win)), ==,
      "grid");
   g_object_unref(pp_files[0]);
   g_object_unref(pp_files[1]);
   return (p_win);
}

/* After the load landed: the navigator lists the three-fixture folder with
 * its FIRST-sorted file current -- the header says so ("plain.jpg · 1/3":
 * position 1 of 3 listed, so the folder is the fixtures' and the RAW/text
 * extra was not counted) and the large view holds plain.jpg's 6x3. */
static void
assert_on_first_of_three(GgazeWindow *p_win) {
   wait_for_large_dims(p_win, 6, 3);
   g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(p_win)), ==,
                   "plain.jpg  \u00b7  1/3");
}

static void
test_open_many_starts_on_the_given_file_once(void) {
   gchar       *c_dir = make_three_fixture_dir();
   GgazeWindow *p_win = new_window();
   GFile       *pp_files[3];
   pp_files[0] = g_file_new_build_filename(c_dir, "rot6.jpg", NULL);
   pp_files[1] = g_file_new_build_filename(c_dir, "plain.jpg", NULL);
   pp_files[2] = g_file_new_build_filename(c_dir, "small.png", NULL);
   g_assert_cmpuint(ggaze_window_load_count(p_win), ==, 0);
   ggaze_window_open_files(p_win, pp_files, 3);
   /* Nothing is dirty, so the gate ran the open synchronously: the count is
    * final here, before any main-loop iteration could deliver a "changed". */
   g_assert_cmpuint(ggaze_window_load_count(p_win), ==, 1);
   GtkStack *p_stack = ggaze_window_get_stack(p_win);
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "grid");
   /* The large view is loaded behind the grid (so `t` is instant): pump
    * until rot6.jpg's upright 4x8 decode is what it holds. */
   wait_for_large_dims(p_win, 4, 8);
   /* Nothing touched the folder (thumbnails go to the XDG cache), so no
    * rescan re-entered the load path either: still one. */
   drain_main(200);
   g_assert_cmpuint(ggaze_window_load_count(p_win), ==, 1);
   for (guint u = 0; u < G_N_ELEMENTS(pp_files); u++) {
      g_object_unref(pp_files[u]);
   }
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(500);
   ggtest_cleanup_temp_dir(c_dir);
}

/* --- gd2 review: the first entry decides the folder -----------------------
 *
 * The old two-step open (folder first, cursor afterwards) resolved the
 * first entry's PARENT and reported nothing about the entry itself. Now the
 * entry goes through _open_resolve_target / _report_open_target like a
 * single-file open, so: a folder first opens THAT folder; a missing, non-
 * image or hidden-RAW first entry opens its folder on the first-sorted
 * image and says why on the status line. The second entry (rot6.jpg, never
 * first-sorted) only asks for the grid -- it must not become current. */

static void
test_open_many_first_entry_is_a_folder(void) {
   gchar       *c_dir = make_three_fixture_dir();
   GgazeWindow *p_win = new_window();
   GFile       *pp_files[2];
   pp_files[0] = g_file_new_for_path(c_dir);
   pp_files[1] = g_file_new_build_filename(c_dir, "rot6.jpg", NULL);
   ggaze_window_open_files(p_win, pp_files, 2);
   g_assert_cmpuint(ggaze_window_load_count(p_win), ==, 1);
   g_assert_cmpstr(
      gtk_stack_get_visible_child_name(ggaze_window_get_stack(p_win)), ==,
      "grid");
   /* The folder itself, not its parent ($TMPDIR): its first image is what
    * loads, and a folder arg asked for nothing more specific, so the
    * status line stays down. */
   assert_on_first_of_three(p_win);
   g_assert_false(gtk_widget_get_visible(ggaze_window_get_info_label(p_win)));
   g_object_unref(pp_files[0]);
   g_object_unref(pp_files[1]);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(500);
   ggtest_cleanup_temp_dir(c_dir);
}

static void
test_open_many_first_entry_is_an_empty_folder(void) {
   GError *p_err = NULL;
   gchar  *c_dir = g_dir_make_tmp("ggaze-open-empty-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   GgazeWindow *p_win = new_window();
   GFile       *pp_files[2];
   pp_files[0] = g_file_new_for_path(c_dir);
   pp_files[1] = g_file_new_build_filename(c_dir, "none.jpg", NULL);
   ggaze_window_open_files(p_win, pp_files, 2);
   /* The one pass still asks for the current file (the seam counts asks,
    * not decodes); with nothing listed it clears the large view and the
    * window shows its empty state for the folder instead of the grid. */
   g_assert_cmpuint(ggaze_window_load_count(p_win), ==, 1);
   drain_main(100);
   g_assert_cmpstr(
      gtk_stack_get_visible_child_name(ggaze_window_get_stack(p_win)), ==,
      "empty");
   g_assert_null(ggaze_viewer_get_texture(large_viewer(p_win)));
   g_object_unref(pp_files[0]);
   g_object_unref(pp_files[1]);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(500);
   ggtest_cleanup_temp_dir(c_dir);
}

static void
test_open_many_first_entry_missing(void) {
   gchar       *c_dir = make_three_fixture_dir();
   GgazeWindow *p_win = open_pair_in_grid(c_dir, "missing.jpg");
   /* Reported at once (the gate ran synchronously), before the load lands. */
   g_assert_cmpstr(status_text(p_win), ==,
                   "missing.jpg not found \u2014 opened its folder");
   g_assert_true(gtk_widget_get_visible(ggaze_window_get_info_label(p_win)));
   assert_on_first_of_three(p_win);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(500);
   ggtest_cleanup_temp_dir(c_dir);
}

static void
test_open_many_first_entry_not_an_image(void) {
   gchar *c_dir = make_three_fixture_dir();
   write_text_file(c_dir, "notes.txt");
   GgazeWindow *p_win = open_pair_in_grid(c_dir, "notes.txt");
   g_assert_cmpstr(
      status_text(p_win), ==,
      "notes.txt is not an image in this folder \u2014 opened the folder");
   assert_on_first_of_three(p_win); /* 1/3: the text file is not listed */
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(500);
   ggtest_cleanup_temp_dir(c_dir);
}

/* A RAW next to a JPEG of the same stem is an image, and one the folder
 * holds -- the hide-raw preference (schema default: on) merely prunes it
 * from the listing. "is not an image in this folder" was wrong for it and
 * sent the user looking for a broken file; the status line now names the
 * preference. A RAW WITHOUT a twin is listed and needs no message, which
 * is why the check is "hide-raw on + RAW name + not current". */
static void
test_open_many_first_entry_hidden_raw_sidecar(void) {
   gchar *c_dir = make_three_fixture_dir();
   write_text_file(c_dir, "plain.cr2"); /* sidecar of plain.jpg */
   GgazeWindow *p_win = open_pair_in_grid(c_dir, "plain.cr2");
   g_assert_cmpstr(status_text(p_win), ==,
                   "plain.cr2 is a RAW sidecar hidden by Preferences "
                   "\u2014 opened the folder");
   assert_on_first_of_three(p_win); /* 1/3: the sidecar was pruned */
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(500);
   ggtest_cleanup_temp_dir(c_dir);
}

/* A RAW-NAMED dotfile (macOS AppleDouble metadata, "._plain.cr2") is
 * dropped by the listing's dotfile filter before RAW pruning ever looks at
 * it, so the RAW-sidecar message would send the user to Preferences for a
 * file no preference shows. The dotfile case is checked first and says
 * what really happened; the twin plain.jpg is there so that, without that
 * check, the RAW rule WOULD have matched. */
static void
test_open_many_first_entry_raw_named_dotfile(void) {
   gchar *c_dir = make_three_fixture_dir();
   write_text_file(c_dir, "._plain.cr2");
   GgazeWindow *p_win = open_pair_in_grid(c_dir, "._plain.cr2");
   g_assert_cmpstr(status_text(p_win), ==,
                   "._plain.cr2 is a hidden dotfile, never listed "
                   "\u2014 opened the folder");
   assert_on_first_of_three(p_win); /* 1/3: the dotfile was not listed */
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(500);
   ggtest_cleanup_temp_dir(c_dir);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   /* Tolerate host GTK WARNINGs; keep CRITICALs fatal. */
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

   g_test_add_func("/open/fixture_shows_large", test_open_fixture_shows_large);
   g_test_add_func("/open/rotated_fixture", test_open_rotated_fixture);
   g_test_add_func("/open/sample_image", test_open_sample_image);
   g_test_add_func("/open/many_starts_on_the_given_file_once",
                   test_open_many_starts_on_the_given_file_once);
   g_test_add_func("/open/many_first_entry_is_a_folder",
                   test_open_many_first_entry_is_a_folder);
   g_test_add_func("/open/many_first_entry_is_an_empty_folder",
                   test_open_many_first_entry_is_an_empty_folder);
   g_test_add_func("/open/many_first_entry_missing",
                   test_open_many_first_entry_missing);
   g_test_add_func("/open/many_first_entry_not_an_image",
                   test_open_many_first_entry_not_an_image);
   g_test_add_func("/open/many_first_entry_hidden_raw_sidecar",
                   test_open_many_first_entry_hidden_raw_sidecar);
   g_test_add_func("/open/many_first_entry_raw_named_dotfile",
                   test_open_many_first_entry_raw_named_dotfile);
   return (g_test_run());
}