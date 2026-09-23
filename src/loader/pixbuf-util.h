#ifndef GGAZE_PIXBUF_UTIL_H
#define GGAZE_PIXBUF_UTIL_H

/*:*
 * ggaze — GdkPixbuf -> GdkTexture helpers (shared by every pixbuf consumer)
 *
 * The pixbuf backend, the libjpeg backend's full-decode phase and the
 * thumbnail generator all end with the same two steps: apply the embedded
 * EXIF Orientation so the result is upright (decision #26) and wrap the
 * pixels in a GdkMemoryTexture without the deprecated
 * gdk_texture_new_for_pixbuf(). They used to carry three private copies of
 * that code, which had already begun to drift (one copy lacked the NULL
 * guards); this is the single home. The bytes -> GdkPixbuf decode through
 * a GdkPixbufLoader lives here too, since the pixbuf backend and the
 * thumbnail cache read both decode a buffer they gated first (task tb2).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <glib.h>

G_BEGIN_DECLS

/* Return an upright copy of p_pix (EXIF Orientation applied), or a new ref on
 * p_pix itself when it carries no orientation. Never returns NULL for a valid
 * pixbuf. (transfer full) */
GdkPixbuf *pixbuf_util_upright(GdkPixbuf *p_pix);

/* Wrap p_pix's pixels in a GdkMemoryTexture (R8G8B8A8, non-premultiplied).
 * Adds an alpha channel first when the source has none. The texture keeps
 * its own ref on the pixel owner, so the caller keeps ownership of p_pix.
 * Returns NULL for an empty pixbuf or when the alpha copy cannot be
 * allocated. (transfer full) */
GdkTexture *pixbuf_util_to_texture(GdkPixbuf *p_pix);

/* Convenience: pixbuf_util_upright + pixbuf_util_to_texture. */
GdkTexture *pixbuf_util_to_upright_texture(GdkPixbuf *p_pix);

/* Decode a whole file's bytes through a GdkPixbufLoader, so the caller
 * decodes exactly the buffer it inspected (no path is reopened). The
 * loader keeps the module's options (a PNG's tEXt chunks come back through
 * gdk_pixbuf_get_option() as "tEXt::Key", exactly as they do from
 * gdk_pixbuf_new_from_file()). Returns a new ref on the pixbuf, or NULL
 * with p_err set. Not orientation-applied. (transfer full) */
GdkPixbuf *pixbuf_util_decode_bytes(const guchar *p_buf, gsize u_len,
                                    GError **p_err);

G_END_DECLS

#endif /* GGAZE_PIXBUF_UTIL_H */
