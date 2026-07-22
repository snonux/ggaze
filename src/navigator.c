/*:*
 * ggaze — directory navigator
 *
 * GObject (no GtkWidget) holding the current folder listing: filter to
 * image MIME, sort (name/time/size), cursor + path-based marks, GFileMonitor
 * with debounce, nearest-fallback on current-file removal. Emits "changed".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "navigator.h"

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>
#include <string.h>

/* Extensions (lowercase, no dot). */
static const char *IMAGE_EXTS[] = {"jpg",  "jpeg", "png", "gif", "webp",
                                   "tif",  "tiff", "ico", "jxl", "avif",
                                   "heif", "heic", "bmp", "svg", NULL};
static const char *RAW_EXTS[]   = {"raf", "cr2", "cr3", "nef", "arw",
                                   "dng", "orf", "rw2", "pef", "srw",
                                   "3fr", "x3f", NULL};

#define GGAZE_DEFAULT_DEBOUNCE_MS 250

typedef struct {
   GFile *file; /* owned ref */
   gchar *name; /* owned (basename) */
   gint64 mtime;
   gint64 size;
} Entry;

struct _Navigator {
   GObject     parent_instance;
   GFile      *p_dir;     /* owned */
   GPtrArray  *p_files;   /* GFile* (owned refs), sorted/filtered */
   gint        i_current; /* -1 if empty */
   GgazeSort   e_sort;
   gboolean    b_wrap;
   gboolean    b_hide_raw;
   GHashTable *p_marks;   /* GFile* (owned refs) -> presence */
   GHashTable *p_removed; /* GFile* (owned refs): trashed/deleted, dimmed */
   GFile *p_last_mark;    /* anchor for `V` range-mark (owned ref, NULL=none) */
   GFileMonitor *p_monitor;
   guint         u_debounce_ms;
   guint         u_debounce_id; /* 0 = none pending */
};

G_DEFINE_TYPE(Navigator, navigator, G_TYPE_OBJECT)

static guint u_changed_signal = 0;

/* --- helpers -------------------------------------------------------------- */

static const char *
_ext_of(const char *c_name) {
   const char *c_dot = strrchr(c_name, '.');
   if (c_dot == NULL || c_dot == c_name) {
      return (NULL);
   }
   return (c_dot + 1);
}

static gboolean
_str_in_set(const char *c_s, const char **set) {
   if (c_s == NULL) {
      return (FALSE);
   }
   for (gsize u_i = 0; set[u_i] != NULL; u_i++) {
      if (g_ascii_strcasecmp(c_s, set[u_i]) == 0) {
         return (TRUE);
      }
   }
   return (FALSE);
}

static gboolean
_is_raw_ext(const char *c_ext) {
   return (_str_in_set(c_ext, RAW_EXTS));
}

static gboolean
_is_image_file(const char *c_name, const char *c_ct) {
   if (c_ct != NULL && g_str_has_prefix(c_ct, "image/")) {
      return (TRUE);
   }
   const char *c_ext = _ext_of(c_name);
   return (_str_in_set(c_ext, IMAGE_EXTS) || _str_in_set(c_ext, RAW_EXTS));
}

static gboolean
_is_jpeg_ext(const char *c_ext) {
   return (c_ext != NULL && (g_ascii_strcasecmp(c_ext, "jpg") == 0 ||
                             g_ascii_strcasecmp(c_ext, "jpeg") == 0));
}

/* Owned stem (basename without extension, lowercased for compare). */
static char *
_stem_lower(const char *c_name) {
   const char *c_dot = strrchr(c_name, '.');
   gsize u_len  = (c_dot != NULL && c_dot != c_name) ? (gsize)(c_dot - c_name)
                                                     : strlen(c_name);
   char *c_stem = g_strndup(c_name, u_len);
   for (char *p = c_stem; *p != '\0'; p++) {
      *p = (char)g_ascii_tolower((gint)*p);
   }
   return (c_stem);
}

static void
_entry_free(gpointer p_void) {
   Entry *p_e = (Entry *)p_void;
   g_clear_object(&p_e->file);
   g_free(p_e->name);
   g_free(p_e);
}

static gint
_compare_entries(gconstpointer p_a, gconstpointer p_b, gpointer p_data) {
   const Entry *p_ea   = *(Entry *const *)p_a;
   const Entry *p_eb   = *(Entry *const *)p_b;
   GgazeSort    e_sort = (GgazeSort)GPOINTER_TO_INT(p_data);

   switch (e_sort) {
   case GGAZE_SORT_TIME:
      if (p_ea->mtime < p_eb->mtime) {
         return (-1);
      }
      if (p_ea->mtime > p_eb->mtime) {
         return (1);
      }
      break;
   case GGAZE_SORT_SIZE:
      if (p_ea->size < p_eb->size) {
         return (-1);
      }
      if (p_ea->size > p_eb->size) {
         return (1);
      }
      break;
   case GGAZE_SORT_NAME:
   default:
      break;
   }
   /* Tie-break (and the NAME case) by collated basename. */
   return (g_utf8_collate(p_ea->name, p_eb->name));
}

static gint
_find_index_by_file(Navigator *p_nav, GFile *p_file) {
   if (p_file == NULL) {
      return (-1);
   }
   for (guint u_i = 0; u_i < p_nav->p_files->len; u_i++) {
      if (g_file_equal(p_file,
                       (GFile *)g_ptr_array_index(p_nav->p_files, u_i))) {
         return ((gint)u_i);
      }
   }
   return (-1);
}

/* Re-read the directory, filter, hide-raw, sort, and commit; keep the current
 * file by path, falling back to the nearest by position if it is gone. */
static void
_relist(Navigator *p_nav) {
   GError          *p_err  = NULL;
   GFileEnumerator *p_enum = g_file_enumerate_children(
      p_nav->p_dir,
      "standard::name,standard::type,standard::content-type,"
      "time::modified,standard::size",
      G_FILE_QUERY_INFO_NONE, NULL, &p_err);
   if (p_enum == NULL) {
      if (p_err != NULL) {
         g_warning("navigator: enumerate failed: %s", p_err->message);
         g_error_free(p_err);
      }
      return;
   }

   GPtrArray  *p_entries = g_ptr_array_new_with_free_func(_entry_free);
   GHashTable *p_jpeg_stems =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

   GFileInfo *p_info = NULL;
   while ((p_info = g_file_enumerator_next_file(p_enum, NULL, NULL)) != NULL) {
      const char *c_name = g_file_info_get_name(p_info);
      if (c_name == NULL || c_name[0] == '.') {
         g_object_unref(p_info);
         continue;
      }
      if (g_file_info_get_file_type(p_info) != G_FILE_TYPE_REGULAR) {
         g_object_unref(p_info);
         continue;
      }
      const char *c_ct = g_file_info_get_content_type(p_info);
      if (!_is_image_file(c_name, c_ct)) {
         g_object_unref(p_info);
         continue;
      }
      Entry *p_e = g_new(Entry, 1);
      p_e->file  = g_file_get_child(p_nav->p_dir, c_name);
      p_e->name  = g_strdup(c_name);
      p_e->mtime = (gint64)g_file_info_get_attribute_uint64(
         p_info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
      p_e->size = (gint64)g_file_info_get_size(p_info);
      g_object_unref(p_info);

      const char *c_ext = _ext_of(p_e->name); /* p_e->name is owned; c_name
                                               * was borrowed from p_info */
      if (_is_jpeg_ext(c_ext)) {
         char *c_stem = _stem_lower(p_e->name);
         g_hash_table_add(p_jpeg_stems, c_stem);
      }
      g_ptr_array_add(p_entries, p_e);
   }
   g_object_unref(p_enum);

   /* Drop RAW sidecars that have a JPEG twin (decision #33). */
   if (p_nav->b_hide_raw) {
      for (gsize u_i = p_entries->len; u_i > 0; u_i--) {
         Entry      *p_e   = (Entry *)g_ptr_array_index(p_entries, u_i - 1);
         const char *c_ext = _ext_of(p_e->name);
         if (_is_raw_ext(c_ext)) {
            char *c_stem = _stem_lower(p_e->name);
            if (g_hash_table_contains(p_jpeg_stems, c_stem)) {
               g_ptr_array_remove_index(p_entries, u_i - 1);
            }
            g_free(c_stem);
         }
      }
   }
   g_hash_table_unref(p_jpeg_stems);

   g_ptr_array_sort_with_data(p_entries, _compare_entries,
                              GINT_TO_POINTER((gint)p_nav->e_sort));

   /* Remember current by path, then commit. */
   GFile *p_keep = NULL;
   if (p_nav->i_current >= 0 && (guint)p_nav->i_current < p_nav->p_files->len) {
      p_keep = g_object_ref(
         g_ptr_array_index(p_nav->p_files, (guint)p_nav->i_current));
   }
   gint i_old = p_nav->i_current;

   g_ptr_array_set_size(p_nav->p_files, 0);
   for (gsize u_i = 0; u_i < p_entries->len; u_i++) {
      Entry *p_e = (Entry *)g_ptr_array_index(p_entries, u_i);
      g_ptr_array_add(p_nav->p_files, g_object_ref(p_e->file));
   }

   if (p_keep != NULL) {
      gint i_found = _find_index_by_file(p_nav, p_keep);
      if (i_found >= 0) {
         p_nav->i_current = i_found;
      } else if (p_nav->p_files->len == 0) {
         p_nav->i_current = -1;
      } else {
         p_nav->i_current = CLAMP(i_old, 0, (gint)p_nav->p_files->len - 1);
      }
      g_object_unref(p_keep);
   } else if (p_nav->p_files->len == 0) {
      p_nav->i_current = -1;
   } else if (p_nav->i_current < 0) {
      p_nav->i_current = 0;
   }

   /* Prune marks whose file is no longer in the listing (external delete via
    * the monitor, or a rescan). Keeps marks consistent with the live folder. */
   GHashTableIter iter;
   gpointer       p_key;
   g_hash_table_iter_init(&iter, p_nav->p_marks);
   while (g_hash_table_iter_next(&iter, &p_key, NULL)) {
      if (_find_index_by_file(p_nav, (GFile *)p_key) < 0) {
         g_hash_table_iter_remove(&iter);
      }
   }
   /* The range anchor must also be a live marked file: drop it if its file
    * left the listing (external delete / rescan), so `V` no-ops instead of
    * range-marking from a stale path. */
   if (p_nav->p_last_mark != NULL &&
       _find_index_by_file(p_nav, p_nav->p_last_mark) < 0) {
      g_clear_object(&p_nav->p_last_mark);
   }

   /* Preserve removed (dimmed) items: keep them in the listing even if absent
    * from disk (trashed/deleted this session); un-remove any that reappeared.
    */
   {
      GHashTableIter riter;
      gpointer       rkey;
      g_hash_table_iter_init(&riter, p_nav->p_removed);
      while (g_hash_table_iter_next(&riter, &rkey, NULL)) {
         GFile *p_rf = (GFile *)rkey;
         if (_find_index_by_file(p_nav, p_rf) < 0) {
            g_ptr_array_add(p_nav->p_files, g_object_ref(p_rf));
         } else {
            g_hash_table_iter_remove(&riter);
         }
      }
   }

   g_ptr_array_unref(p_entries);
}

static void
_emit_changed(Navigator *p_nav) {
   g_signal_emit(p_nav, u_changed_signal, 0);
}

/* --- monitor / debounce -------------------------------------------------- */

static gboolean
_debounce_fire(gpointer p_data) {
   Navigator *p_nav     = (Navigator *)p_data;
   p_nav->u_debounce_id = 0;
   _relist(p_nav);
   _emit_changed(p_nav);
   return (G_SOURCE_REMOVE);
}

static void
_schedule_debounce(Navigator *p_nav) {
   if (p_nav->u_debounce_id != 0) {
      return; /* already pending; the single rescan picks up all changes */
   }
   p_nav->u_debounce_id =
      g_timeout_add(p_nav->u_debounce_ms, _debounce_fire, p_nav);
}

static void
monitor_changed_cb(GFileMonitor *p_mon, GFile *p_other, GFile *p_src,
                   GFileMonitorEvent e_ev, gpointer p_data) {
   (void)p_mon;
   (void)p_other;
   (void)p_src;
   switch (e_ev) {
   case G_FILE_MONITOR_EVENT_CREATED:
   case G_FILE_MONITOR_EVENT_DELETED:
   case G_FILE_MONITOR_EVENT_MOVED_IN:
   case G_FILE_MONITOR_EVENT_MOVED_OUT:
   case G_FILE_MONITOR_EVENT_RENAMED:
      _schedule_debounce((Navigator *)p_data);
      break;
   default:
      break;
   }
}

/* --- GObject ------------------------------------------------------------- */

static void
navigator_dispose(GObject *p_obj) {
   Navigator *p_nav = (Navigator *)p_obj;
   if (p_nav->u_debounce_id != 0) {
      g_source_remove(p_nav->u_debounce_id);
      p_nav->u_debounce_id = 0;
   }
   if (p_nav->p_monitor != NULL) {
      g_file_monitor_cancel(p_nav->p_monitor);
      g_clear_object(&p_nav->p_monitor);
   }
   g_clear_pointer(&p_nav->p_marks, g_hash_table_unref);
   g_clear_pointer(&p_nav->p_removed, g_hash_table_unref);
   g_clear_pointer(&p_nav->p_files, g_ptr_array_unref);
   g_clear_object(&p_nav->p_dir);
   g_clear_object(&p_nav->p_last_mark);
   G_OBJECT_CLASS(navigator_parent_class)->dispose(p_obj);
}

static void
navigator_class_init(NavigatorClass *p_klass) {
   GObjectClass *p_oc = G_OBJECT_CLASS(p_klass);
   p_oc->dispose      = navigator_dispose;
   u_changed_signal =
      g_signal_new("changed", G_TYPE_FROM_CLASS(p_klass), G_SIGNAL_RUN_LAST, 0,
                   NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 0);
}

static void
navigator_init(Navigator *p_nav) {
   p_nav->p_files =
      g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
   p_nav->p_marks =
      g_hash_table_new_full((GHashFunc)g_file_hash, (GEqualFunc)g_file_equal,
                            (GDestroyNotify)g_object_unref, NULL);
   p_nav->p_removed =
      g_hash_table_new_full((GHashFunc)g_file_hash, (GEqualFunc)g_file_equal,
                            (GDestroyNotify)g_object_unref, NULL);
   p_nav->i_current     = -1;
   p_nav->e_sort        = GGAZE_SORT_NAME;
   p_nav->b_wrap        = TRUE;
   p_nav->b_hide_raw    = TRUE;
   p_nav->u_debounce_ms = GGAZE_DEFAULT_DEBOUNCE_MS;
   p_nav->p_last_mark   = NULL;
}

/* --- public API ---------------------------------------------------------- */

Navigator *
navigator_new(GFile *p_dir, GgazeSort e_sort, gboolean b_wrap,
              gboolean b_hide_raw) {
   g_return_val_if_fail(G_IS_FILE(p_dir), NULL);
   Navigator *p_nav  = (Navigator *)g_object_new(GGAZE_TYPE_NAVIGATOR, NULL);
   p_nav->p_dir      = (GFile *)g_object_ref(p_dir);
   p_nav->e_sort     = e_sort;
   p_nav->b_wrap     = b_wrap;
   p_nav->b_hide_raw = b_hide_raw;
   _relist(p_nav);
   if (p_nav->i_current < 0 && p_nav->p_files->len > 0) {
      p_nav->i_current = 0;
   }

   GError *p_err    = NULL;
   p_nav->p_monitor = g_file_monitor_directory(
      p_nav->p_dir, G_FILE_MONITOR_WATCH_MOVES, NULL, &p_err);
   if (p_nav->p_monitor != NULL) {
      g_signal_connect(p_nav->p_monitor, "changed",
                       G_CALLBACK(monitor_changed_cb), p_nav);
   } else {
      if (p_err != NULL) {
         g_warning("navigator: monitor failed: %s", p_err->message);
         g_error_free(p_err);
      }
   }
   return (p_nav);
}

void
navigator_delete(Navigator *p_nav) {
   if (p_nav != NULL) {
      g_object_unref(p_nav);
   }
}

GFile *
navigator_get_dir(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), NULL);
   return (p_nav->p_dir);
}

guint
navigator_get_count(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), 0);
   return (p_nav->p_files->len);
}

GFile *
navigator_get_file(Navigator *p_nav, guint u_index) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), NULL);
   g_return_val_if_fail(u_index < p_nav->p_files->len, NULL);
   return ((GFile *)g_ptr_array_index(p_nav->p_files, u_index));
}

gint
navigator_get_current_index(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), -1);
   return (p_nav->i_current);
}

GFile *
navigator_get_current(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), NULL);
   if (p_nav->i_current < 0 || (guint)p_nav->i_current >= p_nav->p_files->len) {
      return (NULL);
   }
   return ((GFile *)g_ptr_array_index(p_nav->p_files, (guint)p_nav->i_current));
}

guint
navigator_get_remaining(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), 0);
   return (navigator_get_count(p_nav) - g_hash_table_size(p_nav->p_removed));
}

gboolean
navigator_is_removed(Navigator *p_nav, GFile *p_file) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), FALSE);
   return (g_hash_table_contains(p_nav->p_removed, p_file));
}

void
navigator_mark_removed(Navigator *p_nav, GFile *p_file) {
   g_return_if_fail(GGAZE_IS_NAVIGATOR(p_nav));
   g_return_if_fail(G_IS_FILE(p_file));
   /* Decision Q: marks clear on trash/delete. Drop this file's mark (and
    * the range anchor if it was the anchor) without touching unrelated
    * marks, so mark-based ops never target paths that no longer exist. */
   gboolean b_mark_changed = g_hash_table_remove(p_nav->p_marks, p_file);
   if (p_nav->p_last_mark != NULL && g_file_equal(p_nav->p_last_mark, p_file)) {
      g_clear_object(&p_nav->p_last_mark);
      b_mark_changed = TRUE;
   }
   gboolean b_removed_added = !g_hash_table_contains(p_nav->p_removed, p_file);
   if (b_removed_added) {
      g_hash_table_add(p_nav->p_removed, g_object_ref(p_file));
   }
   /* Emit when the removed set or the marks actually changed, so grid badges
    * and the header never go stale (e.g. re-marking a file already in
    * p_removed, then removing it again). */
   if (b_removed_added || b_mark_changed) {
      _emit_changed(p_nav);
   }
}

gboolean
navigator_unmark_removed(Navigator *p_nav, GFile *p_file) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), FALSE);
   return (g_hash_table_remove(p_nav->p_removed, p_file));
}

guint
navigator_get_removed_count(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), 0);
   return (g_hash_table_size(p_nav->p_removed));
}

gboolean
navigator_set_current(Navigator *p_nav, guint u_index) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), FALSE);
   if (u_index >= p_nav->p_files->len) {
      return (FALSE);
   }
   if (p_nav->i_current == (gint)u_index) {
      return (FALSE);
   }
   p_nav->i_current = (gint)u_index;
   _emit_changed(p_nav);
   return (TRUE);
}

gboolean
navigator_set_current_file(Navigator *p_nav, GFile *p_file) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), FALSE);
   gint i = _find_index_by_file(p_nav, p_file);
   if (i < 0) {
      return (FALSE);
   }
   if (p_nav->i_current == i) {
      return (FALSE);
   }
   p_nav->i_current = i;
   _emit_changed(p_nav);
   return (TRUE);
}

static gboolean
_advance(Navigator *p_nav, gint i_delta) {
   if (p_nav->p_files->len == 0) {
      return (FALSE);
   }
   gint i_new = p_nav->i_current + i_delta;
   if (i_new < 0 || i_new >= (gint)p_nav->p_files->len) {
      if (!p_nav->b_wrap) {
         return (FALSE);
      }
      i_new = (i_new < 0) ? (gint)p_nav->p_files->len - 1 : 0;
   }
   if (i_new == p_nav->i_current) {
      return (FALSE);
   }
   p_nav->i_current = i_new;
   _emit_changed(p_nav);
   return (TRUE);
}

gboolean
navigator_prev(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), FALSE);
   return (_advance(p_nav, -1));
}

gboolean
navigator_next(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), FALSE);
   return (_advance(p_nav, 1));
}

gboolean
navigator_first(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), FALSE);
   return (navigator_set_current(p_nav, 0));
}

gboolean
navigator_last(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), FALSE);
   if (p_nav->p_files->len == 0) {
      return (FALSE);
   }
   return (navigator_set_current(p_nav, p_nav->p_files->len - 1));
}

GgazeSort
navigator_get_sort(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), GGAZE_SORT_NAME);
   return (p_nav->e_sort);
}

void
navigator_set_sort(Navigator *p_nav, GgazeSort e_sort) {
   g_return_if_fail(GGAZE_IS_NAVIGATOR(p_nav));
   if (p_nav->e_sort == e_sort) {
      return;
   }
   p_nav->e_sort = e_sort;
   _relist(p_nav);
   _emit_changed(p_nav);
}

gboolean
navigator_get_wrap(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), FALSE);
   return (p_nav->b_wrap);
}

void
navigator_set_wrap(Navigator *p_nav, gboolean b_wrap) {
   g_return_if_fail(GGAZE_IS_NAVIGATOR(p_nav));
   p_nav->b_wrap = b_wrap;
}

gboolean
navigator_get_hide_raw(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), FALSE);
   return (p_nav->b_hide_raw);
}

void
navigator_set_hide_raw(Navigator *p_nav, gboolean b_hide_raw) {
   g_return_if_fail(GGAZE_IS_NAVIGATOR(p_nav));
   if (p_nav->b_hide_raw == b_hide_raw) {
      return;
   }
   p_nav->b_hide_raw = b_hide_raw;
   _relist(p_nav);
   _emit_changed(p_nav);
}

/* --- marks --------------------------------------------------------------- */

gboolean
navigator_is_marked(Navigator *p_nav, GFile *p_file) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), FALSE);
   return (g_hash_table_contains(p_nav->p_marks, p_file));
}

void
navigator_toggle_mark(Navigator *p_nav, GFile *p_file) {
   g_return_if_fail(GGAZE_IS_NAVIGATOR(p_nav));
   g_return_if_fail(G_IS_FILE(p_file));
   if (g_hash_table_contains(p_nav->p_marks, p_file)) {
      g_hash_table_remove(p_nav->p_marks, p_file);
      /* If the unmarked file was the range anchor, drop it: a subsequent `V`
       * has no meaningful anchor once the anchor itself is unmarked. */
      if (p_nav->p_last_mark != NULL &&
          g_file_equal(p_nav->p_last_mark, p_file)) {
         g_clear_object(&p_nav->p_last_mark);
      }
   } else {
      g_hash_table_add(p_nav->p_marks, g_object_ref(p_file));
      /* Remember this as the anchor for a later `V` range-mark. */
      g_set_object(&p_nav->p_last_mark, p_file);
   }
}

static void
_mark_index(Navigator *p_nav, guint u_index) {
   if (u_index >= p_nav->p_files->len) {
      return;
   }
   GFile *p_file = (GFile *)g_ptr_array_index(p_nav->p_files, u_index);
   if (!g_hash_table_contains(p_nav->p_marks, p_file)) {
      g_hash_table_add(p_nav->p_marks, g_object_ref(p_file));
   }
}

void
navigator_mark_range(Navigator *p_nav, GFile *p_from, GFile *p_to) {
   g_return_if_fail(GGAZE_IS_NAVIGATOR(p_nav));
   gint i_a = _find_index_by_file(p_nav, p_from);
   gint i_b = _find_index_by_file(p_nav, p_to);
   if (i_a < 0 || i_b < 0) {
      return;
   }
   gint i_lo = MIN(i_a, i_b);
   gint i_hi = MAX(i_a, i_b);
   for (gint i = i_lo; i <= i_hi; i++) {
      _mark_index(p_nav, (guint)i);
   }
   _emit_changed(p_nav);
}

void
navigator_mark_all(Navigator *p_nav) {
   g_return_if_fail(GGAZE_IS_NAVIGATOR(p_nav));
   for (guint u_i = 0; u_i < p_nav->p_files->len; u_i++) {
      _mark_index(p_nav, u_i);
   }
   _emit_changed(p_nav);
}

void
navigator_clear_marks(Navigator *p_nav) {
   g_return_if_fail(GGAZE_IS_NAVIGATOR(p_nav));
   g_hash_table_remove_all(p_nav->p_marks);
   g_clear_object(&p_nav->p_last_mark);
   _emit_changed(p_nav);
}

guint
navigator_get_mark_count(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), 0);
   return (g_hash_table_size(p_nav->p_marks));
}

GList *
navigator_get_marks(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), NULL);
   GList *p_out  = NULL;
   GList *p_keys = g_hash_table_get_keys(p_nav->p_marks);
   for (GList *p_it = p_keys; p_it != NULL; p_it = p_it->next) {
      p_out = g_list_prepend(p_out, g_object_ref((GFile *)p_it->data));
   }
   g_list_free(p_keys);
   return (g_list_reverse(p_out));
}

/* The anchor set by the last `v` toggle-on, used by `V` range-mark. Borrowed
 * (owned by the navigator); NULL if no mark has been toggled on yet, or if the
 * anchor was unmarked / removed / cleared. Survives re-sort (path-based). */
GFile *
navigator_get_last_mark(Navigator *p_nav) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), NULL);
   return (p_nav->p_last_mark);
}

/* --- mutations ----------------------------------------------------------- */

void
navigator_rescan(Navigator *p_nav) {
   g_return_if_fail(GGAZE_IS_NAVIGATOR(p_nav));
   _relist(p_nav);
   _emit_changed(p_nav);
}

gboolean
navigator_remove(Navigator *p_nav, GFile *p_file) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), FALSE);
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   gint i = _find_index_by_file(p_nav, p_file);
   if (i < 0) {
      return (FALSE);
   }
   /* Clear its mark first (decision Q) while the file ref is still valid. */
   g_hash_table_remove(p_nav->p_marks, p_file);
   if (p_nav->p_last_mark != NULL && g_file_equal(p_nav->p_last_mark, p_file)) {
      g_clear_object(&p_nav->p_last_mark);
   }
   g_ptr_array_remove_index(p_nav->p_files, (guint)i);
   if (p_nav->p_files->len == 0) {
      p_nav->i_current = -1;
   } else if (p_nav->i_current == i) {
      /* Was current: fall back to nearest (stay at this index -> the next file,
       * or the new last if we removed the tail). */
      p_nav->i_current = MIN(i, (gint)p_nav->p_files->len - 1);
   } else if (p_nav->i_current > i) {
      p_nav->i_current--;
   }
   _emit_changed(p_nav);
   return (TRUE);
}

void
navigator_set_debounce_ms(Navigator *p_nav, guint u_ms) {
   g_return_if_fail(GGAZE_IS_NAVIGATOR(p_nav));
   p_nav->u_debounce_ms = u_ms;
}

void
navigator_emit_changed(Navigator *p_nav) {
   g_return_if_fail(GGAZE_IS_NAVIGATOR(p_nav));
   _emit_changed(p_nav);
}