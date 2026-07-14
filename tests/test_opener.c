/* test_opener.c — %f expansion + launch with true/false. */
#include "opener.h"
#include <gio/gio.h>
#include <glib.h>

static GFile *
make_tmp_file(void) {
   GError *e = NULL;
   char   *d = g_dir_make_tmp("ggaze-opener-XXXXXX", &e);
   g_assert_no_error(e);
   char  *p = g_build_filename(d, "test.txt", NULL);
   GFile *f = g_file_new_for_path(p);
   g_file_replace_contents(f, "x", 1, NULL, FALSE,
                           G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, NULL);
   g_free(p);
   g_free(d);
   return f;
}

static void
test_launch_true(void) {
   GFile     *f    = make_tmp_file();
   Opener    *o    = opener_new();
   OpenerProg prog = {"true", "true %f"};
   GError    *e    = NULL;
   g_assert_true(opener_launch(o, f, &prog, &e));
   g_assert_no_error(e);
   opener_delete(o);
   g_object_unref(f);
}

static void
test_launch_false(void) {
   GFile     *f    = make_tmp_file();
   Opener    *o    = opener_new();
   OpenerProg prog = {"false", "false %f"};
   GError    *e    = NULL;
   g_assert_true(opener_launch(o, f, &prog, &e));
   g_assert_no_error(e);
   opener_delete(o);
   g_object_unref(f);
}

static void
test_weird_filename(void) {
   GError *e = NULL;
   char   *d = g_dir_make_tmp("ggaze-opener-XXXXXX", &e);
   g_assert_no_error(e);
   char  *p = g_build_filename(d, "file with spaces $HOME `whoami`.jpg", NULL);
   GFile *f = g_file_new_for_path(p);
   g_file_replace_contents(f, "x", 1, NULL, FALSE,
                           G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, NULL);
   Opener    *o    = opener_new();
   OpenerProg prog = {"true", "true %f"};
   g_assert_true(opener_launch(o, f, &prog, &e));
   g_assert_no_error(e);
   opener_delete(o);
   g_object_unref(f);
   g_free(p);
   GFile           *dd = g_file_new_for_path(d);
   GFileEnumerator *en = g_file_enumerate_children(
      dd, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (en) {
      GFileInfo *i;
      while ((i = g_file_enumerator_next_file(en, NULL, NULL))) {
         GFile *c = g_file_get_child(dd, g_file_info_get_name(i));
         g_file_delete(c, NULL, NULL);
         g_object_unref(c);
         g_object_unref(i);
      }
      g_object_unref(en);
   }
   g_file_delete(dd, NULL, NULL);
   g_object_unref(dd);
   g_free(d);
}

int
main(int argc, char **argv) {
   g_test_init(&argc, &argv, NULL);
   g_test_add_func("/opener/true", test_launch_true);
   g_test_add_func("/opener/false", test_launch_false);
   g_test_add_func("/opener/weird_filename", test_weird_filename);
   return g_test_run();
}