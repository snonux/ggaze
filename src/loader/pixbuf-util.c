/*:*
 * ggaze — GdkPixbuf -> GdkTexture helpers
 *
 * See pixbuf-util.h. Pure functions over GdkPixbuf; no display needed.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "pixbuf-util.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <glib.h>

GdkPixbuf *
pixbuf_util_upright(GdkPixbuf *p_pix) {
   g_return_val_if_fail(GDK_IS_PIXBUF(p_pix), NULL);
   /* apply_embedded_orientation returns NULL only on allocation failure;
    * an image without an orientation option comes back as a new ref. */
   GdkPixbuf *p_oriented = gdk_pixbuf_apply_embedded_orientation(p_pix);
   if (p_oriented != NULL) {
      return (p_oriented);
   }
   return (GDK_PIXBUF(g_object_ref(p_pix)));
}

GdkTexture *
pixbuf_util_to_texture(GdkPixbuf *p_pix) {
   g_return_val_if_fail(GDK_IS_PIXBUF(p_pix), NULL);
   int i_w = gdk_pixbuf_get_width(p_pix);
   int i_h = gdk_pixbuf_get_height(p_pix);
   if (i_w <= 0 || i_h <= 0) {
      return (NULL);
   }
   /* GdkPixbuf stores non-premultiplied R8G8B8A8 when it has alpha; add an
    * alpha channel otherwise. gdk_pixbuf_add_alpha uses g_try_malloc and
    * returns NULL when the copy cannot be allocated. */
   GdkPixbuf *p_rgba = gdk_pixbuf_get_has_alpha(p_pix)
                          ? GDK_PIXBUF(g_object_ref(p_pix))
                          : gdk_pixbuf_add_alpha(p_pix, FALSE, 0, 0, 0);
   if (p_rgba == NULL) {
      return (NULL);
   }
   int     i_rowstride = gdk_pixbuf_get_rowstride(p_rgba);
   guchar *p_pixels    = gdk_pixbuf_get_pixels(p_rgba);
   /* The last row need not be padded to the full rowstride. */
   gsize   u_len   = (gsize)(i_h - 1) * (gsize)i_rowstride + (gsize)i_w * 4u;
   GBytes *p_bytes = g_bytes_new_with_free_func(
      p_pixels, u_len, (GDestroyNotify)g_object_unref, p_rgba);
   GdkTexture *p_tex = gdk_memory_texture_new(i_w, i_h, GDK_MEMORY_R8G8B8A8,
                                              p_bytes, (gsize)i_rowstride);
   g_bytes_unref(p_bytes);
   return (p_tex);
}

GdkTexture *
pixbuf_util_to_upright_texture(GdkPixbuf *p_pix) {
   GdkPixbuf *p_up = pixbuf_util_upright(p_pix);
   if (p_up == NULL) {
      return (NULL);
   }
   GdkTexture *p_tex = pixbuf_util_to_texture(p_up);
   g_object_unref(p_up);
   return (p_tex);
}

GdkPixbuf *
pixbuf_util_decode_bytes(const guchar *p_buf, gsize u_len, GError **p_err) {
   /* A loader must be closed before it is finalized or GdkPixbuf logs a
    * warning per corrupt file (fatal under G_DEBUG=fatal-warnings), so
    * both exits close it; on the write-failure exit the close error is
    * irrelevant (the write error is the one reported), and a close that
    * fails on truncated data may still leave a usable pixbuf. */
   GdkPixbufLoader *p_loader = gdk_pixbuf_loader_new();
   GError          *p_sub    = NULL;
   if (!gdk_pixbuf_loader_write(p_loader, p_buf, u_len, &p_sub)) {
      g_propagate_error(p_err, p_sub);
      gdk_pixbuf_loader_close(p_loader, NULL);
      g_object_unref(p_loader);
      return (NULL);
   }
   if (!gdk_pixbuf_loader_close(p_loader, &p_sub)) {
      g_clear_error(&p_sub);
   }
   GdkPixbuf *p_pix = gdk_pixbuf_loader_get_pixbuf(p_loader);
   if (p_pix == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "could not decode image (GdkPixbuf produced no pixbuf)");
   } else {
      g_object_ref(p_pix);
   }
   g_object_unref(p_loader);
   return (p_pix);
}
