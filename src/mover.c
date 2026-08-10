/* mover.c — configurable move destinations with undo. */
#include "mover.h"

#include "pathutil.h"

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
   GFile *p_ddir = g_file_new_for_path(p_dest->c_path);
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
      const char *c_d = strrchr(base, '.');
      char       *c_s =
         (c_d && c_d != base) ? g_strndup(base, c_d - base) : g_strdup(base);
      const char *c_e     = (c_d && c_d != base) ? c_d : "";
      char       *c_first = g_strdup_printf("%s%s", c_s, c_e);
      char       *c_fmt   = g_strdup_printf("%s-%%u%s", c_s, c_e);
      GFile      *dst     = pathutil_unique_child(p_ddir, c_first, c_fmt, 1);
      g_free(c_fmt);
      g_free(c_first);
      g_free(c_s);
      g_free(base);
      if (dst == NULL) {
         g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES,
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

void
mover_clear_last(Mover *m) {
   g_return_if_fail(m != NULL);
   g_ptr_array_set_size(m->p_last_src, 0);
   g_ptr_array_set_size(m->p_last_dst, 0);
}