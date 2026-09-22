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
 * What this backend cannot guard against on its own (task tb2): on a glycin
 * desktop (Fedora >= 41) gdk-pixbuf forwards formats it has no module for
 * (JXL/AVIF/HEIF/...) to sandboxed loader subprocesses, and
 * gdk_pixbuf_loader_close() then blocks in gly_loader_load() with no
 * cancellable, timeout or partial-result path. The glycin-jxl loader was
 * measured to wait forever on a truncated or garbage codestream of any
 * length. The dispatcher (loader.c) therefore refuses a file shorter than
 * its signature's minimum before this backend is reached and, without the
 * jxl feature, refuses every JXL outright (G_IO_ERROR_NOT_SUPPORTED): this
 * backend never sees a JXL in any build, so the residual glycin-jxl defect
 * costs a "not built in" message rather than a hung worker. The
 * GCancellable is honoured at the two points it can be: the read and the
 * moment before the (uninterruptible) decode.
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

/* Decode a whole file's bytes through a GdkPixbufLoader. Returns a new ref on
 * the pixbuf, or NULL with p_err. A loader must be closed before it is
 * finalized or GdkPixbuf logs a warning per corrupt file (fatal under
 * G_DEBUG=fatal-warnings), so both exits close it; on the write-failure exit
 * the close error is irrelevant (the write error is the one reported), and
 * a close that fails on truncated data may still leave a usable pixbuf. */
static GdkPixbuf *
_decode_bytes(const guchar *p_buf, gsize u_len, GError **p_err) {
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
   /* Last chance to honour a superseded load: the GdkPixbufLoader below
    * cannot be interrupted once it runs (loader.h backend contract). */
   if (g_cancellable_set_error_if_cancelled(p_cancel, p_err)) {
      g_free(c_buf);
      return (NULL);
   }

   GdkPixbuf *p_pix = _decode_bytes((const guchar *)c_buf, u_len, p_err);
   g_free(c_buf);
   if (p_pix == NULL) {
      return (NULL);
   }
   /* Honor EXIF Orientation so the texture is upright (decision #26). */
   GdkTexture *p_tex = pixbuf_util_to_upright_texture(p_pix);
   if (p_tex == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "could not build texture from decoded pixels");
   }
   g_object_unref(p_pix);
   return (p_tex);
}

const GgazeLoaderBackend pixbuf_backend = {
   .can_load = _pixbuf_can_load,
   .load     = _pixbuf_load,
};
