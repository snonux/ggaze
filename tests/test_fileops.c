/*:*
 * ggaze — fileops unit test (no display)
 *
 * Drives the trash / delete / move / undo policy over a real temp folder
 * with a fake report sink: targets are the marks else the current file; a
 * trash advances only when the victim was current and reports with the
 * undo hint; a vanished target is refused out loud; delete reports; a move
 * dims what moved, records the undo and reports partial failures; undo
 * picks the most recent engine and says "Nothing to undo" otherwise.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "fileops.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "mover.h"
#include "navigator.h"
#include "trash.h"
#include "undo.h"

/* --- fixture ------------------------------------------------------------- */

typedef struct {
   char      *c_dir;
   char      *c_dest;
   GFile     *p_dir;
   Navigator *p_nav;
   Trash     *p_trash;
   Mover     *p_mover;
   Undo      *p_undo;
   FileOps   *p_fo;
   char      *c_last; /* last reported status */
} Fx;

static void
_report(gpointer p_host, const char *c_msg) {
   Fx *p_fx = (Fx *)p_host;
   g_free(p_fx->c_last);
   p_fx->c_last = g_strdup(c_msg);
}

static void
touch(const char *c_dir, const char *c_name) {
   char *c_path = g_build_filename(c_dir, c_name, NULL);
   g_assert_true(g_file_set_contents(c_path, "x", 1, NULL));
   g_free(c_path);
}

static gboolean
exists(const char *c_dir, const char *c_name) {
   char    *c_path = g_build_filename(c_dir, c_name, NULL);
   gboolean b      = g_file_test(c_path, G_FILE_TEST_EXISTS);
   g_free(c_path);
   return (b);
}

static void
rm_rf(const char *c_dir) {
   GDir *p_d = g_dir_open(c_dir, 0, NULL);
   if (p_d != NULL) {
      const char *c_n;
      while ((c_n = g_dir_read_name(p_d)) != NULL) {
         char *c_p = g_build_filename(c_dir, c_n, NULL);
         if (g_file_test(c_p, G_FILE_TEST_IS_DIR)) {
            rm_rf(c_p);
         } else {
            g_unlink(c_p);
         }
         g_free(c_p);
      }
      g_dir_close(p_d);
   }
   g_rmdir(c_dir);
}

/* a.jpg b.jpg c.jpg; current = a.jpg; one move destination. */
static void
fx_setup(Fx *p_fx) {
   p_fx->c_dir  = g_dir_make_tmp("ggaze-fileops-XXXXXX", NULL);
   p_fx->c_dest = g_dir_make_tmp("ggaze-fileops-dest-XXXXXX", NULL);
   touch(p_fx->c_dir, "a.jpg");
   touch(p_fx->c_dir, "b.jpg");
   touch(p_fx->c_dir, "c.jpg");
   p_fx->p_dir   = g_file_new_for_path(p_fx->c_dir);
   p_fx->p_nav   = navigator_new(p_fx->p_dir, GGAZE_SORT_NAME, FALSE, TRUE);
   p_fx->p_trash = trash_new(p_fx->p_dir);
   p_fx->p_mover = mover_new();
   p_fx->p_undo  = undo_new();
   GPtrArray *p_dests = settings_pair_array_new();
   g_ptr_array_add(p_dests, settings_pair_new("keep", p_fx->c_dest));
   mover_set_dests(p_fx->p_mover, p_dests);
   g_ptr_array_unref(p_dests);
   p_fx->p_fo = fileops_new(p_fx->p_mover, p_fx->p_undo, _report, p_fx);
   fileops_set_folder(p_fx->p_fo, p_fx->p_nav, p_fx->p_trash);
}

static void
fx_teardown(Fx *p_fx) {
   fileops_delete(p_fx->p_fo);
   undo_delete(p_fx->p_undo);
   mover_delete(p_fx->p_mover);
   trash_delete(p_fx->p_trash);
   navigator_delete(p_fx->p_nav);
   g_object_unref(p_fx->p_dir);
   rm_rf(p_fx->c_dir);
   rm_rf(p_fx->c_dest);
   g_free(p_fx->c_dir);
   g_free(p_fx->c_dest);
   g_free(p_fx->c_last);
}

static GFile *
nav_file(Fx *p_fx, guint u_idx) {
   return (navigator_get_file(p_fx->p_nav, u_idx));
}

/* --- tests --------------------------------------------------------------- */

static void
test_capture_targets(void) {
   Fx fx = {0};
   fx_setup(&fx);
   GList *p_t = fileops_capture_targets(fx.p_fo);
   g_assert_cmpuint(g_list_length(p_t), ==, 1);
   g_assert_true(g_file_equal(G_FILE(p_t->data), nav_file(&fx, 0)));
   g_list_free_full(p_t, g_object_unref);
   navigator_toggle_mark(fx.p_nav, nav_file(&fx, 1));
   navigator_toggle_mark(fx.p_nav, nav_file(&fx, 2));
   p_t = fileops_capture_targets(fx.p_fo);
   g_assert_cmpuint(g_list_length(p_t), ==, 2);
   g_assert_true(fileops_files_include_current(fx.p_fo, p_t) == FALSE);
   g_list_free_full(p_t, g_object_unref);
   fx_teardown(&fx);
}

static void
test_trash_advances_and_reports(void) {
   Fx fx = {0};
   fx_setup(&fx);
   GFile *p_a = g_object_ref(nav_file(&fx, 0));
   fileops_trash(fx.p_fo, p_a);
   g_assert_false(exists(fx.c_dir, "a.jpg"));
   g_assert_true(navigator_is_removed(fx.p_nav, p_a));
   g_assert_cmpint(navigator_get_current_index(fx.p_nav), ==, 1);
   g_assert_cmpstr(fx.c_last, ==, "Trashed a.jpg — u to undo");

   /* Trashing a non-current file does not move the cursor. */
   GFile *p_c = g_object_ref(nav_file(&fx, 2));
   fileops_trash(fx.p_fo, p_c);
   g_assert_cmpint(navigator_get_current_index(fx.p_nav), ==, 1);

   /* A target that vanished behind a prompt is refused out loud. */
   fileops_trash(fx.p_fo, p_a);
   g_assert_cmpstr(fx.c_last, ==, "Nothing trashed — the file is gone");

   fileops_undo(fx.p_fo); /* restores c.jpg (most recent) */
   g_assert_true(exists(fx.c_dir, "c.jpg"));
   g_assert_cmpstr(fx.c_last, ==, "Restored from Trash");
   g_object_unref(p_a);
   g_object_unref(p_c);
   fx_teardown(&fx);
}

static void
test_delete_reports(void) {
   Fx fx = {0};
   fx_setup(&fx);
   GList *p_t = fileops_capture_targets(fx.p_fo); /* a.jpg (current) */
   fileops_delete_files(fx.p_fo, p_t);
   g_assert_false(exists(fx.c_dir, "a.jpg"));
   g_assert_cmpstr(fx.c_last, ==, "Deleted a.jpg permanently (no undo)");
   g_assert_cmpint(navigator_get_current_index(fx.p_nav), ==, 1);
   /* Deleting it again fails and says so. */
   fileops_delete_files(fx.p_fo, p_t);
   g_assert_cmpstr(fx.c_last, ==, "Delete failed for 1 file");
   g_list_free_full(p_t, g_object_unref);
   fileops_undo(fx.p_fo);
   g_assert_cmpstr(fx.c_last, ==, "Nothing to undo");
   fx_teardown(&fx);
}

static void
test_move_and_undo(void) {
   Fx fx = {0};
   fx_setup(&fx);
   navigator_toggle_mark(fx.p_nav, nav_file(&fx, 0));
   navigator_toggle_mark(fx.p_nav, nav_file(&fx, 1));
   GList *p_t = fileops_capture_targets(fx.p_fo);
   g_assert_true(fileops_move(fx.p_fo, 0, p_t));
   g_assert_true(exists(fx.c_dest, "a.jpg"));
   g_assert_true(exists(fx.c_dest, "b.jpg"));
   g_assert_cmpstr(fx.c_last, ==, "Moved 2 files to keep");
   g_assert_cmpint(navigator_get_current_index(fx.p_nav), ==, 2);
   g_assert_false(fileops_move(fx.p_fo, 5, p_t)); /* bad destination */
   g_list_free_full(p_t, g_object_unref);
   fileops_undo(fx.p_fo);
   g_assert_cmpstr(fx.c_last, ==, "Move undone");
   g_assert_true(exists(fx.c_dir, "a.jpg"));
   g_assert_false(exists(fx.c_dest, "a.jpg"));
   fx_teardown(&fx);
}

static void
test_no_folder_is_noop(void) {
   Fx fx = {0};
   fx_setup(&fx);
   fileops_set_folder(fx.p_fo, NULL, NULL);
   g_assert_null(fileops_capture_targets(fx.p_fo));
   g_assert_false(fileops_target_still_in_folder(fx.p_fo, nav_file(&fx, 0)));
   fileops_trash(fx.p_fo, nav_file(&fx, 0));
   g_assert_true(exists(fx.c_dir, "a.jpg"));
   fileops_undo(fx.p_fo);
   g_assert_null(fx.c_last);
   fx_teardown(&fx);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/fileops/capture_targets", test_capture_targets);
   g_test_add_func("/fileops/trash_advances_and_reports",
                   test_trash_advances_and_reports);
   g_test_add_func("/fileops/delete_reports", test_delete_reports);
   g_test_add_func("/fileops/move_and_undo", test_move_and_undo);
   g_test_add_func("/fileops/no_folder_is_noop", test_no_folder_is_noop);
   return (g_test_run());
}
