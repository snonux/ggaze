/*:*
 * ggaze — opener unit test
 *
 * %f expansion + launch with true/false. The argv-shape regression test
 * (test_expand_weird_one_argv) calls opener_expand_command directly, which
 * opener.h exposes for exactly this purpose.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

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

/* Build a temp dir containing a file whose name has spaces, $, and backticks
 * — the same "weird" pattern the original test_weird_filename used. */
static GFile *
make_weird_file(void) {
   GError *e = NULL;
   char   *d = g_dir_make_tmp("ggaze-opener-XXXXXX", &e);
   g_assert_no_error(e);
   char  *p = g_build_filename(d, "file with spaces $HOME `whoami`.jpg", NULL);
   GFile *f = g_file_new_for_path(p);
   g_file_replace_contents(f, "x", 1, NULL, FALSE,
                           G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, NULL);
   g_free(p);
   g_free(d);
   return f;
}

/* Delete p_file and its temp parent directory (recursing any leftover
 * children so the dir removal succeeds). */
static void
cleanup_file(GFile *f) {
   if (!f)
      return;
   GFile *d = g_file_get_parent(f);
   g_file_delete(f, NULL, NULL);
   if (d) {
      GFileEnumerator *en = g_file_enumerate_children(
         d, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
      if (en) {
         GFileInfo *i;
         while ((i = g_file_enumerator_next_file(en, NULL, NULL))) {
            GFile *c = g_file_get_child(d, g_file_info_get_name(i));
            g_file_delete(c, NULL, NULL);
            g_object_unref(c);
            g_object_unref(i);
         }
         g_object_unref(en);
      }
      g_file_delete(d, NULL, NULL);
      g_object_unref(d);
   }
   g_object_unref(f);
}

static void
test_launch_true(void) {
   GFile       *f    = make_tmp_file();
   Opener      *o    = opener_new();
   SettingsPair prog = {"true", "true %f"};
   GError      *e    = NULL;
   g_assert_true(opener_launch(f, &prog, &e));
   g_assert_no_error(e);
   opener_delete(o);
   cleanup_file(f);
}

static void
test_launch_false(void) {
   GFile       *f    = make_tmp_file();
   Opener      *o    = opener_new();
   SettingsPair prog = {"false", "false %f"};
   GError      *e    = NULL;
   g_assert_true(opener_launch(f, &prog, &e));
   g_assert_no_error(e);
   opener_delete(o);
   cleanup_file(f);
}

static void
test_weird_filename(void) {
   GFile       *f    = make_weird_file();
   Opener      *o    = opener_new();
   SettingsPair prog = {"true", "true %f"};
   GError      *e    = NULL;
   g_assert_true(opener_launch(f, &prog, &e));
   g_assert_no_error(e);
   opener_delete(o);
   cleanup_file(f);
}

static void
test_quoted(void) {
   GFile       *f    = make_tmp_file();
   Opener      *o    = opener_new();
   SettingsPair prog = {"sh -c 'true'", "sh -c 'true'"};
   GError      *e    = NULL;
   g_assert_true(opener_launch(f, &prog, &e));
   g_assert_no_error(e);
   opener_delete(o);
   cleanup_file(f);
}

static void
test_quoted_with_pctf(void) {
   GFile       *f    = make_tmp_file();
   Opener      *o    = opener_new();
   SettingsPair prog = {"sh -c 'true %f'", "sh -c 'true %f'"};
   GError      *e    = NULL;
   g_assert_true(opener_launch(f, &prog, &e));
   g_assert_no_error(e);
   opener_delete(o);
   cleanup_file(f);
}

static void
test_escaped_spaces(void) {
   GFile       *f    = make_tmp_file();
   Opener      *o    = opener_new();
   SettingsPair prog = {"escaped", "true a\\ b"};
   GError      *e    = NULL;
   g_assert_true(opener_launch(f, &prog, &e));
   g_assert_no_error(e);
   opener_delete(o);
   cleanup_file(f);
}

static void
test_option_flags(void) {
   GFile       *f    = make_tmp_file();
   Opener      *o    = opener_new();
   SettingsPair prog = {"opts", "true -verbose %f"};
   GError      *e    = NULL;
   g_assert_true(opener_launch(f, &prog, &e));
   g_assert_no_error(e);
   opener_delete(o);
   cleanup_file(f);
}

static void
test_weird_filename_quoted(void) {
   GFile       *f    = make_weird_file();
   Opener      *o    = opener_new();
   SettingsPair prog = {"sh -c 'true %f'", "sh -c 'true %f'"};
   GError      *e    = NULL;
   /* %f is substituted inside the already-parsed single argv element
    * `true %f` → `true /weird/path`, passed as ONE arg to `sh -c`. */
   g_assert_true(opener_launch(f, &prog, &e));
   g_assert_no_error(e);
   opener_delete(o);
   cleanup_file(f);
}

static void
test_malformed(void) {
   GFile       *f    = make_tmp_file();
   Opener      *o    = opener_new();
   SettingsPair prog = {"bad", "sh -c 'echo hello"};
   GError      *e    = NULL;
   g_assert_false(opener_launch(f, &prog, &e));
   g_assert_nonnull(e);
   g_error_free(e);
   opener_delete(o);
   cleanup_file(f);
}

static void
test_empty(void) {
   GFile       *f    = make_tmp_file();
   Opener      *o    = opener_new();
   SettingsPair prog = {"empty", ""};
   GError      *e    = NULL;
   g_assert_false(opener_launch(f, &prog, &e));
   g_assert_nonnull(e);
   g_error_free(e);
   opener_delete(o);
   cleanup_file(f);
}

/* The key regression test: with a weird filename (spaces + shell
 * metacharacters) and an UNQUOTED %f template, the path must land in the
 * parsed argv as exactly ONE element — not split on the path's spaces. `true`
 * would ignore extra args, so a launch-only check cannot prove this; we
 * inspect the argv shape directly via opener_expand_command. */
static void
test_expand_weird_one_argv(void) {
   GFile  *f    = make_weird_file();
   GError *e    = NULL;
   char  **argv = opener_expand_command("true %f", f, &e);
   g_assert_no_error(e);
   g_assert_nonnull(argv);
   /* argv = {"true", "<weird path>", NULL} — exactly three slots. */
   g_assert_cmpstr(argv[0], ==, "true");
   g_assert_nonnull(argv[1]);
   g_assert_cmpstr(argv[2], ==, NULL);
   /* The path element must equal the file's path verbatim, spaces and all. */
   char *c_path = g_file_get_path(f);
   g_assert_cmpstr(argv[1], ==, c_path);
   g_free(c_path);
   g_strfreev(argv);
   cleanup_file(f);
}

/* A non-local GFile (no path) must be rejected with a GError rather than
 * producing an argv with an embedded NULL that silently drops trailing args. */
static void
test_expand_no_local_path(void) {
   GFile  *f    = g_file_new_for_uri("https://example.com/img.jpg");
   GError *e    = NULL;
   char  **argv = opener_expand_command("editor %f", f, &e);
   g_assert_null(argv);
   g_assert_nonnull(e);
   g_error_free(e);
   g_object_unref(f);
}

int
main(int argc, char **argv) {
   g_test_init(&argc, &argv, NULL);
   g_test_add_func("/opener/true", test_launch_true);
   g_test_add_func("/opener/false", test_launch_false);
   g_test_add_func("/opener/weird_filename", test_weird_filename);
   g_test_add_func("/opener/quoted", test_quoted);
   g_test_add_func("/opener/quoted_with_pctf", test_quoted_with_pctf);
   g_test_add_func("/opener/escaped_spaces", test_escaped_spaces);
   g_test_add_func("/opener/option_flags", test_option_flags);
   g_test_add_func("/opener/weird_filename_quoted", test_weird_filename_quoted);
   g_test_add_func("/opener/malformed", test_malformed);
   g_test_add_func("/opener/empty", test_empty);
   g_test_add_func("/opener/expand_weird_one_argv", test_expand_weird_one_argv);
   g_test_add_func("/opener/expand_no_local_path", test_expand_no_local_path);
   return g_test_run();
}