/*:*
 * ggaze — shared path helpers
 *
 * See pathutil.h: stem/extension split, symlink-safe directory checks and
 * the one non-colliding child-name rule trash, move and enhance-save share.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "pathutil.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

void
pathutil_split_ext(const char *c_base, char **pc_stem, const char **pc_ext) {
   g_return_if_fail(c_base != NULL);
   g_return_if_fail(pc_stem != NULL);
   g_return_if_fail(pc_ext != NULL);
   /* A leading dot is a hidden-file marker, not an extension separator. */
   const char *c_dot = strrchr(c_base, '.');
   if (c_dot == NULL || c_dot == c_base) {
      *pc_stem = g_strdup(c_base);
      *pc_ext  = "";
      return;
   }
   *pc_stem = g_strndup(c_base, (gsize)(c_dot - c_base));
   *pc_ext  = c_dot;
}

gboolean
pathutil_dir_is_safe(GFile *p_dir, GError **p_err) {
   g_return_val_if_fail(G_IS_FILE(p_dir), FALSE);
   GFileInfo *p_info =
      g_file_query_info(p_dir, "standard::type",
                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, p_err);
   if (p_info == NULL) {
      return (FALSE);
   }
   GFileType e_type = g_file_info_get_file_type(p_info);
   g_object_unref(p_info);
   if (e_type != G_FILE_TYPE_DIRECTORY) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                  "refusing to use an existing path that is not a real "
                  "directory (found a symlink or other non-directory)");
      return (FALSE);
   }
   return (TRUE);
}

gboolean
pathutil_ensure_dir(GFile *p_dir, GError **p_err) {
   g_return_val_if_fail(G_IS_FILE(p_dir), FALSE);
   gboolean b_ok = g_file_make_directory_with_parents(p_dir, NULL, p_err);
   if (!b_ok && p_err != NULL &&
       g_error_matches(*p_err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
      g_clear_error(p_err);
      b_ok = pathutil_dir_is_safe(p_dir, p_err);
   }
   return (b_ok);
}

GFile *
pathutil_unique_child(GFile *p_dir, const char *c_prefix, const char *c_suffix,
                      guint u_start) {
   g_return_val_if_fail(G_IS_FILE(p_dir), NULL);
   g_return_val_if_fail(c_prefix != NULL, NULL);
   g_return_val_if_fail(c_suffix != NULL, NULL);
   /* The prefix/suffix are user-controlled file-name pieces, so they are
    * concatenated -- never passed as a printf format (a name like
    * "50% off.jpg" used to be mangled and "%s%s%s.jpg" crashed). */
   char  *c_name = g_strconcat(c_prefix, c_suffix, NULL);
   GFile *p_out  = NULL;
   for (guint u_n = u_start;; u_n++) {
      p_out = g_file_get_child(p_dir, c_name);
      if (!g_file_query_exists(p_out, NULL)) {
         g_free(c_name);
         return (p_out); /* c_name's basename is the unique name */
      }
      g_object_unref(p_out);
      p_out = NULL;
      if (u_n >= u_start + 100000) {
         break;
      }
      g_free(c_name);
      c_name = g_strdup_printf("%s-%u%s", c_prefix, u_n, c_suffix);
   }
   g_free(c_name);
   return (NULL);
}