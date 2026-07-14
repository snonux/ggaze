/* mover.c — configurable move destinations with undo. */
#include "mover.h"

struct Mover {
   GPtrArray *p_dests;    /* MoverDest* (owned) */
   GPtrArray *p_last_src; /* GFile* (owned, for undo) */
   GPtrArray *p_last_dst; /* GFile* (owned, for undo) */
};

static void
_dest_free(gpointer p) {
   MoverDest *d = (MoverDest *)p;
   if (d != NULL) {
      g_free(d->c_name);
      g_free(d->c_path);
      g_free(d);
   }
}

Mover *
mover_new(void) {
   Mover *m      = g_new0(Mover, 1);
   m->p_dests    = g_ptr_array_new_with_free_func(_dest_free);
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
   g_ptr_array_set_size(m->p_dests, 0);
   if (p_dests != NULL) {
      for (guint i = 0; i < p_dests->len; i++) {
         const MoverDest *d =
            (const MoverDest *)g_ptr_array_index((GPtrArray *)p_dests, i);
         MoverDest *nd = g_new(MoverDest, 1);
         nd->c_name    = g_strdup(d->c_name);
         nd->c_path    = g_strdup(d->c_path);
         g_ptr_array_add(m->p_dests, nd);
      }
   }
}

const GPtrArray *
mover_get_dests(Mover *m) {
   g_return_val_if_fail(m != NULL, NULL);
   return m->p_dests;
}

gboolean
mover_move(Mover *m, GList *p_files, const MoverDest *p_dest, GError **p_err) {
   g_return_val_if_fail(m != NULL, FALSE);
   g_return_val_if_fail(p_dest != NULL, FALSE);
   GFile  *p_ddir = g_file_new_for_path(p_dest->c_path);
   GError *e      = NULL;
   g_file_make_directory_with_parents(p_ddir, NULL, &e);
   if (e != NULL && !g_error_matches(e, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
      g_propagate_error(p_err, e);
      g_object_unref(p_ddir);
      return FALSE;
   }
   g_clear_error(&e);

   g_ptr_array_set_size(m->p_last_src, 0);
   g_ptr_array_set_size(m->p_last_dst, 0);

   for (GList *it = p_files; it != NULL; it = it->next) {
      GFile *src  = G_FILE(it->data);
      char  *base = g_file_get_basename(src);
      GFile *dst  = g_file_get_child(p_ddir, base);
      g_free(base);
      for (guint n = 1; g_file_query_exists(dst, NULL); n++) {
         g_object_unref(dst);
         /* Suffix on the stem (before the extension): a.jpg -> a-1.jpg */
         char       *c_b = g_file_get_basename(src);
         const char *c_d = strrchr(c_b, '.');
         char       *c_s =
            (c_d && c_d != c_b) ? g_strndup(c_b, c_d - c_b) : g_strdup(c_b);
         const char *c_e = (c_d && c_d != c_b) ? c_d : "";
         char       *nb  = g_strdup_printf("%s-%u%s", c_s, n, c_e);
         g_free(c_s);
         g_free(c_b);
         dst = g_file_get_child(p_ddir, nb);
         g_free(nb);
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
   if (m->p_last_dst->len == 0)
      return FALSE;
   for (guint i = 0; i < m->p_last_dst->len; i++) {
      GFile  *dst = g_ptr_array_index(m->p_last_dst, i);
      GFile  *src = g_ptr_array_index(m->p_last_src, i);
      GError *e   = NULL;
      if (!g_file_move(dst, src, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL,
                       NULL, &e)) {
         g_propagate_error(p_err, e);
         return FALSE;
      }
   }
   g_ptr_array_set_size(m->p_last_src, 0);
   g_ptr_array_set_size(m->p_last_dst, 0);
   return TRUE;
}

gboolean
mover_can_undo(Mover *m) {
   g_return_val_if_fail(m != NULL, FALSE);
   return m->p_last_dst->len > 0;
}