/*:*
 * ggaze — self-test of the shared temp-folder teardown (tests/helpers/)
 *
 * ggtest_remove_tree() runs at the end of most suites, on folders that the
 * code under test filled. It used to enumerate with the default
 * follow-symlinks flag, so a link to a directory was reported as a
 * directory and recursed into: the TARGET's contents were deleted, even
 * when the target lived outside the temp folder (gd2 review). This suite
 * pins the no-follow behaviour down, since no product suite would notice a
 * regression -- they only check that the temp folder itself is gone.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "temp_dir.h"

/* c_link -> c_target, through GIO: symlink(2) is not declared under the
 * project's strict -std=c11. */
static void
make_link(const char *c_link, const char *c_target) {
   GError *p_err  = NULL;
   GFile  *p_link = g_file_new_for_path(c_link);
   g_assert_true(g_file_make_symbolic_link(p_link, c_target, NULL, &p_err));
   g_assert_no_error(p_err);
   g_object_unref(p_link);
}

/* A fresh folder under $TMPDIR; the caller removes it. */
static gchar *
make_tmp(const char *c_template) {
   GError *p_err = NULL;
   gchar  *c_dir = g_dir_make_tmp(c_template, &p_err);
   g_assert_no_error(p_err);
   return (c_dir);
}

/* Folder A holds "link -> B" (B a sibling folder, outside A) and B holds a
 * file. Taking A down removes the link and A; B and its file survive. */
static void
test_remove_tree_does_not_follow_dir_links(void) {
   GError *p_err  = NULL;
   gchar  *c_a    = make_tmp("ggaze-tempdir-a-XXXXXX");
   gchar  *c_b    = make_tmp("ggaze-tempdir-b-XXXXXX");
   gchar  *c_file = g_build_filename(c_b, "keep.txt", NULL);
   gchar  *c_link = g_build_filename(c_a, "link", NULL);
   g_assert_true(g_file_set_contents(c_file, "x", 1, &p_err));
   g_assert_no_error(p_err);
   make_link(c_link, c_b);

   ggtest_cleanup_temp_dir(g_strdup(c_a)); /* A: the link, then A itself */

   g_assert_false(g_file_test(c_a, G_FILE_TEST_EXISTS));
   g_assert_true(g_file_test(c_b, G_FILE_TEST_IS_DIR));
   g_assert_true(g_file_test(c_file, G_FILE_TEST_IS_REGULAR));

   g_assert_cmpint(g_unlink(c_file), ==, 0);
   g_assert_cmpint(g_rmdir(c_b), ==, 0);
   g_free(c_link);
   g_free(c_file);
   g_free(c_b);
   g_free(c_a);
}

/* The ordinary case still works: nested folders and files go, depth
 * first, and a dangling link (whose target type cannot be queried when
 * following) is removed like any other link. */
static void
test_remove_tree_nested_and_dangling(void) {
   GError *p_err  = NULL;
   gchar  *c_a    = make_tmp("ggaze-tempdir-n-XXXXXX");
   gchar  *c_sub  = g_build_filename(c_a, "sub", "deeper", NULL);
   gchar  *c_file = g_build_filename(c_sub, "f.txt", NULL);
   gchar  *c_link = g_build_filename(c_a, "dangling", NULL);
   g_assert_cmpint(g_mkdir_with_parents(c_sub, 0700), ==, 0);
   g_assert_true(g_file_set_contents(c_file, "x", 1, &p_err));
   g_assert_no_error(p_err);
   make_link(c_link, "/nonexistent/ggaze-target");

   ggtest_cleanup_temp_dir(g_strdup(c_a));

   g_assert_false(g_file_test(c_a, G_FILE_TEST_EXISTS));
   g_free(c_link);
   g_free(c_file);
   g_free(c_sub);
   g_free(c_a);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/temp_dir/remove_tree_does_not_follow_dir_links",
                   test_remove_tree_does_not_follow_dir_links);
   g_test_add_func("/temp_dir/remove_tree_nested_and_dangling",
                   test_remove_tree_nested_and_dangling);
   return (g_test_run());
}
