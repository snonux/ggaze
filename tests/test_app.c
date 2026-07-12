/*:*
 * ggaze — app CLI unit test
 *
 * Exercises the ggaze binary as a subprocess (so --help/--version, which call
 * exit() inside g_application_run, do not terminate the test process itself).
 * Verifies: `--version` prints "ggaze" and exits 0; `--help` exits 0; an
 * unknown flag exits non-zero. No display is needed — all three exit before
 * activate. The ggaze binary path is passed as argv[1] by meson.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "ggaze-config.h"

#include <glib.h>
#include <gio/gio.h>

static const char *GGAZE_BIN;

/* Run GGAZE_BIN with c_arg, return the exit status (0 on success). If
 * c_out is non-NULL, receive the combined stdout+stderr. */
static gint
run_one(const char *c_arg, char **c_out) {
   GError      *p_err = NULL;
   GSubprocess *p_sub = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                            G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                         &p_err, GGAZE_BIN, c_arg, NULL);
   g_assert_no_error(p_err);

   char *c_stdout = NULL;
   g_subprocess_communicate_utf8(p_sub, NULL, NULL, &c_stdout, NULL, &p_err);
   g_assert_no_error(p_err);

   g_assert_true(g_subprocess_get_if_exited(p_sub));
   gint i_status = g_subprocess_get_exit_status(p_sub);

   if (c_out != NULL) {
      *c_out = c_stdout;
   } else {
      g_free(c_stdout);
   }
   g_object_unref(p_sub);
   return (i_status);
}

static void
test_version(void) {
   char *c_out    = NULL;
   gint  i_status = run_one("--version", &c_out);
   g_assert_cmpint(i_status, ==, 0);
   g_assert_nonnull(c_out);
   g_assert_nonnull(g_strstr_len(c_out, -1, "ggaze"));
   g_free(c_out);
}

static void
test_help(void) {
   /* GApplication provides --help itself; it prints and exits 0. */
   gint i_status = run_one("--help", NULL);
   g_assert_cmpint(i_status, ==, 0);
}

static void
test_unknown_flag(void) {
   gint i_status = run_one("--definitely-not-a-ggaze-flag", NULL);
   g_assert_cmpint(i_status, !=, 0);
}

int
main(int i_argc, char **c_argv) {
   g_assert_cmpint(i_argc, >=, 2);
   GGAZE_BIN = c_argv[1];

   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/app/version", test_version);
   g_test_add_func("/app/help", test_help);
   g_test_add_func("/app/unknown_flag", test_unknown_flag);
   return (g_test_run());
}