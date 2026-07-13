/*:*
 * ggaze — decoded-texture cache
 *
 * Bounded LRU (GFile -> GdkTexture) with O(1) get/put via a hash mapping keys
 * to GQueue nodes. Main-thread only.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "texturecache.h"

typedef struct {
   GFile      *file; /* owned ref */
   GdkTexture *tex;  /* owned ref */
   GList      *link; /* node in the order queue (MRU at tail) */
} CacheEntry;

struct TextureCache {
   guint       u_cap;
   GHashTable *p_map;   /* GFile* (owned) -> CacheEntry* (owned) */
   GQueue     *p_order; /* CacheEntry* MRU at tail */
};

static void
_entry_free(gpointer p_void) {
   CacheEntry *p_e = (CacheEntry *)p_void;
   g_clear_object(&p_e->file);
   g_clear_object(&p_e->tex);
   g_free(p_e);
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
   /* Mark most-recently-used: move to tail. */
   g_queue_unlink(p_cache->p_order, p_e->link);
   g_queue_push_tail_link(p_cache->p_order, p_e->link);
   return (p_e->tex);
}

void
texturecache_put(TextureCache *p_cache, GFile *p_file, GdkTexture *p_tex) {
   g_return_if_fail(p_cache != NULL);
   g_return_if_fail(G_IS_FILE(p_file));
   g_return_if_fail(GDK_IS_TEXTURE(p_tex));

   CacheEntry *p_e = (CacheEntry *)g_hash_table_lookup(p_cache->p_map, p_file);
   if (p_e != NULL) {
      /* Replace the texture; keep MRU position fresh. */
      g_set_object(&p_e->tex, p_tex);
      g_queue_unlink(p_cache->p_order, p_e->link);
      g_queue_push_tail_link(p_cache->p_order, p_e->link);
      return;
   }

   p_e             = g_new(CacheEntry, 1);
   p_e->file       = (GFile *)g_object_ref(p_file);
   p_e->tex        = (GdkTexture *)g_object_ref(p_tex);
   p_e->link       = g_list_alloc();
   p_e->link->data = p_e;
   g_queue_push_tail_link(p_cache->p_order, p_e->link);
   g_hash_table_insert(p_cache->p_map, p_e->file, p_e);

   /* Evict LRU (head) while over capacity. */
   while (g_queue_get_length(p_cache->p_order) > p_cache->u_cap) {
      GList      *p_head = g_queue_pop_head_link(p_cache->p_order);
      CacheEntry *p_old  = (CacheEntry *)p_head->data;
      /* Removing from the hash frees the entry (and its file/tex). The link is
       * freed by g_list_free below. */
      g_hash_table_remove(p_cache->p_map, p_old->file);
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