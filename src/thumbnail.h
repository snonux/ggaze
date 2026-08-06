#ifndef GGAZE_THUMBNAIL_H
#define GGAZE_THUMBNAIL_H

/*:*
 * ggaze — thumbnail cache
 *
 * freedesktop Thumbnail Managing Standard: ~/.cache/thumbnails/normal (128)
 * and large (256), plus a custom x-large bucket for >256 (decision #37/T).
 * Stores PNG with Thumb::URI/MTime/Size; verifies mtime before trust. Decodes
 * asynchronously in a GTask worker; the GdkTexture is returned on the main
 * thread. See docs/tech-stack.md "Thumbnail cache".
 *
 * The cache lives on disk, so it persists across ggaze runs and is shared with
 * other TMS-compliant apps: reopening a folder re-uses the PNGs written last
 * time instead of re-decoding every picture.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct Thumbnail Thumbnail;

Thumbnail *thumbnail_new(void);
void       thumbnail_delete(Thumbnail *p_t);

/* Asynchronously get a thumbnail for p_file at ~i_size px (the nearest TMS
 * bucket >= i_size is cached). Returns the GdkTexture via p_cb on the main
 * thread (transfer full in thumbnail_get_finish). */
void thumbnail_get_async(Thumbnail *p_t, GFile *p_file, int i_size,
                         GCancellable *p_cancel, GAsyncReadyCallback p_cb,
                         gpointer p_data);
GdkTexture *thumbnail_get_finish(Thumbnail *p_t, GAsyncResult *p_res,
                                 GError **p_err);

/* Absolute path of the on-disk cache entry ggaze uses for p_file at ~i_size px
 * (<cache>/thumbnails/<bucket>/<md5-of-uri>.png). Stateless -- no Thumbnail
 * instance needed. Public so tests can assert on the persisted entry itself
 * (present, re-used, re-written when stale) rather than re-deriving the md5
 * naming and drifting from it. Returns a newly allocated string (g_free). */
char *thumbnail_cache_path(GFile *p_file, int i_size);

G_END_DECLS

#endif /* GGAZE_THUMBNAIL_H */