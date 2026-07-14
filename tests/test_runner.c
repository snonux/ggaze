/* test_runner.c — %f/%d expansion, injection guard, exit status. */
#include "runner.h"
#include <gio/gio.h>
#include <glib.h>

static GMainLoop *g_loop;
static int        g_exit_code;

static void
_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   (void)p_data;
   GError *e   = NULL;
   g_exit_code = runner_run_finish(NULL, p_res, &e);
   g_clear_error(&e);
   g_main_loop_quit(g_loop);
}

static void
run_and_wait(Runner *r, GFile *p_file, GFile *p_dir,
             const RunnerScript *p_script) {
   g_exit_code = -99;
   g_loop      = g_main_loop_new(NULL, FALSE);
   GError *e   = NULL;
   g_assert_true(runner_run(r, p_file, p_dir, p_script, _done_cb, NULL, &e));
   g_assert_no_error(e);
   g_main_loop_run(g_loop);
   g_main_loop_unref(g_loop);
}

static void
test_true_exit_zero(void) {
   Runner      *r = runner_new();
   RunnerScript s = {"true", "true"};
   GFile       *f = g_file_new_for_path("/tmp/nonexistent");
   run_and_wait(r, f, NULL, &s);
   g_assert_cmpint(g_exit_code, ==, 0);
   runner_delete(r);
   g_object_unref(f);
}

static void
test_false_exit_nonzero(void) {
   Runner      *r = runner_new();
   RunnerScript s = {"false", "false"};
   GFile       *f = g_file_new_for_path("/tmp/nonexistent");
   run_and_wait(r, f, NULL, &s);
   g_assert_cmpint(g_exit_code, !=, 0);
   runner_delete(r);
   g_object_unref(f);
}

static void
test_injection_guard(void) {
   GError *e = NULL;
   char   *d = g_dir_make_tmp("ggaze-runner-XXXXXX", &e);
   g_assert_no_error(e);
   /* Create a file with a malicious name. */
   char  *p = g_build_filename(d, ";echo HACKED;.jpg", NULL);
   GFile *f = g_file_new_for_path(p);
   g_file_replace_contents(f, "x", 1, NULL, FALSE,
                           G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, NULL);
   g_free(p);
   GFile *dd = g_file_new_for_path(d);

   Runner *r = runner_new();
   /* true %f should succeed regardless of the filename (injection guard).
    * The single-quoted path is a valid argument to true. */
   RunnerScript s = {"true", "true %f"};
   run_and_wait(r, f, dd, &s);
   g_assert_cmpint(g_exit_code, ==, 0);

   runner_delete(r);
   g_object_unref(f);
   g_object_unref(dd);
   GFile           *ddd = g_file_new_for_path(d);
   GFileEnumerator *en  = g_file_enumerate_children(
      ddd, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (en) {
      GFileInfo *i;
      while ((i = g_file_enumerator_next_file(en, NULL, NULL))) {
         GFile *c = g_file_get_child(ddd, g_file_info_get_name(i));
         g_file_delete(c, NULL, NULL);
         g_object_unref(c);
         g_object_unref(i);
      }
      g_object_unref(en);
   }
   g_file_delete(ddd, NULL, NULL);
   g_object_unref(ddd);
   g_free(d);
}

int
main(int argc, char **argv) {
   g_test_init(&argc, &argv, NULL);
   g_test_add_func("/runner/true_exit_zero", test_true_exit_zero);
   g_test_add_func("/runner/false_exit_nonzero", test_false_exit_nonzero);
   g_test_add_func("/runner/injection_guard", test_injection_guard);
   return g_test_run();
}