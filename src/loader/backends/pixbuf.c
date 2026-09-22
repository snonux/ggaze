/*:*
 * ggaze — GdkPixbuf loader backend (fallback)
 *
 * Decodes any GdkPixbuf-supported format (PNG/GIF/WebP/TIFF/ICO, plus JPEG
 * when the libjpeg backend is not built) via a GdkPixbufLoader, applies the
 * embedded EXIF Orientation (decision #26) so the returned GdkTexture is
 * upright, and hands the result to the caller. It is the dispatcher's
 * explicit fallback (loader.c tries every format-specific backend first and
 * then this one unconditionally), so can_load() simply says yes: this file
 * never has to know which formats the optional backends own.
 *
 * JPEG-specific guard (mu0 review): in a build without the `jpeg` feature
 * this backend decodes JPEG, and GdkPixbufLoader was found to pre-allocate a
 * huge sparse memfd sized off a JPEG's declared-but-unvalidated SOF header
 * dimensions and stall ~28s before its own internal cap rejects an oversized
 * file (no crash, but an unbounded-latency DoS). _pixbuf_load() therefore
 * peeks a JPEG's declared dimensions with detect_jpeg_peek_dims() (no
 * decoder invoked) and rejects an oversized one up front, mirroring jpeg.c's
 * _jpeg_reject_if_oversized() for its own GdkPixbuf call site. With `jpeg`
 * on, jpeg.c claims every JPEG first and this guard is not reached.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gio/gio.h>

#include "../detect.h"
#include "../loader.h"
#include "../pixbuf-util.h"

/* Reject p_buf/u_len (the full file, already read into memory) if it is a
 * JPEG whose declared header dimensions exceed GGAZE_JPEG_MAX_SIDE/
 * GGAZE_JPEG_MAX_PIXELS, before handing it to GdkPixbufLoader. Non-JPEG
 * input and JPEGs with no parseable SOF marker pass through untouched (the
 * real loader below produces whatever error is appropriate). See this
 * file's top-of-file comment for why this check exists. */
static gboolean
_pixbuf_reject_if_oversized_jpeg(const guint8 *p_buf, gsize u_len,
                                 GError **p_err) {
   if (detect_format(p_buf, u_len) != GGAZE_FMT_JPEG) {
      return (TRUE);
   }
   guint32 u_w, u_h;
   if (!detect_jpeg_peek_dims(p_buf, u_len, &u_w, &u_h)) {
      return (TRUE);
   }
   return (detect_jpeg_dims_within_bounds(u_w, u_h, p_err));
}

/* The fallback accepts everything the specific backends did not claim --
 * including GGAZE_FMT_UNKNOWN, so GdkPixbuf gets to try (and to produce the
 * definitive error for) anything the sniffer does not recognise. */
static gboolean
_pixbuf_can_load(const guint8 *p_head, gsize u_len) {
   (void)p_head;
   (void)u_len;
   return (TRUE);
}

static GdkTexture *
_pixbuf_load(GFile *p_file, GCancellable *p_cancel, GError **p_err) {
   gchar *c_buf = NULL;
   gsize  u_len = 0;
   if (!g_file_load_contents(p_file, p_cancel, &c_buf, &u_len, NULL, p_err)) {
      return (NULL);
   }
   if (!_pixbuf_reject_if_oversized_jpeg((const guint8 *)c_buf, u_len, p_err)) {
      g_free(c_buf);
      return (NULL);
   }

   GdkPixbufLoader *p_loader = gdk_pixbuf_loader_new();
   GError          *p_sub    = NULL;
   if (!gdk_pixbuf_loader_write(p_loader, (const guchar *)c_buf, u_len,
                                &p_sub)) {
      g_propagate_error(p_err, p_sub);
      /* A loader must be closed before it is finalized or GdkPixbuf logs a
       * warning per corrupt file (fatal under G_DEBUG=fatal-warnings). The
       * close error is irrelevant here: the write error is the one reported.
       */
      gdk_pixbuf_loader_close(p_loader, NULL);
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
   GdkTexture *p_tex = pixbuf_util_to_upright_texture(p_pix);
   if (p_tex == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "could not build texture from decoded pixels");
   }
   g_object_unref(p_loader);
   g_free(c_buf);
   return (p_tex);
}

const GgazeLoaderBackend pixbuf_backend = {
   .can_load = _pixbuf_can_load,
   .load     = _pixbuf_load,
};
