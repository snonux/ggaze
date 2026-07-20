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

/* Observable injection test: a hostile filename containing shell
 * metacharacters that, if executed, would create a sentinel file. The
 * single-quoted %f must prevent the ';' from being interpreted. */
static void
test_hostile_filename(void) {
   GError *e = NULL;
   char   *d = g_dir_make_tmp("ggaze-runner-XXXXXX", &e);
   g_assert_no_error(e);

   /* Sentinel that would be created if the ';' in the name were executed. */
   char  *sentinel_p = g_build_filename(d, "PWNED", NULL);
   GFile *sentinel   = g_file_new_for_path(sentinel_p);

   /* Hostile filename: ;touch <sentinel>; — if unquoted this runs touch. */
   char  *name = g_strdup_printf(";touch %s;", sentinel_p);
   char  *fp   = g_build_filename(d, name, NULL);
   GFile *f    = g_file_new_for_path(fp);
   g_file_replace_contents(f, "x", 1, NULL, FALSE,
                           G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, NULL);
   GFile *dd = g_file_new_for_path(d);

   Runner      *r = runner_new();
   RunnerScript s = {"true", "true %f"};
   run_and_wait(r, f, dd, &s);
   g_assert_cmpint(g_exit_code, ==, 0);
   /* The sentinel must NOT exist: the ';' was quoted, not executed. */
   g_assert_false(g_file_query_exists(sentinel, NULL));

   runner_delete(r);
   g_object_unref(sentinel);
   g_object_unref(f);
   g_object_unref(dd);
   g_free(sentinel_p);
   g_free(name);
   g_free(fp);
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

/* Run a command that writes to a temp outfile, wait, then read the outfile
 * back and compare its contents to c_expected. Cleans up the outfile. */
static void
_run_and_check_output(const char *c_command, const char *c_expected) {
   GError *e = NULL;
   char   *d = g_dir_make_tmp("ggaze-runner-XXXXXX", &e);
   g_assert_no_error(e);
   char  *out_p = g_build_filename(d, "out.txt", NULL);
   GFile *dd    = g_file_new_for_path(d);

   /* Interpolate the outfile path via %s wrapped in literal single quotes,
    * e.g. `printf 'hello world\n' > '/tmp/.../out.txt'`. Temp paths from
    * g_dir_make_tmp contain no quotes, so this is safe for the fixtures. */
   char *cmd = g_strdup_printf(c_command, out_p);

   Runner      *r = runner_new();
   RunnerScript s = {"obs", cmd};
   run_and_wait(r, NULL, dd, &s);
   g_assert_cmpint(g_exit_code, ==, 0);

   char *contents = NULL;
   gsize len      = 0;
   g_assert_true(g_file_get_contents(out_p, &contents, &len, &e));
   g_assert_no_error(e);
   g_assert_cmpstr(contents, ==, c_expected);
   g_free(contents);

   g_free(cmd);
   runner_delete(r);
   g_object_unref(dd);
   g_free(out_p);
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

/* Regression: a multi-word command must reach sh -c intact. The buggy
 * g_shell_parse_argv code fed sh -c only the first word ("printf"), so the
 * outfile would never contain "hello world". */
static void
test_multi_word(void) {
   _run_and_check_output("printf 'hello world\\n' > '%s'", "hello world\n");
}

/* Pipeline: '|' must be parsed by sh, not split into argv words. */
static void
test_pipeline(void) {
   _run_and_check_output("echo pipeline | cat > '%s'", "pipeline\n");
}

/* Redirection: '>' must be interpreted by the shell. */
static void
test_redirection(void) {
   _run_and_check_output("echo redir > '%s'", "redir\n");
}

/* Multiple space-separated arguments survive intact. */
static void
test_spaces_in_args(void) {
   _run_and_check_output("printf 'a b c\\n' > '%s'", "a b c\n");
}

int
main(int argc, char **argv) {
   g_test_init(&argc, &argv, NULL);
   g_test_add_func("/runner/true_exit_zero", test_true_exit_zero);
   g_test_add_func("/runner/false_exit_nonzero", test_false_exit_nonzero);
   g_test_add_func("/runner/injection_guard", test_injection_guard);
   g_test_add_func("/runner/hostile_filename", test_hostile_filename);
   g_test_add_func("/runner/multi_word", test_multi_word);
   g_test_add_func("/runner/pipeline", test_pipeline);
   g_test_add_func("/runner/redirection", test_redirection);
   g_test_add_func("/runner/spaces_in_args", test_spaces_in_args);
   return g_test_run();
}
