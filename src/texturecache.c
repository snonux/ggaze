/*:*
 * ggaze — decoded-texture cache
 *
 * Bounded LRU (GFile -> GdkTexture) with O(1) get/put via a hash mapping keys
 * to GQueue nodes. Main-thread only. Each entry remembers the file's
 * mtime/size at put time and a get() re-checks them (one stat), so a file
 * rewritten in place is decoded afresh instead of shown stale.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "texturecache.h"

typedef struct {
   GFile      *p_file;    /* owned ref */
   GdkTexture *p_tex;     /* owned ref */
   GList      *p_link;    /* node in the order queue (MRU at tail) */
   gboolean    b_stamped; /* mtime/size below were readable at put time */
   guint64     u_mtime;   /* file state the texture was decoded from */
   goffset     i_size;
} CacheEntry;

struct TextureCache {
   guint       u_cap;
   GHashTable *p_map;   /* GFile* (owned) -> CacheEntry* (owned) */
   GQueue     *p_order; /* CacheEntry* MRU at tail */
};

static void
_entry_free(gpointer p_void) {
   CacheEntry *p_e = (CacheEntry *)p_void;
   g_clear_object(&p_e->p_file);
   g_clear_object(&p_e->p_tex);
   g_free(p_e);
}

/* Read p_file's mtime and size. FALSE when the file cannot be queried (a
 * synthetic GFile in a unit test, a vanished file). */
static gboolean
_stat(GFile *p_file, guint64 *p_mtime, goffset *p_size) {
   GFileInfo *p_info = g_file_query_info(
      p_file, G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
      G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (p_info == NULL) {
      return (FALSE);
   }
   *p_mtime =
      g_file_info_get_attribute_uint64(p_info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
   *p_size = g_file_info_get_size(p_info);
   g_object_unref(p_info);
   return (TRUE);
}

/* TRUE iff the entry still describes the file on disk. An entry whose file
 * could not be stat'ed at put time is trusted (nothing to compare against);
 * one that was stamped but now differs -- or cannot be queried any more --
 * is stale: the picture was rewritten in place (external editor, script)
 * or removed. */
static gboolean
_entry_is_fresh(const CacheEntry *p_e) {
   if (!p_e->b_stamped) {
      return (TRUE);
   }
   guint64 u_mtime = 0;
   goffset i_size  = 0;
   if (!_stat(p_e->p_file, &u_mtime, &i_size)) {
      return (FALSE);
   }
   return (u_mtime == p_e->u_mtime && i_size == p_e->i_size);
}

TextureCache *
texturecache_new(guint u_cap) {
   TextureCache *p_c = g_new(TextureCache, 1);
   p_c->u_cap        = (u_cap == 0) ? 1 : u_cap;
   p_c->p_map =
      g_hash_table_new_full((GHashFunc)g_file_hash, (GEqualFunc)g_file_equal,
                            NULL, _entry_free); /* entry owns the key */
   p_c->p_order = g_queue_new();
   return (p_c);
}

void
texturecache_delete(TextureCache *p_cache) {
   if (p_cache == NULL) {
      return;
   }
   /* Clearing the hash frees entries (which are not in the queue order list as
    * separate refs — the queue holds the same pointers, so free the queue list
    * itself without touching the data). */
   g_queue_free(p_cache->p_order);
   g_hash_table_unref(p_cache->p_map);
   g_free(p_cache);
}

GdkTexture *
texturecache_get(TextureCache *p_cache, GFile *p_file) {
   g_return_val_if_fail(p_cache != NULL, NULL);
   CacheEntry *p_e = (CacheEntry *)g_hash_table_lookup(p_cache->p_map, p_file);
   if (p_e == NULL) {
      return (NULL);
   }
   if (!_entry_is_fresh(p_e)) {
      /* Stale: evict so the caller decodes the file as it is now (the `e`
       * external-edit and `!` script workflows rewrite files in place). */
      texturecache_remove(p_cache, p_file);
      return (NULL);
   }
   /* Mark most-recently-used: move to tail. */
   g_queue_unlink(p_cache->p_order, p_e->p_link);
   g_queue_push_tail_link(p_cache->p_order, p_e->p_link);
   return (p_e->p_tex);
}

void
texturecache_remove(TextureCache *p_cache, GFile *p_file) {
   g_return_if_fail(p_cache != NULL);
   CacheEntry *p_e = (CacheEntry *)g_hash_table_lookup(p_cache->p_map, p_file);
   if (p_e == NULL) {
      return;
   }
   g_queue_unlink(p_cache->p_order, p_e->p_link);
   g_list_free(p_e->p_link);
   g_hash_table_remove(p_cache->p_map, p_file); /* frees the entry */
}

void
texturecache_put(TextureCache *p_cache, GFile *p_file, GdkTexture *p_tex) {
   g_return_if_fail(p_cache != NULL);
   g_return_if_fail(G_IS_FILE(p_file));
   g_return_if_fail(GDK_IS_TEXTURE(p_tex));

   CacheEntry *p_e = (CacheEntry *)g_hash_table_lookup(p_cache->p_map, p_file);
   if (p_e != NULL) {
      /* Replace the texture and re-stamp; keep MRU position fresh. */
      g_set_object(&p_e->p_tex, p_tex);
      p_e->b_stamped = _stat(p_file, &p_e->u_mtime, &p_e->i_size);
      g_queue_unlink(p_cache->p_order, p_e->p_link);
      g_queue_push_tail_link(p_cache->p_order, p_e->p_link);
      return;
   }

   p_e               = g_new0(CacheEntry, 1);
   p_e->p_file       = (GFile *)g_object_ref(p_file);
   p_e->p_tex        = (GdkTexture *)g_object_ref(p_tex);
   p_e->b_stamped    = _stat(p_file, &p_e->u_mtime, &p_e->i_size);
   p_e->p_link       = g_list_alloc();
   p_e->p_link->data = p_e;
   g_queue_push_tail_link(p_cache->p_order, p_e->p_link);
   g_hash_table_insert(p_cache->p_map, p_e->p_file, p_e);

   /* Evict LRU (head) while over capacity. */
   while (g_queue_get_length(p_cache->p_order) > p_cache->u_cap) {
      GList      *p_head = g_queue_pop_head_link(p_cache->p_order);
      CacheEntry *p_old  = (CacheEntry *)p_head->data;
      /* Removing from the hash frees the entry (and its file/tex). The link is
       * freed by g_list_free below. */
      g_hash_table_remove(p_cache->p_map, p_old->p_file);
      g_list_free(p_head);
   }
}

guint
texturecache_get_size(TextureCache *p_cache) {
   g_return_val_if_fail(p_cache != NULL, 0);
   return ((guint)g_queue_get_length(p_cache->p_order));
}

void
texturecache_clear(TextureCache *p_cache) {
   g_return_if_fail(p_cache != NULL);
   /* Removing all hash entries frees the CacheEntry structs; the queue list
    * nodes are cleared without freeing the data again. */
   g_queue_clear(p_cache->p_order);
   g_hash_table_remove_all(p_cache->p_map);
}