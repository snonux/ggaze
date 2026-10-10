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

#include "pathutil.h"

struct Trash {
   GFile *p_dir; /* the shoot dir; .Trash = p_dir/.Trash */
   /* The last batch binned, for the undo: GFile* pairs (owned refs), the
    * original path at 2n and the .Trash path at 2n+1, in bin order. One
    * file for a plain trash_bin; trash_bin_more adds to it. */
   GPtrArray *p_last;
};

static GFile *
_trash_dir(Trash *p_t) {
   return (g_file_get_child(p_t->p_dir, ".Trash"));
}

Trash *
trash_new(GFile *p_shoot_dir) {
   g_return_val_if_fail(G_IS_FILE(p_shoot_dir), NULL);
   Trash *p_t  = g_new(Trash, 1);
   p_t->p_dir  = (GFile *)g_object_ref(p_shoot_dir);
   p_t->p_last = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
   return (p_t);
}

void
trash_delete(Trash *p_t) {
   if (p_t == NULL) {
      return;
   }
   g_clear_pointer(&p_t->p_last, g_ptr_array_unref);
   g_clear_object(&p_t->p_dir);
   g_free(p_t);
}

/* Move p_file into .Trash and add it to the undo batch, which b_more keeps
 * and extends; without it a successful move starts a new batch. */
static gboolean
_bin(Trash *p_t, GFile *p_file, gboolean b_more, GError **p_err) {
   g_return_val_if_fail(p_t != NULL, FALSE);
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   GFile *p_td = _trash_dir(p_t);
   if (!pathutil_ensure_dir(p_td, p_err)) {
      g_object_unref(p_td);
      return (FALSE);
   }
   /* Collision-suffix on the stem ("a.jpg" -> "a-1.jpg"), the same rule the
    * mover and enhance-save use, so a binned file keeps its extension. */
   char       *c_base = g_file_get_basename(p_file);
   char       *c_stem = NULL;
   const char *c_ext  = NULL;
   pathutil_split_ext(c_base, &c_stem, &c_ext);
   GFile *p_dst = pathutil_unique_child(p_td, c_stem, c_ext, 1);
   g_free(c_stem);
   g_free(c_base);
   g_object_unref(p_td);
   if (p_dst == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_EXISTS,
                  "could not find a non-colliding trash name");
      return (FALSE);
   }

   gboolean b_ok = g_file_move(p_file, p_dst, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                               NULL, NULL, NULL, p_err);
   if (b_ok) {
      if (!b_more) {
         g_ptr_array_set_size(p_t->p_last, 0);
      }
      g_ptr_array_add(p_t->p_last, g_object_ref(p_file));
      g_ptr_array_add(p_t->p_last, p_dst); /* take ownership */
   } else {
      g_object_unref(p_dst);
   }
   return (b_ok);
}

gboolean
trash_bin(Trash *p_t, GFile *p_file, GError **p_err) {
   return (_bin(p_t, p_file, FALSE, p_err));
}

gboolean
trash_bin_more(Trash *p_t, GFile *p_file, GError **p_err) {
   return (_bin(p_t, p_file, TRUE, p_err));
}

gboolean
trash_permanently_delete(GFile *p_file, GError **p_err) {
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   return (g_file_delete(p_file, NULL, p_err));
}

gboolean
trash_restore_last(Trash *p_t, GError **p_err) {
   g_return_val_if_fail(p_t != NULL, FALSE);
   if (p_t->p_last->len == 0) {
      return (FALSE);
   }
   /* Newest first; a file that will not go back stays in the batch with
    * the ones before it, so a second undo can try again. */
   while (p_t->p_last->len >= 2) {
      guint  u_i   = p_t->p_last->len - 2;
      GFile *p_src = (GFile *)g_ptr_array_index(p_t->p_last, u_i);
      GFile *p_dst = (GFile *)g_ptr_array_index(p_t->p_last, u_i + 1);
      if (!g_file_move(p_dst, p_src, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL,
                       NULL, p_err)) {
         return (FALSE);
      }
      g_ptr_array_set_size(p_t->p_last, (gint)u_i);
   }
   return (TRUE);
}

guint
trash_count(Trash *p_t) {
   g_return_val_if_fail(p_t != NULL, 0);
   GFile           *p_td = _trash_dir(p_t);
   GFileEnumerator *p_e  = g_file_enumerate_children(
      p_td, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
   guint u_n = 0;
   if (p_e != NULL) {
      GFileInfo *p_info;
      while ((p_info = g_file_enumerator_next_file(p_e, NULL, NULL)) != NULL) {
         u_n++;
         g_object_unref(p_info);
      }
      g_object_unref(p_e);
   }
   g_object_unref(p_td);
   return (u_n);
}

gboolean
trash_empty(Trash *p_t, guint *p_deleted, GError **p_err) {
   g_return_val_if_fail(p_t != NULL, FALSE);
   guint            u_done = 0;
   gboolean         b_ok   = TRUE;
   GFile           *p_td   = _trash_dir(p_t);
   GFileEnumerator *p_e    = g_file_enumerate_children(
      p_td, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (p_e != NULL) {
      GFileInfo *p_info;
      while (b_ok &&
             (p_info = g_file_enumerator_next_file(p_e, NULL, NULL)) != NULL) {
         GFile *p_f = g_file_get_child(p_td, g_file_info_get_name(p_info));
         b_ok       = g_file_delete(p_f, NULL, p_err);
         u_done += b_ok ? 1 : 0;
         g_object_unref(p_f);
         g_object_unref(p_info);
      }
      g_object_unref(p_e);
   }
   g_object_unref(p_td);
   /* Whatever was restorable is gone now. */
   g_ptr_array_set_size(p_t->p_last, 0);
   if (p_deleted != NULL) {
      *p_deleted = u_done;
   }
   return (b_ok);
}

gboolean
trash_can_undo(Trash *p_t) {
   g_return_val_if_fail(p_t != NULL, FALSE);
   return (p_t->p_last->len > 0);
}