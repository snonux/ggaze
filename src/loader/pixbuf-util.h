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
 * guards); this is the single home.
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

G_END_DECLS

#endif /* GGAZE_PIXBUF_UTIL_H */
