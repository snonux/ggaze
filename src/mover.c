/* mover.c — configurable move destinations with undo. */
#include "mover.h"

#include "pathutil.h"

struct Mover {
   GPtrArray *p_dests;    /* SettingsPair* (owned) */
   GPtrArray *p_last_src; /* GFile* (owned, for undo) */
   GPtrArray *p_last_dst; /* GFile* (owned, for undo) */
};

Mover *
mover_new(void) {
   Mover *m      = g_new0(Mover, 1);
   m->p_dests    = g_ptr_array_new_with_free_func(settings_pair_free);
   m->p_last_src = g_ptr_array_new_with_free_func(g_object_unref);
   m->p_last_dst = g_ptr_array_new_with_free_func(g_object_unref);
   return m;
}

void
mover_delete(Mover *m) {
   if (m == NULL)
      return;
   g_ptr_array_unref(m->p_dests);
   g_ptr_array_unref(m->p_last_src);
   g_ptr_array_unref(m->p_last_dst);
   g_free(m);
}

void
mover_set_dests(Mover *m, const GPtrArray *p_dests) {
   g_return_if_fail(m != NULL);
   g_ptr_array_unref(m->p_dests);
   m->p_dests = settings_pair_array_copy(p_dests);
}

const GPtrArray *
mover_get_dests(Mover *m) {
   g_return_val_if_fail(m != NULL, NULL);
   return m->p_dests;
}

gboolean
mover_move(Mover *m, GList *p_files, const SettingsPair *p_dest,
           GError **p_err) {
   g_return_val_if_fail(m != NULL, FALSE);
   g_return_val_if_fail(p_dest != NULL, FALSE);
   GFile *p_ddir = g_file_new_for_path(p_dest->c_value);
   if (!pathutil_ensure_dir(p_ddir, p_err)) {
      g_object_unref(p_ddir);
      return FALSE;
   }
   GError *e = NULL;

   g_ptr_array_set_size(m->p_last_src, 0);
   g_ptr_array_set_size(m->p_last_dst, 0);

   for (GList *it = p_files; it != NULL; it = it->next) {
      GFile *src  = G_FILE(it->data);
      char  *base = g_file_get_basename(src);
      /* Suffix on the stem (before the extension): a.jpg -> a-1.jpg. */
      char       *c_s = NULL;
      const char *c_e = NULL;
      pathutil_split_ext(base, &c_s, &c_e);
      GFile *dst = pathutil_unique_child(p_ddir, c_s, c_e, 1);
      g_free(c_s);
      g_free(base);
      if (dst == NULL) {
         g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_EXISTS,
                     "could not find a non-colliding move destination");
         g_object_unref(p_ddir);
         return FALSE;
      }
      if (!g_file_move(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL,
                       NULL, &e)) {
         g_propagate_error(p_err, e);
         g_object_unref(dst);
         g_object_unref(p_ddir);
         return FALSE;
      }
      g_ptr_array_add(m->p_last_src, g_object_ref(src));
      g_ptr_array_add(m->p_last_dst, dst);
   }
   g_object_unref(p_ddir);
   return TRUE;
}

gboolean
mover_undo_last(Mover *m, GError **p_err) {
   g_return_val_if_fail(m != NULL, FALSE);
   if (m->p_last_dst->len == 0) {
      return (FALSE);
   }
   /* Restore from the end and drop each pair as soon as it is back, so a
    * failure part-way leaves only the still-moved files recorded: the next
    * undo then retries exactly those instead of failing forever on a file
    * that was already restored. */
   while (m->p_last_dst->len > 0) {
      guint   u_i = m->p_last_dst->len - 1;
      GFile  *dst = g_ptr_array_index(m->p_last_dst, u_i);
      GFile  *src = g_ptr_array_index(m->p_last_src, u_i);
      GError *e   = NULL;
      if (!g_file_move(dst, src, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL,
                       NULL, &e)) {
         g_propagate_error(p_err, e);
         return (FALSE);
      }
      g_ptr_array_remove_index(m->p_last_dst, u_i);
      g_ptr_array_remove_index(m->p_last_src, u_i);
   }
   return (TRUE);
}

gboolean
mover_can_undo(Mover *m) {
   g_return_val_if_fail(m != NULL, FALSE);
   return m->p_last_dst->len > 0;
}

void
mover_clear_last(Mover *m) {
   g_return_if_fail(m != NULL);
   g_ptr_array_set_size(m->p_last_src, 0);
   g_ptr_array_set_size(m->p_last_dst, 0);
}