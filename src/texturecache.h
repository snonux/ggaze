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

TextureCache *texturecache_new(guint u_cap);
void          texturecache_delete(TextureCache *p_cache);

/* Look up p_file; returns its GdkTexture (transfer none) or NULL, and marks it
 * most-recently-used. */
GdkTexture *texturecache_get(TextureCache *p_cache, GFile *p_file);

/* Store p_tex for p_file (refs both); evicts the least-recently-used entry if
 * the cache is over capacity. Replaces an existing entry. */
void texturecache_put(TextureCache *p_cache, GFile *p_file, GdkTexture *p_tex);

guint texturecache_get_size(TextureCache *p_cache);
void  texturecache_clear(TextureCache *p_cache);

G_END_DECLS

#endif /* GGAZE_TEXTURECACHE_H */