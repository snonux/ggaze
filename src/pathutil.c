/* pathutil.c — shared path/string helpers (see pathutil.h). */
#include "pathutil.h"

#include <gio/gio.h>
#include <glib.h>

char *
pathutil_str_replace(const char *c_str, const char *c_old, const char *c_new) {
   if (c_str == NULL || c_old == NULL || c_new == NULL) {
      return (NULL);
   }
   GString    *p_out     = g_string_new(NULL);
   const char *p         = c_str;
   gsize       u_old_len = strlen(c_old);
   while (*p != '\0') {
      if (strncmp(p, c_old, u_old_len) == 0) {
         g_string_append(p_out, c_new);
         p += u_old_len;
      } else {
         g_string_append_c(p_out, *p);
         p++;
      }
   }
   return (g_string_free(p_out, FALSE));
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
pathutil_unique_child(GFile *p_dir, const char *c_first, const char *c_fmt,
                      guint u_start) {
   g_return_val_if_fail(G_IS_FILE(p_dir), NULL);
   g_return_val_if_fail(c_first != NULL, NULL);
   g_return_val_if_fail(c_fmt != NULL, NULL);
   char  *c_name = g_strdup(c_first);
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
      c_name = g_strdup_printf(c_fmt, u_n);
   }
   g_free(c_name);
   return (NULL);
}