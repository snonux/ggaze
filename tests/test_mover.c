/* test_mover.c — move + undo + collision suffixing. */
#include "mover.h"
#include <gio/gio.h>
#include <glib.h>

static char *
make_tmp_dir(void) {
   GError *e = NULL;
   char   *d = g_dir_make_tmp("ggaze-mover-XXXXXX", &e);
   g_assert_no_error(e);
   return d;
}
static GFile *
write_file(const char *d, const char *n) {
   char  *p = g_build_filename(d, n, NULL);
   GFile *f = g_file_new_for_path(p);
   g_file_replace_contents(f, "x", 1, NULL, FALSE,
                           G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, NULL);
   g_free(p);
   return f;
}
static void
cleanup_dir(char *d) {
   GFile           *dd = g_file_new_for_path(d);
   GFileEnumerator *e  = g_file_enumerate_children(
      dd, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (e) {
      GFileInfo *i;
      while ((i = g_file_enumerator_next_file(e, NULL, NULL))) {
         GFile *c = g_file_get_child(dd, g_file_info_get_name(i));
         g_file_delete(c, NULL, NULL);
         g_object_unref(c);
         g_object_unref(i);
      }
      g_object_unref(e);
   }
   g_file_delete(dd, NULL, NULL);
   g_object_unref(dd);
   g_free(d);
}

static void
test_move_and_undo(void) {
   char        *src_dir = make_tmp_dir();
   char        *dst_dir = make_tmp_dir();
   GFile       *a       = write_file(src_dir, "a.jpg");
   GFile       *b       = write_file(src_dir, "b.jpg");
   Mover       *m       = mover_new();
   SettingsPair dest    = {"dst", dst_dir};
   GList       *files   = g_list_prepend(g_list_prepend(NULL, a), b);
   GError      *e       = NULL;
   g_assert_true(mover_move(m, files, &dest, &e));
   g_assert_no_error(e);
   g_assert_false(g_file_query_exists(a, NULL));
   g_assert_false(g_file_query_exists(b, NULL));
   g_assert_true(mover_can_undo(m));
   g_assert_true(mover_undo_last(m, &e));
   g_assert_true(g_file_query_exists(a, NULL));
   g_assert_true(g_file_query_exists(b, NULL));
   g_assert_false(mover_can_undo(m));
   g_list_free(files);
   mover_delete(m);
   g_object_unref(a);
   g_object_unref(b);
   cleanup_dir(src_dir);
   cleanup_dir(dst_dir);
}

static void
test_collision(void) {
   char        *src_dir = make_tmp_dir();
   char        *dst_dir = make_tmp_dir();
   GFile       *a       = write_file(src_dir, "a.jpg");
   GFile       *p_dst_a = write_file(dst_dir, "a.jpg");
   Mover       *m       = mover_new();
   SettingsPair dest    = {"dst", dst_dir};
   GList       *files   = g_list_prepend(NULL, a);
   GError      *e       = NULL;
   g_assert_true(mover_move(m, files, &dest, &e));
   GFile *p_dd = g_file_new_for_path(dst_dir);
   GFile *p_a1 = g_file_get_child(p_dd, "a-1.jpg");
   g_assert_true(g_file_query_exists(p_a1, NULL));
   g_object_unref(p_a1);
   g_object_unref(p_dd);
   g_list_free(files);
   mover_delete(m);
   g_object_unref(p_dst_a);
   g_object_unref(a);
   cleanup_dir(src_dir);
   cleanup_dir(dst_dir);
}

/* A destination that already exists as a symlink (pointing elsewhere) must
 * be rejected -- never followed -- so no file is moved through it and the
 * symlink itself is left untouched. Mirrors test_trash.c's
 * test_bin_rejects_symlink_trash for the identical mover.c hazard. */
static void
test_move_rejects_symlink_dest(void) {
   char  *src_dir     = make_tmp_dir();
   char  *outside_dir = make_tmp_dir();
   GFile *a           = write_file(src_dir, "a.jpg");

   char   *link_path = g_build_filename(src_dir, "dst-link", NULL);
   GFile  *link_file = g_file_new_for_path(link_path);
   GError *link_err  = NULL;
   g_file_make_symbolic_link(link_file, outside_dir, NULL, &link_err);
   g_assert_no_error(link_err);
   g_object_unref(link_file);

   Mover       *m     = mover_new();
   SettingsPair dest  = {"dst", link_path};
   GList       *files = g_list_prepend(NULL, a);
   GError      *e     = NULL;
   g_assert_false(mover_move(m, files, &dest, &e));
   g_assert_error(e, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY);
   g_clear_error(&e);

   /* a.jpg must still be at its original path -- never moved through the
    * symlink -- and the outside dir must remain empty. */
   g_assert_true(g_file_query_exists(a, NULL));
   GFile *outside_a = g_file_new_build_filename(outside_dir, "a.jpg", NULL);
   g_assert_false(g_file_query_exists(outside_a, NULL));
   g_object_unref(outside_a);

   /* The symlink itself must be untouched (not followed, not deleted). */
   GFile     *check_link = g_file_new_for_path(link_path);
   GFileInfo *info =
      g_file_query_info(check_link, "standard::type",
                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
   g_assert_nonnull(info);
   g_assert_cmpint(g_file_info_get_file_type(info), ==,
                   G_FILE_TYPE_SYMBOLIC_LINK);
   g_object_unref(info);
   g_object_unref(check_link);

   g_list_free(files);
   mover_delete(m);
   g_object_unref(a);
   g_free(link_path);
   cleanup_dir(src_dir);
   cleanup_dir(outside_dir); /* untouched by mover_move -- just remove it */
}

/* A destination that already exists as a regular file (not a directory)
 * must also be rejected safely. */
static void
test_move_rejects_regular_file_dest(void) {
   char  *src_dir  = make_tmp_dir();
   GFile *a        = write_file(src_dir, "a.jpg");
   char  *dst_path = g_build_filename(src_dir, "dst-file", NULL);
   GFile *dst_file = g_file_new_for_path(dst_path);
   g_file_replace_contents(dst_file, "x", 1, NULL, FALSE,
                           G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, NULL);

   Mover       *m     = mover_new();
   SettingsPair dest  = {"dst", dst_path};
   GList       *files = g_list_prepend(NULL, a);
   GError      *e     = NULL;
   g_assert_false(mover_move(m, files, &dest, &e));
   g_assert_error(e, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY);
   g_clear_error(&e);
   g_assert_true(g_file_query_exists(a, NULL)); /* never moved */

   g_list_free(files);
   mover_delete(m);
   g_object_unref(a);
   g_object_unref(dst_file);
   g_free(dst_path);
   cleanup_dir(src_dir);
}

/* A destination that already exists as a normal, real directory must keep
 * working exactly as before (no regression). */
static void
test_move_accepts_preexisting_real_dir(void) {
   char  *src_dir = make_tmp_dir();
   char  *dst_dir = make_tmp_dir(); /* already a real dir before the move */
   GFile *a       = write_file(src_dir, "a.jpg");

   Mover       *m     = mover_new();
   SettingsPair dest  = {"dst", dst_dir};
   GList       *files = g_list_prepend(NULL, a);
   GError      *e     = NULL;
   g_assert_true(mover_move(m, files, &dest, &e));
   g_assert_no_error(e);
   g_assert_false(g_file_query_exists(a, NULL));
   g_assert_true(mover_can_undo(m));

   g_assert_true(mover_undo_last(m, &e));
   g_assert_no_error(e);
   g_assert_true(g_file_query_exists(a, NULL));
   g_assert_false(mover_can_undo(m));

   g_list_free(files);
   mover_delete(m);
   g_object_unref(a);
   cleanup_dir(src_dir);
   cleanup_dir(dst_dir);
}

int
main(int argc, char **argv) {
   g_test_init(&argc, &argv, NULL);
   g_test_add_func("/mover/move_undo", test_move_and_undo);
   g_test_add_func("/mover/collision", test_collision);
   g_test_add_func("/mover/rejects_symlink_dest",
                   test_move_rejects_symlink_dest);
   g_test_add_func("/mover/rejects_regular_file_dest",
                   test_move_rejects_regular_file_dest);
   g_test_add_func("/mover/accepts_preexisting_real_dir",
                   test_move_accepts_preexisting_real_dir);
   return g_test_run();
}