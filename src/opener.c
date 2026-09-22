/*:*
 * ggaze — configurable external programs (`e`)
 *
 * Holds the ordered editor list (SettingsPair: name + command template with
 * %f) read from GSettings and launches one on a file as a detached
 * GSubprocess. The template is shell-parsed FIRST and %f substituted into the
 * parsed argv afterwards, so the path is always exactly one argument whatever
 * spaces or metacharacters it contains. Plain-C, unit-testable.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "opener.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

struct Opener {
   GPtrArray *p_progs; /* SettingsPair* (owned) */
};

Opener *
opener_new(void) {
   Opener *p_o  = g_new0(Opener, 1);
   p_o->p_progs = settings_pair_array_new();
   return (p_o);
}

void
opener_delete(Opener *p_o) {
   if (p_o == NULL) {
      return;
   }
   g_ptr_array_unref(p_o->p_progs);
   g_free(p_o);
}

void
opener_set_progs(Opener *p_o, const GPtrArray *p_progs) {
   g_return_if_fail(p_o != NULL);
   g_ptr_array_unref(p_o->p_progs);
   p_o->p_progs = settings_pair_array_copy(p_progs);
}

const GPtrArray *
opener_get_progs(Opener *p_o) {
   g_return_val_if_fail(p_o != NULL, NULL);
   return (p_o->p_progs);
}

char **
opener_expand_command(const char *c_cmd, GFile *p_file, GError **p_err) {
   g_return_val_if_fail(c_cmd != NULL, NULL);
   g_return_val_if_fail(G_IS_FILE(p_file), NULL);
   char *c_path = g_file_get_path(p_file);
   if (c_path == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "opener: file has no local path");
      return (NULL);
   }
   /* Parse the raw template first: resolves quotes/escapes/options. */
   char **pp_argv = NULL;
   if (!g_shell_parse_argv(c_cmd, NULL, &pp_argv, p_err)) {
      g_free(c_path);
      return (NULL);
   }
   /* Substitute %f into each parsed element so the path is always one argv
    * value, regardless of spaces or shell metacharacters it contains. */
   for (guint u = 0; pp_argv[u] != NULL; u++) {
      if (strstr(pp_argv[u], "%f") == NULL) {
         continue;
      }
      GString *p_s = g_string_new(pp_argv[u]);
      g_string_replace(p_s, "%f", c_path, 0);
      g_free(pp_argv[u]);
      pp_argv[u] = g_string_free(p_s, FALSE);
   }
   g_free(c_path);
   return (pp_argv);
}

gboolean
opener_launch(GFile *p_file, const SettingsPair *p_prog, GError **p_err) {
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   g_return_val_if_fail(p_prog != NULL, FALSE);
   char **pp_argv = opener_expand_command(p_prog->c_value, p_file, p_err);
   if (pp_argv == NULL) {
      return (FALSE);
   }
   GSubprocess *p_sub = g_subprocess_newv((const char *const *)pp_argv,
                                          G_SUBPROCESS_FLAGS_NONE, p_err);
   g_strfreev(pp_argv);
   if (p_sub == NULL) {
      return (FALSE);
   }
   /* Detached: don't wait. Unref immediately (process keeps running). */
   g_object_unref(p_sub);
   return (TRUE);
}
