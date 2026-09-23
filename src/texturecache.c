/*:*
 * ggaze — decoded-texture cache
 *
 * Bounded LRU (GFile -> GdkTexture) with O(1) get/put via a hash mapping keys
 * to GQueue nodes. Main-thread only. Each entry remembers the file's stamp
 * (mtime to the nanosecond, size, inode) as read BEFORE its decode started
 * and a get() re-checks it (one query), so a file rewritten in place is
 * decoded afresh instead of shown stale.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "texturecache.h"

#include <string.h>

/* The stamp (TextureStamp) identifies the file state a texture was decoded
 * from. The whole seconds of the mtime plus the size were the stamp once,
 * and a rewrite within the same second to the same byte count (a script's
 * output, a padded re-export) was served stale (fd2). The sub-second part
 * tells such rewrites apart wherever the filesystem records it -- to the
 * nanosecond (G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC, GLib >= 2.74, which
 * meson.build pins; the usec attribute is the same value truncated). The
 * resolution is not the precision, though: many kernels/filesystems take
 * mtimes from the coarse clock, which moves in 1-4 ms ticks, so a
 * same-size rewrite within one tick still reads as unchanged. On a
 * whole-second filesystem the sub-second part reads 0 at put and get
 * alike, so the seconds + size check stands on its own as before.
 *
 * The inode comes from the same query for free and tells an atomic
 * replace (an editor's write to a temp file and rename over the original)
 * from the same file. Where a backend reports an inode that is not stable
 * across queries (some FUSE mounts), the stamp differs on every get: that
 * costs a redundant decode (and in enhance-ctrl's _recheck_original a
 * re-render), never a wrong picture -- the check only ever errs toward
 * "changed".
 *
 * WHEN the stamp is taken matters as much as what it holds: before the
 * decode (texturecache_lookup() hands the caller the miss's stamp). Read
 * after it, a writer that finished while the decode still read the old
 * bytes would stamp the old pixels as the new file state for good.
 *
 * An entry is either STAMPED (a valid t_stamp, compared on every get),
 * UNKNOWN (put under a stamp that could not be read: never fresh) or
 * TRUSTED (put with no stamp at all: never compared). UNKNOWN is what a
 * decode gets whose pre-decode query failed -- the file was briefly
 * absent mid-rename. Trusting that entry (as the cache once did) would
 * make every later rewrite a stale hit, since nothing was ever recorded to
 * compare against; treating it as stale costs one redundant decode on the
 * next get, which then reads a real stamp. TRUSTED is only for a texture
 * whose file cannot be queried by design (a synthetic GFile in a unit
 * test): texturecache_put_stamped(NULL), or texturecache_put() on a file
 * that does not exist. */
typedef struct {
   GFile       *p_file;    /* owned ref */
   GdkTexture  *p_tex;     /* owned ref */
   GList       *p_link;    /* node in the order queue (MRU at tail) */
   TextureStamp t_stamp;   /* file state the texture was decoded from */
   gboolean     b_trusted; /* put with no stamp: fresh without a query */
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

/* The one query a stamp costs. */
static const char *const c_stamp_attributes = G_FILE_ATTRIBUTE_TIME_MODIFIED
   "," G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC "," G_FILE_ATTRIBUTE_STANDARD_SIZE
   "," G_FILE_ATTRIBUTE_UNIX_INODE;

/* Read p_file's stamp in one query; b_valid FALSE when the file cannot be
 * queried (a synthetic GFile in a unit test, a vanished file). An
 * attribute the backend does not provide reads as 0 -- consistently at put
 * and get, so it never tells two states apart and never makes them differ
 * either. */
static void
_stamp_read(GFile *p_file, TextureStamp *p_stamp) {
   GFileInfo *p_info = g_file_query_info(p_file, c_stamp_attributes,
                                         G_FILE_QUERY_INFO_NONE, NULL, NULL);
   memset(p_stamp, 0, sizeof(*p_stamp));
   if (p_info == NULL) {
      return;
   }
   p_stamp->b_valid = TRUE;
   p_stamp->u_mtime =
      g_file_info_get_attribute_uint64(p_info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
   p_stamp->u_nsec = g_file_info_get_attribute_uint32(
      p_info, G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC);
   p_stamp->i_size = g_file_info_get_size(p_info);
   p_stamp->u_inode =
      g_file_info_get_attribute_uint64(p_info, G_FILE_ATTRIBUTE_UNIX_INODE);
   g_object_unref(p_info);
}

/* Equal iff both are valid and every part matches: a file that can no
 * longer be queried is never equal to the state it was stamped in. */
static gboolean
_stamp_equal(const TextureStamp *p_a, const TextureStamp *p_b) {
   return (p_a->b_valid && p_b->b_valid && p_a->u_mtime == p_b->u_mtime &&
           p_a->u_nsec == p_b->u_nsec && p_a->i_size == p_b->i_size &&
           p_a->u_inode == p_b->u_inode);
}

/* TRUE iff the entry still describes the file on disk; p_now receives the
 * stamp read for the check (invalid when none was read). A TRUSTED entry
 * is fresh without a query. Any other is stale when the file now differs
 * from its stamp or cannot be queried any more (rewritten in place by an
 * external editor or script, replaced, removed) -- and always when the
 * stamp itself is invalid (UNKNOWN: _stamp_equal never matches it), so
 * the caller re-decodes under the stamp read here. */
static gboolean
_entry_is_fresh(const CacheEntry *p_e, TextureStamp *p_now) {
   memset(p_now, 0, sizeof(*p_now));
   if (p_e->b_trusted) {
      return (TRUE);
   }
   _stamp_read(p_e->p_file, p_now);
   return (_stamp_equal(p_now, &p_e->t_stamp));
}

/* Copy p_stamp into p_e (STAMPED, or UNKNOWN when it is invalid), or mark
 * the entry TRUSTED for NULL. */
static void
_entry_set_stamp(CacheEntry *p_e, const TextureStamp *p_stamp) {
   p_e->b_trusted = (p_stamp == NULL);
   if (p_stamp != NULL) {
      p_e->t_stamp = *p_stamp;
   } else {
      memset(&p_e->t_stamp, 0, sizeof(p_e->t_stamp));
   }
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
   TextureStamp t_unused;
   return (texturecache_lookup(p_cache, p_file, &t_unused));
}

GdkTexture *
texturecache_lookup(TextureCache *p_cache, GFile *p_file,
                    TextureStamp *p_stamp) {
   g_return_val_if_fail(p_stamp != NULL, NULL);
   memset(p_stamp, 0, sizeof(*p_stamp));
   g_return_val_if_fail(p_cache != NULL, NULL);
   CacheEntry *p_e = (CacheEntry *)g_hash_table_lookup(p_cache->p_map, p_file);
   if (p_e == NULL) {
      _stamp_read(p_file, p_stamp); /* the pre-decode stamp for the put */
      return (NULL);
   }
   if (!_entry_is_fresh(p_e, p_stamp)) {
      /* Stale: evict so the caller decodes the file as it is now (the `e`
       * external-edit and `!` script workflows rewrite files in place).
       * p_stamp already holds the file state that decode will start from. */
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
   g_return_if_fail(G_IS_FILE(p_file));
   /* The texture describes the file as it is now, so a file that cannot
    * be queried now is one that cannot be queried at all (a synthetic
    * GFile): store it TRUSTED rather than UNKNOWN, or it could never hit. */
   TextureStamp t_now;
   _stamp_read(p_file, &t_now);
   texturecache_put_stamped(p_cache, p_file, p_tex,
                            t_now.b_valid ? &t_now : NULL);
}

/* Evict LRU entries (queue head) while over capacity. */
static void
_evict_over_cap(TextureCache *p_cache) {
   while (g_queue_get_length(p_cache->p_order) > p_cache->u_cap) {
      GList      *p_head = g_queue_pop_head_link(p_cache->p_order);
      CacheEntry *p_old  = (CacheEntry *)p_head->data;
      /* Removing from the hash frees the entry (and its file/tex). The link is
       * freed by g_list_free below. */
      g_hash_table_remove(p_cache->p_map, p_old->p_file);
      g_list_free(p_head);
   }
}

void
texturecache_put_stamped(TextureCache *p_cache, GFile *p_file,
                         GdkTexture *p_tex, const TextureStamp *p_stamp) {
   g_return_if_fail(p_cache != NULL);
   g_return_if_fail(G_IS_FILE(p_file));
   g_return_if_fail(GDK_IS_TEXTURE(p_tex));

   CacheEntry *p_e = (CacheEntry *)g_hash_table_lookup(p_cache->p_map, p_file);
   if (p_e != NULL) {
      /* Replace the texture and its stamp; keep MRU position fresh. */
      g_set_object(&p_e->p_tex, p_tex);
      _entry_set_stamp(p_e, p_stamp);
      g_queue_unlink(p_cache->p_order, p_e->p_link);
      g_queue_push_tail_link(p_cache->p_order, p_e->p_link);
      return;
   }

   p_e               = g_new0(CacheEntry, 1);
   p_e->p_file       = (GFile *)g_object_ref(p_file);
   p_e->p_tex        = (GdkTexture *)g_object_ref(p_tex);
   p_e->p_link       = g_list_alloc();
   p_e->p_link->data = p_e;
   _entry_set_stamp(p_e, p_stamp);
   g_queue_push_tail_link(p_cache->p_order, p_e->p_link);
   g_hash_table_insert(p_cache->p_map, p_e->p_file, p_e);
   _evict_over_cap(p_cache);
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
