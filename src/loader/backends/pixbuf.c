/*:*
 * ggaze — GdkPixbuf loader backend (fallback)
 *
 * Decodes any GdkPixbuf-supported format (PNG/JPEG/GIF/WebP/TIFF/ICO) via a
 * GdkPixbufLoader, applies the embedded EXIF Orientation (decision #26) so the
 * returned GdkTexture is upright, and hands the result to the caller. Acts as
 * the fallback backend: can_load() returns TRUE for unknown formats too (let
 * GdkPixbuf try) and FALSE only for formats owned by the JXL/AVIF/HEIF
 * backends in M5.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gio/gio.h>

#include "../detect.h"
#include "../loader.h"

static GdkTexture *_texture_from_pixbuf(GdkPixbuf *p_pix);

static gboolean
_pixbuf_can_load(const guint8 *p_head, gsize u_len) {
   switch (detect_format(p_head, u_len)) {
   case GGAZE_FMT_JXL:
   case GGAZE_FMT_AVIF:
   case GGAZE_FMT_HEIF:
      return (FALSE); /* owned by specific backends (M5) */
   case GGAZE_FMT_UNKNOWN:
   case GGAZE_FMT_JPEG:
   case GGAZE_FMT_PNG:
   case GGAZE_FMT_GIF:
   case GGAZE_FMT_WEBP:
   case GGAZE_FMT_TIFF:
   case GGAZE_FMT_ICO:
      return (TRUE);
   }
   return (FALSE); /* unreachable; keeps -Wreturn-type calm */
}

static GdkTexture *
_pixbuf_load(GFile *p_file, GCancellable *p_cancel, GError **p_err) {
   gchar *c_buf = NULL;
   gsize  u_len = 0;
   if (!g_file_load_contents(p_file, p_cancel, &c_buf, &u_len, NULL, p_err)) {
      return (NULL);
   }

   GdkPixbufLoader *p_loader = gdk_pixbuf_loader_new();
   GError          *p_sub    = NULL;
   if (!gdk_pixbuf_loader_write(p_loader, (const guchar *)c_buf, u_len,
                                &p_sub)) {
      g_propagate_error(p_err, p_sub);
      g_object_unref(p_loader);
      g_free(c_buf);
      return (NULL);
   }

   /* Close may fail on truncated data but a pixbuf may still be available. */
   if (!gdk_pixbuf_loader_close(p_loader, &p_sub)) {
      if (p_sub != NULL) {
         g_error_free(p_sub);
      }
   }

   GdkPixbuf *p_pix = gdk_pixbuf_loader_get_pixbuf(p_loader);
   if (p_pix == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "could not decode image (GdkPixbuf produced no pixbuf)");
      g_object_unref(p_loader);
      g_free(c_buf);
      return (NULL);
   }

   /* Honor EXIF Orientation so the texture is upright (decision #26). */
   GdkPixbuf *p_oriented = gdk_pixbuf_apply_embedded_orientation(p_pix);
   GdkPixbuf *p_use =
      (p_oriented != NULL) ? p_oriented : GDK_PIXBUF(g_object_ref(p_pix));

   GdkTexture *p_tex = _texture_from_pixbuf(p_use);

   g_object_unref(p_use);
   g_object_unref(p_loader);
   g_free(c_buf);
   return (p_tex);
}

const GgazeLoaderBackend pixbuf_backend = {
   .can_load = _pixbuf_can_load,
   .load     = _pixbuf_load,
};

/* Build a GdkTexture from a GdkPixbuf without the deprecated
 * gdk_texture_new_for_pixbuf(). GdkPixbuf stores non-premultiplied R8G8B8A8
 * when it has alpha; otherwise we add an alpha channel first. */
static GdkTexture *
_texture_from_pixbuf(GdkPixbuf *p_pix) {
   g_return_val_if_fail(GDK_IS_PIXBUF(p_pix), NULL);
   int i_w = gdk_pixbuf_get_width(p_pix);
   int i_h = gdk_pixbuf_get_height(p_pix);
   g_return_val_if_fail(i_w > 0 && i_h > 0, NULL);

   GdkPixbuf *p_rgba = gdk_pixbuf_get_has_alpha(p_pix)
                          ? GDK_PIXBUF(g_object_ref(p_pix))
                          : gdk_pixbuf_add_alpha(p_pix, FALSE, 0, 0, 0);
   if (p_rgba == NULL) {
      return (NULL);
   }

   int     i_rowstride = gdk_pixbuf_get_rowstride(p_rgba);
   guchar *p_pixels    = gdk_pixbuf_get_pixels(p_rgba);
   gsize   u_len   = (gsize)(i_h - 1) * (gsize)i_rowstride + (gsize)i_w * 4u;
   GBytes *p_bytes = g_bytes_new_with_free_func(
      p_pixels, u_len, (GDestroyNotify)g_object_unref, p_rgba);
   GdkTexture *p_tex = gdk_memory_texture_new(i_w, i_h, GDK_MEMORY_R8G8B8A8,
                                              p_bytes, (gsize)i_rowstride);
   g_bytes_unref(p_bytes);
   return (p_tex);
}