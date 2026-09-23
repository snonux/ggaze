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
 * on this guard is unreachable -- jpeg.c claims every JPEG at dispatch, and
 * one that arrives here anyway (the file changed after the sniff) is
 * refused by the fallback gate below before the guard runs -- so the full
 * build's coverage of it is honestly zero; the minimal lane (jpeg off) is
 * where it is exercised.
 *
 * What this backend cannot guard against on its own (task tb2): on a glycin
 * desktop (Fedora >= 41) gdk-pixbuf forwards formats it has no module for
 * (JXL/AVIF/HEIF/...) to sandboxed loader subprocesses, and
 * gdk_pixbuf_loader_close() then blocks in gly_loader_load() with no
 * cancellable, timeout or partial-result path. The glycin-jxl loader was
 * measured to wait forever on a truncated or garbage codestream of any
 * length. The dispatcher (loader.c) therefore refuses a file shorter than
 * its signature's minimum before this backend is reached and, without the
 * jxl feature, refuses every JXL outright (G_IO_ERROR_NOT_SUPPORTED). The
 * dispatcher's sniff is one open and this backend's read another, though,
 * so _pixbuf_load() runs the gate again on the buffer it is about to
 * decode -- as loader_sniff_bytes_for_fallback(), which adds the dispatch
 * rule: bytes a specific backend of this build claims are refused too,
 * since they can only be here because the file changed between the two
 * opens. That second rule is what makes the JXL guarantee exact rather
 * than build-dependent: without libjxl the not-built-in rule refuses a
 * JXL, with libjxl the plain gate would ADMIT one (the jxl backend decodes
 * complete ones) and a garbage JXL swapped in after the sniff would reach
 * the GdkPixbufLoader and hang glycin-jxl; the dispatch rule refuses it
 * there. So the GdkPixbufLoader never sees a JXL in any build, and the
 * residual glycin-jxl defect costs an error message rather than a hung
 * worker. The GCancellable is honoured at the two points it can be: the
 * read and the moment before the (uninterruptible) decode.
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

/* Everything that must be true of p_buf/u_len before a GdkPixbufLoader
 * may see it: the dispatcher's gate again (empty, truncated, JXL without
 * libjxl) plus the dispatch rule (nothing a specific backend of this build
 * claims -- with libjxl that is what keeps a JXL out) -- on THESE bytes,
 * so the check and the decode cannot be split by a swap of the file
 * between two opens (top-of-file comment) -- then the declared-size guard
 * for a JPEG. TRUE to decode. */
static gboolean
_pixbuf_bytes_decodable(const guint8 *p_buf, gsize u_len, GError **p_err) {
   if (!loader_sniff_bytes_for_fallback(p_buf, u_len, p_err)) {
      return (FALSE);
   }
   return (_pixbuf_reject_if_oversized_jpeg(p_buf, u_len, p_err));
}

static GdkTexture *
_pixbuf_load(GFile *p_file, GCancellable *p_cancel, GError **p_err) {
   gchar *c_buf = NULL;
   gsize  u_len = 0;
   if (!g_file_load_contents(p_file, p_cancel, &c_buf, &u_len, NULL, p_err)) {
      return (NULL);
   }
   if (!_pixbuf_bytes_decodable((const guint8 *)c_buf, u_len, p_err)) {
      g_free(c_buf);
      return (NULL);
   }
   /* Last chance to honour a superseded load: the GdkPixbufLoader below
    * cannot be interrupted once it runs (loader.h backend contract). A
    * cancel that lands after the read's own check -- while the last read()
    * is in flight -- is caught here and nowhere else. */
   if (g_cancellable_set_error_if_cancelled(p_cancel, p_err)) {
      g_free(c_buf);
      return (NULL);
   }

   GdkPixbuf *p_pix =
      pixbuf_util_decode_bytes((const guchar *)c_buf, u_len, p_err);
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
