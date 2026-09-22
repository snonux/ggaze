/*:*
 * ggaze — configurable shell scripts (`!`)
 *
 * Holds the ordered script list (SettingsPair: name + shell command with %f
 * and %d) read from GSettings and runs one via /bin/sh -c as an async
 * GSubprocess, reporting the exit status through a GAsyncReadyCallback. The
 * placeholders are expanded in ONE left-to-right pass with g_shell_quote, so
 * a hostile file or folder name can never escape its quotes. Plain-C,
 * unit-testable.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "runner.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

struct Runner {
   GPtrArray *p_scripts; /* SettingsPair* (owned) */
};

Runner *
runner_new(void) {
   Runner *p_r    = g_new0(Runner, 1);
   p_r->p_scripts = settings_pair_array_new();
   return (p_r);
}

void
runner_delete(Runner *p_r) {
   if (p_r == NULL) {
      return;
   }
   g_ptr_array_unref(p_r->p_scripts);
   g_free(p_r);
}

void
runner_set_scripts(Runner *p_r, const GPtrArray *p_scripts) {
   g_return_if_fail(p_r != NULL);
   g_ptr_array_unref(p_r->p_scripts);
   p_r->p_scripts = settings_pair_array_copy(p_scripts);
}

const GPtrArray *
runner_get_scripts(Runner *p_r) {
   g_return_val_if_fail(p_r != NULL, NULL);
   return (p_r->p_scripts);
}

/* Expand %f (file path) and %d (folder path) in c_cmd, each single-quoted
 * with g_shell_quote so a hostile file name cannot break out of its argument.
 * The template is scanned ONCE, left to right, and the quoted values are
 * emitted without ever being re-scanned: a sequential replace-%f-then-%d
 * used to splice the folder path into a file name that itself contained
 * "%d", which both mangled the path and let shell metacharacters in the
 * folder name escape their quotes (command injection). "%%" yields a literal
 * "%"; any other "%x" is copied through unchanged. Caller frees. */
static char *
_expand(const char *c_cmd, GFile *p_file, GFile *p_dir) {
   char *c_fpath = p_file != NULL ? g_file_get_path(p_file) : g_strdup("");
   char *c_dpath = p_dir != NULL ? g_file_get_path(p_dir) : g_strdup("");
   char *c_fq    = g_shell_quote(c_fpath != NULL ? c_fpath : "");
   char *c_dq    = g_shell_quote(c_dpath != NULL ? c_dpath : "");
   g_free(c_fpath);
   g_free(c_dpath);

   GString *p_out = g_string_sized_new(strlen(c_cmd) + 64);
   for (const char *p = c_cmd; *p != '\0'; p++) {
      if (*p != '%') {
         g_string_append_c(p_out, *p);
         continue;
      }
      switch (p[1]) {
      case 'f':
         g_string_append(p_out, c_fq);
         p++;
         break;
      case 'd':
         g_string_append(p_out, c_dq);
         p++;
         break;
      case '%':
         g_string_append_c(p_out, '%');
         p++;
         break;
      default:
         g_string_append_c(p_out, '%');
         break;
      }
   }
   g_free(c_fq);
   g_free(c_dq);
   return (g_string_free(p_out, FALSE));
}

gboolean
runner_run(Runner *p_r, GFile *p_file, GFile *p_dir,
           const SettingsPair *p_script, GAsyncReadyCallback p_cb,
           gpointer p_data, GError **p_err) {
   g_return_val_if_fail(p_r != NULL, FALSE);
   g_return_val_if_fail(p_script != NULL, FALSE);
   char *c_cmd = _expand(p_script->c_value, p_file, p_dir);
   /* Pass the whole expanded command as a single argv element to sh -c so
    * that pipelines, redirections, && and multi-word arguments are parsed
    * by the shell as one script (not split by g_shell_parse_argv, which
    * would feed sh -c only the first word and treat the rest as $0/$1...).
    * %f/%d are already single-quoted by _expand, so paths stay safe. */
   const char  *c_argv[] = {"/bin/sh", "-c", c_cmd, NULL};
   GSubprocess *p_sub =
      g_subprocess_newv(c_argv, G_SUBPROCESS_FLAGS_NONE, p_err);
   g_free(c_cmd);
   if (p_sub == NULL) {
      return (FALSE);
   }
   if (p_cb != NULL) {
      g_subprocess_wait_check_async(p_sub, NULL, p_cb, p_data);
   }
   g_object_unref(p_sub);
   return (TRUE);
}

int
runner_run_finish(GAsyncResult *p_res, GError **p_err) {
   GSubprocess *p_sub = G_SUBPROCESS(g_async_result_get_source_object(p_res));
   if (p_sub == NULL) {
      return (-1);
   }
   gboolean b_ok  = g_subprocess_wait_check_finish(p_sub, p_res, p_err);
   int      i_ret = 0;
   if (!b_ok) {
      /* Distinguish a normal non-zero exit (G_SPAWN_EXIT_ERROR) from a
       * launch/wait failure: report the real exit status in the former so
       * the UI can show "failed (exit N)". -1 means an error (no exit code).
       * The exit status is only valid while p_sub is alive, so read it before
       * unreffing. */
      if (p_err != NULL && *p_err != NULL &&
          (*p_err)->domain == G_SPAWN_EXIT_ERROR) {
         i_ret = (int)g_subprocess_get_exit_status(p_sub);
         g_clear_error(p_err); /* not an error: it just exited non-zero */
      } else {
         i_ret = -1;
      }
   }
   g_object_unref(p_sub);
   return (i_ret);
}
