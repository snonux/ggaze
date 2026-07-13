/*:*
 * ggaze — local ./Trash bin
 *
 * Implements the lazy .Trash folder, collision-suffixed binning, permanent
 * delete, and one-level restore. Plain-C.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "trash.h"

struct Trash {
   GFile *p_dir;      /* the shoot dir; .Trash = p_dir/.Trash */
   GFile *p_last_src; /* original path of the last binned file */
   GFile *p_last_dst; /* .Trash path of the last binned file */
};

static GFile *
_trash_dir(Trash *p_t) {
   return (g_file_get_child(p_t->p_dir, ".Trash"));
}

/* Ensure .Trash exists (lazily). */
static gboolean
_ensure_trash_dir(Trash *p_t, GError **p_err) {
   GFile   *p_td = _trash_dir(p_t);
   gboolean b_ok = g_file_make_directory_with_parents(p_td, NULL, p_err);
   if (!b_ok) {
      if (p_err != NULL &&
          g_error_matches(*p_err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
         g_clear_error(p_err);
         b_ok = TRUE;
      }
   }
   g_object_unref(p_td);
   return (b_ok);
}

/* Find a non-colliding name in .Trash for c_basename. Caller frees the path. */
static char *
_unique_trash_name(Trash *p_t, const char *c_basename) {
   GFile *p_td   = _trash_dir(p_t);
   char  *c_name = g_strdup(c_basename);
   for (guint u_n = 1; u_n < 100000; u_n++) {
      GFile *p_candidate = g_file_get_child(p_td, c_name);
      if (!g_file_query_exists(p_candidate, NULL)) {
         g_object_unref(p_candidate);
         g_object_unref(p_td);
         return (c_name); /* the basename is the unique name */
      }
      g_object_unref(p_candidate);
      g_free(c_name);
      c_name = g_strdup_printf("%s-%u", c_basename, u_n);
   }
   g_free(c_name);
   g_object_unref(p_td);
   return (NULL);
}

Trash *
trash_new(GFile *p_shoot_dir) {
   g_return_val_if_fail(G_IS_FILE(p_shoot_dir), NULL);
   Trash *p_t      = g_new(Trash, 1);
   p_t->p_dir      = (GFile *)g_object_ref(p_shoot_dir);
   p_t->p_last_src = NULL;
   p_t->p_last_dst = NULL;
   return (p_t);
}

void
trash_delete(Trash *p_t) {
   if (p_t == NULL) {
      return;
   }
   g_clear_object(&p_t->p_last_dst);
   g_clear_object(&p_t->p_last_src);
   g_clear_object(&p_t->p_dir);
   g_free(p_t);
}

gboolean
trash_bin(Trash *p_t, GFile *p_file, GError **p_err) {
   g_return_val_if_fail(p_t != NULL, FALSE);
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   if (!_ensure_trash_dir(p_t, p_err)) {
      return (FALSE);
   }
   char *c_base = g_file_get_basename(p_file);
   char *c_name = _unique_trash_name(p_t, c_base);
   g_free(c_base);
   if (c_name == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES,
                  "could not find a non-colliding trash name");
      return (FALSE);
   }
   GFile *p_td  = _trash_dir(p_t);
   GFile *p_dst = g_file_get_child(p_td, c_name);
   g_free(c_name);
   g_object_unref(p_td);

   gboolean b_ok = g_file_move(p_file, p_dst, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                               NULL, NULL, NULL, p_err);
   if (b_ok) {
      g_clear_object(&p_t->p_last_src);
      g_clear_object(&p_t->p_last_dst);
      p_t->p_last_src = (GFile *)g_object_ref(p_file);
      p_t->p_last_dst = p_dst; /* take ownership (ref'd by get_child) */
   } else {
      g_object_unref(p_dst);
   }
   return (b_ok);
}

gboolean
trash_permanently_delete(Trash *p_t, GFile *p_file, GError **p_err) {
   (void)p_t;
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   return (g_file_delete(p_file, NULL, p_err));
}

gboolean
trash_restore_last(Trash *p_t, GError **p_err) {
   g_return_val_if_fail(p_t != NULL, FALSE);
   if (p_t->p_last_dst == NULL || p_t->p_last_src == NULL) {
      return (FALSE);
   }
   gboolean b_ok =
      g_file_move(p_t->p_last_dst, p_t->p_last_src,
                  G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, p_err);
   if (b_ok) {
      g_clear_object(&p_t->p_last_src);
      g_clear_object(&p_t->p_last_dst);
   }
   return (b_ok);
}

gboolean
trash_can_undo(Trash *p_t) {
   g_return_val_if_fail(p_t != NULL, FALSE);
   return (p_t->p_last_dst != NULL);
}