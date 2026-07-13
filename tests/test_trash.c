/*:*
 * ggaze — trash unit test
 *
 * Exercises the ./Trash bin: bin (lazy create + collision suffix), restore
 * (undo), permanent delete, and can_undo. Uses temp dirs; no display.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "trash.h"

#include <gio/gio.h>
#include <glib.h>

static char *
make_temp_dir(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-trash-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   return (c_dir);
}

static GFile *
write_file(const char *c_dir, const char *c_name) {
   char   *c_path = g_build_filename(c_dir, c_name, NULL);
   GFile  *p_file = g_file_new_for_path(c_path);
   GError *p_err  = NULL;
   g_file_replace_contents(p_file, "x", 1, NULL, FALSE,
                           G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL,
                           &p_err);
   g_assert_no_error(p_err);
   g_free(c_path);
   return (p_file);
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
            /* recursively clean a subdir (.Trash) */
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
test_bin_and_restore(void) {
   char  *c_dir  = make_temp_dir();
   GFile *p_a    = write_file(c_dir, "a.jpg");
   GFile *p_b    = write_file(c_dir, "b.jpg");
   GFile *p_dirf = g_file_new_for_path(c_dir);

   Trash  *p_t   = trash_new(p_dirf);
   GError *p_err = NULL;

   /* Bin a.jpg → moves to .Trash/a.jpg. */
   g_assert_true(trash_bin(p_t, p_a, &p_err));
   g_assert_no_error(p_err);
   g_assert_false(g_file_query_exists(p_a, NULL)); /* gone from dir */
   g_assert_true(trash_can_undo(p_t));

   /* Restore → a.jpg back at original. */
   g_assert_true(trash_restore_last(p_t, &p_err));
   g_assert_no_error(p_err);
   g_assert_true(g_file_query_exists(p_a, NULL));
   g_assert_false(trash_can_undo(p_t));

   trash_delete(p_t);
   g_object_unref(p_a);
   g_object_unref(p_b);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

static void
test_collision_suffix(void) {
   char  *c_dir  = make_temp_dir();
   GFile *p_a1   = write_file(c_dir, "img.jpg");
   GFile *p_a2   = write_file(c_dir, "img-1.jpg");
   GFile *p_dirf = g_file_new_for_path(c_dir);

   Trash  *p_t   = trash_new(p_dirf);
   GError *p_err = NULL;

   /* Bin img.jpg → .Trash/img.jpg. */
   g_assert_true(trash_bin(p_t, p_a1, &p_err));
   g_assert_no_error(p_err);

   /* Bin img-1.jpg → .Trash/img-1.jpg (no collision yet). */
   g_assert_true(trash_bin(p_t, p_a2, &p_err));
   g_assert_no_error(p_err);

   /* Re-create img.jpg and bin it → .Trash/img-1.jpg would collide? No,
    * img.jpg was already binned. Create a new img.jpg and bin → collision
    * → .Trash/img-1.jpg (suffix). */
   GFile *p_a3 = write_file(c_dir, "img.jpg");
   g_assert_true(trash_bin(p_t, p_a3, &p_err));
   g_assert_no_error(p_err);

   /* Verify .Trash has 3 files. */
   GFile           *p_td = g_file_get_child(p_dirf, ".Trash");
   GFileEnumerator *p_e  = g_file_enumerate_children(
      p_td, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
   guint u_count = 0;
   if (p_e != NULL) {
      GFileInfo *p_info;
      while ((p_info = g_file_enumerator_next_file(p_e, NULL, NULL)) != NULL) {
         u_count++;
         g_object_unref(p_info);
      }
      g_object_unref(p_e);
   }
   g_assert_cmpint(u_count, ==, 3);
   g_object_unref(p_td);
   g_object_unref(p_a3);

   trash_delete(p_t);
   g_object_unref(p_a1);
   g_object_unref(p_a2);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

static void
test_permanent_delete(void) {
   char  *c_dir  = make_temp_dir();
   GFile *p_a    = write_file(c_dir, "a.jpg");
   GFile *p_dirf = g_file_new_for_path(c_dir);

   Trash  *p_t   = trash_new(p_dirf);
   GError *p_err = NULL;

   g_assert_true(trash_permanently_delete(p_t, p_a, &p_err));
   g_assert_no_error(p_err);
   g_assert_false(g_file_query_exists(p_a, NULL));
   g_assert_false(trash_can_undo(p_t)); /* permanent delete is not undoable */

   trash_delete(p_t);
   g_object_unref(p_a);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/trash/bin_and_restore", test_bin_and_restore);
   g_test_add_func("/trash/collision_suffix", test_collision_suffix);
   g_test_add_func("/trash/permanent_delete", test_permanent_delete);
   return (g_test_run());
}