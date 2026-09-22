/*:*
 * ggaze — configurable move destinations (undoable)
 *
 * Holds the ordered destination list (SettingsPair: name + absolute folder)
 * read from GSettings, moves a target set into one of them with the shared
 * "<stem>-<n><ext>" collision rule (pathutil_unique_child), and remembers the
 * last move so `u` can move the set back. Plain-C, unit-testable.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "mover.h"

#include <gio/gio.h>
#include <glib.h>

#include "pathutil.h"

struct Mover {
   GPtrArray *p_dests;    /* SettingsPair* (owned) */
   GPtrArray *p_last_src; /* GFile* (owned, for undo) */
   GPtrArray *p_last_dst; /* GFile* (owned, for undo) */
};

Mover *
mover_new(void) {
   Mover *p_m      = g_new0(Mover, 1);
   p_m->p_dests    = settings_pair_array_new();
   p_m->p_last_src = g_ptr_array_new_with_free_func(g_object_unref);
   p_m->p_last_dst = g_ptr_array_new_with_free_func(g_object_unref);
   return (p_m);
}

void
mover_delete(Mover *p_m) {
   if (p_m == NULL) {
      return;
   }
   g_ptr_array_unref(p_m->p_dests);
   g_ptr_array_unref(p_m->p_last_src);
   g_ptr_array_unref(p_m->p_last_dst);
   g_free(p_m);
}

void
mover_set_dests(Mover *p_m, const GPtrArray *p_dests) {
   g_return_if_fail(p_m != NULL);
   g_ptr_array_unref(p_m->p_dests);
   p_m->p_dests = settings_pair_array_copy(p_dests);
}

const GPtrArray *
mover_get_dests(Mover *p_m) {
   g_return_val_if_fail(p_m != NULL, NULL);
   return (p_m->p_dests);
}

/* The non-colliding destination for p_src inside p_ddir: "a.jpg", then
 * "a-1.jpg", "a-2.jpg", ... (suffix on the stem, before the extension).
 * NULL when no free name was found. */
static GFile *
_dest_for(GFile *p_ddir, GFile *p_src) {
   char       *c_base = g_file_get_basename(p_src);
   char       *c_stem = NULL;
   const char *c_ext  = NULL;
   pathutil_split_ext(c_base, &c_stem, &c_ext);
   GFile *p_dst = pathutil_unique_child(p_ddir, c_stem, c_ext, 1);
   g_free(c_stem);
   g_free(c_base);
   return (p_dst);
}

gboolean
mover_move(Mover *p_m, GList *p_files, const SettingsPair *p_dest,
           GError **p_err) {
   g_return_val_if_fail(p_m != NULL, FALSE);
   g_return_val_if_fail(p_dest != NULL, FALSE);
   GFile *p_ddir = g_file_new_for_path(p_dest->c_value);
   if (!pathutil_ensure_dir(p_ddir, p_err)) {
      g_object_unref(p_ddir);
      return (FALSE);
   }
   g_ptr_array_set_size(p_m->p_last_src, 0);
   g_ptr_array_set_size(p_m->p_last_dst, 0);

   gboolean b_ok = TRUE;
   for (GList *p_it = p_files; b_ok && p_it != NULL; p_it = p_it->next) {
      GFile *p_src = G_FILE(p_it->data);
      GFile *p_dst = _dest_for(p_ddir, p_src);
      if (p_dst == NULL) {
         g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_EXISTS,
                     "could not find a non-colliding move destination");
         b_ok = FALSE;
      } else if (!g_file_move(p_src, p_dst, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL,
                              NULL, NULL, p_err)) {
         g_object_unref(p_dst);
         b_ok = FALSE;
      } else {
         g_ptr_array_add(p_m->p_last_src, g_object_ref(p_src));
         g_ptr_array_add(p_m->p_last_dst, p_dst);
      }
   }
   g_object_unref(p_ddir);
   return (b_ok);
}

gboolean
mover_undo_last(Mover *p_m, GError **p_err) {
   g_return_val_if_fail(p_m != NULL, FALSE);
   if (p_m->p_last_dst->len == 0) {
      return (FALSE);
   }
   /* Restore from the end and drop each pair as soon as it is back, so a
    * failure part-way leaves only the still-moved files recorded: the next
    * undo then retries exactly those instead of failing forever on a file
    * that was already restored. */
   while (p_m->p_last_dst->len > 0) {
      guint  u_i   = p_m->p_last_dst->len - 1;
      GFile *p_dst = g_ptr_array_index(p_m->p_last_dst, u_i);
      GFile *p_src = g_ptr_array_index(p_m->p_last_src, u_i);
      if (!g_file_move(p_dst, p_src, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL,
                       NULL, p_err)) {
         return (FALSE);
      }
      g_ptr_array_remove_index(p_m->p_last_dst, u_i);
      g_ptr_array_remove_index(p_m->p_last_src, u_i);
   }
   return (TRUE);
}

gboolean
mover_can_undo(Mover *p_m) {
   g_return_val_if_fail(p_m != NULL, FALSE);
   return (p_m->p_last_dst->len > 0);
}

void
mover_clear_last(Mover *p_m) {
   g_return_if_fail(p_m != NULL);
   g_ptr_array_set_size(p_m->p_last_src, 0);
   g_ptr_array_set_size(p_m->p_last_dst, 0);
}
