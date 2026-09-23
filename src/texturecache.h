#ifndef GGAZE_TEXTURECACHE_H
#define GGAZE_TEXTURECACHE_H

/*:*
 * ggaze — decoded-texture cache
 *
 * A bounded LRU of (GFile -> GdkTexture) used to make flipping between images
 * feel instant and to cap memory on large folders/huge images. Main-thread
 * only: prefetch loads complete on the main thread and put here; the viewer
 * reads from here. See docs/architecture.md "Concurrency model" + "Prefetch".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct TextureCache TextureCache;

/* The file state a texture was decoded from: mtime (whole seconds plus the
 * sub-second part, to the nanosecond where the filesystem records it),
 * byte count and inode, all from one query. b_valid is FALSE when the file
 * could not be queried (a synthetic GFile in a unit test, a file briefly
 * absent mid-rename); an entry put with such a stamp is never fresh, as
 * nothing recorded could tell a later rewrite. Callers only carry it from
 * a lookup to a put. */
typedef struct {
   gboolean b_valid;
   guint64  u_mtime; /* whole seconds */
   guint32  u_nsec;  /* sub-second part; 0 where not recorded */
   goffset  i_size;
   guint64  u_inode; /* 0 where the backend has none */
} TextureStamp;

TextureCache *texturecache_new(guint u_cap);
void          texturecache_delete(TextureCache *p_cache);

/* Look up p_file; returns its GdkTexture (transfer none) or NULL, and marks it
 * most-recently-used. An entry whose file changed on disk since its stamp
 * was taken (mtime, size or inode differ, or the file is gone) is evicted
 * and NULL returned, so in-place edits never show stale pixels.
 *
 * What the stamp cannot tell: the kernel stamps mtimes from its coarse
 * clock on many filesystems, which advances in 1-4 ms ticks (not every
 * nanosecond), so a same-size in-place rewrite within one tick -- or
 * within one second on a filesystem that keeps whole seconds only -- is
 * still a hit. An inode that is not stable across queries (some FUSE
 * mounts) only ever costs a redundant decode, never a wrong picture. */
GdkTexture *texturecache_get(TextureCache *p_cache, GFile *p_file);

/* texturecache_get() that also hands out the file's stamp as this lookup
 * read it (the freshness check's own query, or one query on a miss with
 * no entry), so a caller that goes on to decode p_file has the stamp from
 * BEFORE the decode at no extra query. p_stamp is written on every call;
 * it is only meaningful on a miss. b_valid is FALSE where nothing was read
 * (a hit on an entry put with a NULL stamp, or trusted by
 * texturecache_put() because the file could not be queried) and where the file
 * could not be queried; a put under that invalid stamp stores an entry the next
 * get treats as stale, so the vanished-then-recreated file is re-read. */
GdkTexture *texturecache_lookup(TextureCache *p_cache, GFile *p_file,
                                TextureStamp *p_stamp);

/* Drop p_file's entry if present. */
void texturecache_remove(TextureCache *p_cache, GFile *p_file);

/* Store p_tex for p_file (refs both) under p_stamp, the file state read
 * BEFORE the decode that produced p_tex started (texturecache_lookup()'s
 * miss stamp). A stamp taken after the decode could describe a rewrite
 * that finished while the decode still read the old bytes: the old pixels
 * would carry the new stamp and stay a hit forever. Taken before, such a
 * rewrite makes the next get miss -- a redundant decode at worst.
 *
 * An invalid stamp (the pre-decode query failed: the file was briefly
 * absent) stores an entry that is never fresh: the next get misses, reads
 * a real stamp and hands it out for the re-decode. Trusting it instead
 * would serve every later rewrite stale, with nothing to compare against.
 * NULL stores the entry trusted (fresh without a query): only for a
 * texture whose file cannot be queried by design, a synthetic GFile in a
 * unit test. Evicts the least-recently-used entry if the cache is over
 * capacity; replaces an existing entry. */
void texturecache_put_stamped(TextureCache *p_cache, GFile *p_file,
                              GdkTexture *p_tex, const TextureStamp *p_stamp);

/* texturecache_put_stamped() with the stamp read now: right only for a
 * texture known to describe the file as it is at this moment (the unit
 * tests' synthetic textures); a finished decode uses the stamp taken
 * before it started. A file that cannot be queried now is stored trusted
 * (NULL stamp): here that means a synthetic GFile, not a vanished one. */
void texturecache_put(TextureCache *p_cache, GFile *p_file, GdkTexture *p_tex);

guint texturecache_get_size(TextureCache *p_cache);
void  texturecache_clear(TextureCache *p_cache);

G_END_DECLS

#endif /* GGAZE_TEXTURECACHE_H */