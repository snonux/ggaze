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

/* Refuse a pre-existing move destination that is not a real directory.
 * Queried with G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS so a symlink is reported
 * as a symlink (type G_FILE_TYPE_SYMBOLIC_LINK) instead of being resolved
 * and stat'd through its target. Unlike trash.c's single hidden .Trash
 * folder, mover destinations (MoverDest) are user-configured, arbitrary
 * paths -- once wired into the UI, a destination directory replaced by a
 * symlink (attacker or leftover corruption) would otherwise be silently
 * followed, moving every selected file wherever the link points. As in
 * trash.c's _trash_dir_is_safe(), the only safe response to a symlink or
 * other non-directory (e.g. a plain file) is to fail with a clear GError --
 * never auto-delete/replace the offending path ourselves, which would be
 * its own TOCTOU hazard. Note: a narrow TOCTOU window still remains between
 * this check and the g_file_move() calls below (something could replace
 * the directory in between); fully closing that would need
 * openat()/O_NOFOLLOW/renameat(), out of scope here -- same residual risk
 * documented for trash.c's identical fix (au0). */
static gboolean
_mover_dest_is_safe(GFile *p_ddir, GError **p_err) {
   GFileInfo *p_info =
      g_file_query_info(p_ddir, "standard::type",
                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, p_err);
   if (p_info == NULL) {
      return FALSE;
   }
   GFileType e_type = g_file_info_get_file_type(p_info);
   g_object_unref(p_info);
   if (e_type != G_FILE_TYPE_DIRECTORY) {
      char *c_path = g_file_get_path(p_ddir);
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                  "refusing to move into '%s': existing path is not a real "
                  "directory (found a symlink or other non-directory)",
                  c_path != NULL ? c_path : "?");
      g_free(c_path);
      return FALSE;
   }
   return TRUE;
}

/* Ensure the destination directory exists (lazily), mirroring trash.c's
 * _ensure_trash_dir(). If g_file_make_directory_with_parents() reports
 * G_IO_ERROR_EXISTS, something is already at that path -- verify via
 * _mover_dest_is_safe() that it is a genuine directory before treating it
 * as usable, rather than blindly assuming success as before. */
static gboolean
_ensure_mover_dest_dir(GFile *p_ddir, GError **p_err) {
   gboolean b_ok = g_file_make_directory_with_parents(p_ddir, NULL, p_err);
   if (!b_ok) {
      if (p_err != NULL &&
          g_error_matches(*p_err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
         g_clear_error(p_err);
         b_ok = _mover_dest_is_safe(p_ddir, p_err);
      }
   }
   return b_ok;
}

gboolean
mover_move(Mover *m, GList *p_files, const MoverDest *p_dest, GError **p_err) {
   g_return_val_if_fail(m != NULL, FALSE);
   g_return_val_if_fail(p_dest != NULL, FALSE);
   GFile *p_ddir = g_file_new_for_path(p_dest->c_path);
   if (!_ensure_mover_dest_dir(p_ddir, p_err)) {
      g_object_unref(p_ddir);
      return FALSE;
   }
   GError *e = NULL;

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

void
mover_clear_last(Mover *m) {
   g_return_if_fail(m != NULL);
   g_ptr_array_set_size(m->p_last_src, 0);
   g_ptr_array_set_size(m->p_last_dst, 0);
}