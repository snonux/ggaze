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
   char     *src_dir = make_tmp_dir();
   char     *dst_dir = make_tmp_dir();
   GFile    *a       = write_file(src_dir, "a.jpg");
   GFile    *b       = write_file(src_dir, "b.jpg");
   Mover    *m       = mover_new();
   MoverDest dest    = {"dst", dst_dir};
   GList    *files   = g_list_prepend(g_list_prepend(NULL, a), b);
   GError   *e       = NULL;
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
   char     *src_dir = make_tmp_dir();
   char     *dst_dir = make_tmp_dir();
   GFile    *a       = write_file(src_dir, "a.jpg");
   GFile    *p_dst_a = write_file(dst_dir, "a.jpg");
   Mover    *m       = mover_new();
   MoverDest dest    = {"dst", dst_dir};
   GList    *files   = g_list_prepend(NULL, a);
   GError   *e       = NULL;
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

int
main(int argc, char **argv) {
   g_test_init(&argc, &argv, NULL);
   g_test_add_func("/mover/move_undo", test_move_and_undo);
   g_test_add_func("/mover/collision", test_collision);
   return g_test_run();
}