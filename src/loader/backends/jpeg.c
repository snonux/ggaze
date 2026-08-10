/*:*
 * ggaze — libjpeg-turbo direct JPEG backend (progressive low-res preview)
 *
 * Two-phase decode: a quick 1/8-scale decode (coarse frame, <50 ms for a
 * 40 MP JPEG) is emitted via the progress callback, then the full decode
 * completes and is returned. Uses libjpeg-turbo's jpeg_mem_src + scale_num/
 * scale_denom for the low-res phase. Compiled when meson feature `jpeg` is on.
 *
 * Safety, libjpeg side: both the low-res and full *direct* decode (i.e. via
 * this file's own _decode_at_scale(), used by _jpeg_load() and phase 1 of
 * _jpeg_load_progressive()) treat libjpeg's reported dimensions as
 * untrusted. A JPEG SOF marker can declare up to 65535x65535 using only a
 * few header bytes, with no actual pixel data required, so
 * _jpeg_check_dims() bounds width/height against detect.h's
 * GGAZE_JPEG_MAX_SIDE/GGAZE_JPEG_MAX_PIXELS and computes the RGB/RGBA buffer
 * sizes with checked guint64 arithmetic BEFORE either g_malloc() call —
 * g_malloc() aborts the process on failure, so an unbounded multi-gigabyte
 * request would crash ggaze rather than fail gracefully. A bound violation
 * yields a recoverable G_IO_ERROR (mirroring the checked-allocation pattern
 * already applied to heif.c and jxl.c), never an abort or overflowed loop
 * counter. NOTE: the loader dispatcher (loader.c) currently never calls
 * jpeg_backend.load() for a synchronous loader_load() (BACKENDS[] omits
 * jpeg_backend; sync JPEG loads fall through to pixbuf.c), so this guard is
 * presently reachable only via _jpeg_load_progressive()'s phase-1 low-res
 * decode -- real defense-in-depth, not dead code, but its libjpeg-side
 * bound can never actually trigger there because libjpeg's own
 * JPEG_MAX_DIMENSION (65500) at 1/8 scale tops out at 8188x8188 (~67M
 * pixels), already under the cap. See tests/test_loader_jpeg.c for the
 * direct-call coverage of this path regardless.
 *
 * Safety, GdkPixbuf side: phase 2 of _jpeg_load_progressive() (the full
 * decode users actually see when browsing, per window.c's _load_current)
 * hands off to gdk_pixbuf_new_from_file() for EXIF-orientation-aware
 * decoding, and pixbuf.c's fallback backend does the equivalent via
 * GdkPixbufLoader for every other reachable JPEG path (sync loader_load(),
 * prefetch). Neither goes through _decode_at_scale(), so the guard above
 * does not cover them. Empirically (mu0 review), gdk-pixbuf 2.44's JPEG
 * loader (glycin) does NOT abort/crash on a maximum-dimension declared
 * header -- it pre-allocates a huge sparse memfd sized off the declared
 * dimensions, then stalls for ~28s before its own internal size cap (8 GB)
 * rejects the file with a clean GError. That is a real, reachable
 * unbounded-latency DoS (not a crash), so both GdkPixbuf entry points are
 * now preceded by detect_jpeg_peek_dims() + the same GGAZE_JPEG_MAX_SIDE/
 * GGAZE_JPEG_MAX_PIXELS caps, applied to the file's real bytes without
 * invoking any decoder (see _jpeg_reject_if_oversized() below and its
 * twin in pixbuf.c).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "../loader.h"
#include "../detect.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#define GGAZE_JPEG_LORES_DENOM 8

struct _jerr_jmp {
   struct jpeg_error_mgr pub;
   jmp_buf               buf;
};

static void
_jerr_exit(j_common_ptr p_cinfo) {
   struct _jerr_jmp *p_ej = (struct _jerr_jmp *)p_cinfo->err;
   longjmp(p_ej->buf, 1);
}

/* Validate libjpeg's post-scale output dimensions and compute the RGB row
 * stride plus the RGB/RGBA buffer sizes with checked arithmetic. On success
 * stores the values in p_rowstride/p_rgb_len/p_rgba_len and returns TRUE; on
 * failure sets a recoverable GError and returns FALSE. Must run before
 * either g_malloc() in _decode_at_scale() below. */
static gboolean
_jpeg_check_dims(int i_w, int i_h, gsize *p_rowstride, gsize *p_rgb_len,
                 gsize *p_rgba_len, GError **p_err) {
   if (i_w <= 0 || i_h <= 0) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "jpeg: invalid dimensions (%dx%d)", i_w, i_h);
      return (FALSE);
   }
   if (i_w > GGAZE_JPEG_MAX_SIDE || i_h > GGAZE_JPEG_MAX_SIDE) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "jpeg: image too large (%dx%d, max %d per side)", i_w, i_h,
                  GGAZE_JPEG_MAX_SIDE);
      return (FALSE);
   }
   guint64 u_pixels = (guint64)i_w * (guint64)i_h;
   if (u_pixels > GGAZE_JPEG_MAX_PIXELS) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "jpeg: pixel count too large (%llu, max %llu)",
                  (unsigned long long)u_pixels,
                  (unsigned long long)GGAZE_JPEG_MAX_PIXELS);
      return (FALSE);
   }
   /* i_w/i_h are already capped at GGAZE_JPEG_MAX_SIDE, so these products
    * cannot approach G_MAXSIZE on any 64-bit build; the check is kept for
    * parity with heif.c/jxl.c and for 32-bit portability. */
   guint64 u_rowstride = (guint64)i_w * 3u;
   guint64 u_rgb_len   = (guint64)i_h * u_rowstride;
   guint64 u_rgba_len  = u_pixels * 4u;
   if (u_rgb_len > (guint64)G_MAXSIZE || u_rgba_len > (guint64)G_MAXSIZE) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "jpeg: buffer size overflows gsize");
      return (FALSE);
   }
   *p_rowstride = (gsize)u_rowstride;
   *p_rgb_len   = (gsize)u_rgb_len;
   *p_rgba_len  = (gsize)u_rgba_len;
   return (TRUE);
}

/* Convert planar RGB pixels to interleaved RGBA (alpha forced to 255).
 * u_pixels/u_rgba_len are the already-validated (_jpeg_check_dims) pixel
 * count and output byte length, so the index arithmetic below cannot
 * overflow. Caller frees the returned buffer. */
static guint8 *
_rgb_to_rgba(const guint8 *p_rgb, gsize u_pixels, gsize u_rgba_len) {
   guint8 *p_rgba = g_malloc(u_rgba_len);
   for (gsize u = 0; u < u_pixels; u++) {
      p_rgba[u * 4 + 0] = p_rgb[u * 3 + 0];
      p_rgba[u * 4 + 1] = p_rgb[u * 3 + 1];
      p_rgba[u * 4 + 2] = p_rgb[u * 3 + 2];
      p_rgba[u * 4 + 3] = 255;
   }
   return (p_rgba);
}

/* Decode JPEG data at the given scale denominator, returning RGBA pixels.
 * Caller frees *pp_pixels. Returns TRUE on success. Dimensions are bounded
 * and buffer sizes computed with checked arithmetic (_jpeg_check_dims)
 * before either allocation, so a maximum-dimension header is rejected with a
 * G_IO_ERROR instead of overflowing i_w*i_h or forcing a g_malloc abort. */
static gboolean
_decode_at_scale(const guint8 *p_data, gsize u_len, int i_denom, int *p_w,
                 int *p_h, guint8 **pp_pixels, GError **p_err) {
   struct jpeg_decompress_struct cinfo;
   struct _jerr_jmp              jerr;
   cinfo.err           = jpeg_std_error(&jerr.pub);
   jerr.pub.error_exit = _jerr_exit;
   /* p_rgb is allocated after setjmp and freed on the success path; on a
    * libjpeg longjmp during decode it must be freed here too, so it is
    * volatile (read after longjmp) and starts NULL (g_free(NULL) is safe on
    * the initial setjmp entry). */
   guint8 *volatile p_rgb = NULL;
   if (setjmp(jerr.buf)) {
      jpeg_destroy_decompress(&cinfo);
      g_free((gpointer)p_rgb);
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "jpeg: decode error");
      return (FALSE);
   }
   jpeg_create_decompress(&cinfo);
   jpeg_mem_src(&cinfo, (const unsigned char *)p_data, (unsigned long)u_len);
   jpeg_read_header(&cinfo, TRUE);
   cinfo.scale_num       = 1;
   cinfo.scale_denom     = (unsigned int)i_denom;
   cinfo.out_color_space = JCS_RGB;
   jpeg_start_decompress(&cinfo);

   int   i_w = (int)cinfo.output_width;
   int   i_h = (int)cinfo.output_height;
   gsize u_rowstride, u_rgb_len, u_rgba_len;
   if (!_jpeg_check_dims(i_w, i_h, &u_rowstride, &u_rgb_len, &u_rgba_len,
                         p_err)) {
      jpeg_destroy_decompress(&cinfo);
      return (FALSE);
   }
   p_rgb = g_malloc(u_rgb_len);

   while (cinfo.output_scanline < (JDIMENSION)i_h) {
      guint8 *p_rows[1] = {p_rgb + (gsize)cinfo.output_scanline * u_rowstride};
      jpeg_read_scanlines(&cinfo, p_rows, 1);
   }
   jpeg_finish_decompress(&cinfo);
   jpeg_destroy_decompress(&cinfo);

   gsize u_pixels = (gsize)i_w * (gsize)i_h;
   *pp_pixels     = _rgb_to_rgba(p_rgb, u_pixels, u_rgba_len);
   g_free(p_rgb);
   p_rgb = NULL;
   *p_w  = i_w;
   *p_h  = i_h;
   return (TRUE);
}

static GdkTexture *
_make_texture(int i_w, int i_h, guint8 *p_pixels) {
   GBytes *p_bytes   = g_bytes_new_take(p_pixels, (gsize)i_w * (gsize)i_h * 4u);
   GdkTexture *p_tex = gdk_memory_texture_new(i_w, i_h, GDK_MEMORY_R8G8B8A8,
                                              p_bytes, (gsize)i_w * 4u);
   g_bytes_unref(p_bytes);
   return (p_tex);
}

static gboolean
_jpeg_can_load(const guint8 *p_head, gsize u_len) {
   return (detect_format(p_head, u_len) == GGAZE_FMT_JPEG);
}

static GdkTexture *
_jpeg_load(GFile *p_file, GCancellable *p_cancel, GError **p_err) {
   (void)p_cancel;
   gchar *c_buf = NULL;
   gsize  u_len = 0;
   if (!g_file_load_contents(p_file, NULL, &c_buf, &u_len, NULL, p_err)) {
      return (NULL);
   }
   int     i_w, i_h;
   guint8 *p_pixels = NULL;
   if (!_decode_at_scale((const guint8 *)c_buf, u_len, 1, &i_w, &i_h, &p_pixels,
                         p_err)) {
      g_free(c_buf);
      return (NULL);
   }
   g_free(c_buf);
   return (_make_texture(i_w, i_h, p_pixels));
}

/* Reject p_buf/u_len (the full JPEG file, still in memory) if its declared
 * header dimensions exceed GGAZE_JPEG_MAX_SIDE/GGAZE_JPEG_MAX_PIXELS, using
 * detect_jpeg_peek_dims()'s dependency-free marker scan -- no decoder
 * invoked. Must run before handing the file to GdkPixbuf (see this file's
 * top-of-file "Safety, GdkPixbuf side" comment for why). Returns TRUE
 * (proceed) when no SOF marker is found or dimensions are within bounds, so
 * a malformed/truncated file still reaches the real decoder for its own
 * error; FALSE (with *p_err set to a G_IO_ERROR) when oversized. */
static gboolean
_jpeg_reject_if_oversized(const guint8 *p_buf, gsize u_len, GError **p_err) {
   guint32 u_w, u_h;
   if (!detect_jpeg_peek_dims(p_buf, u_len, &u_w, &u_h)) {
      return (TRUE);
   }
   return (detect_jpeg_dims_within_bounds(u_w, u_h, p_err));
}

/* Build a GdkTexture (RGBA) from an already-oriented GdkPixbuf. Adds an
 * alpha channel first if the source lacks one. Caller keeps ownership of
 * p_pix; the returned texture holds its own ref via p_rgba. */
static GdkTexture *
_jpeg_pixbuf_to_texture(GdkPixbuf *p_pix) {
   int        i_w         = gdk_pixbuf_get_width(p_pix);
   int        i_h         = gdk_pixbuf_get_height(p_pix);
   GdkPixbuf *p_rgba      = gdk_pixbuf_get_has_alpha(p_pix)
                               ? GDK_PIXBUF(g_object_ref(p_pix))
                               : gdk_pixbuf_add_alpha(p_pix, FALSE, 0, 0, 0);
   int        i_rowstride = gdk_pixbuf_get_rowstride(p_rgba);
   guchar    *p_px        = gdk_pixbuf_get_pixels(p_rgba);
   gsize      u_len   = (gsize)(i_h - 1) * (gsize)i_rowstride + (gsize)i_w * 4u;
   GBytes    *p_bytes = g_bytes_new_with_free_func(
      p_px, u_len, (GDestroyNotify)g_object_unref, p_rgba);
   GdkTexture *p_tex = gdk_memory_texture_new(i_w, i_h, GDK_MEMORY_R8G8B8A8,
                                              p_bytes, (gsize)i_rowstride);
   g_bytes_unref(p_bytes);
   return (p_tex);
}

/* Phase 2 of the progressive load: full decode via GdkPixbuf, which applies
 * EXIF orientation (something _decode_at_scale()/libjpeg does not do here).
 * Caller (_jpeg_load_progressive) must already have run
 * _jpeg_reject_if_oversized() on this file's bytes -- this function trusts
 * that check happened and does not repeat it. */
static GdkTexture *
_jpeg_full_decode_via_pixbuf(GFile *p_file, GError **p_err) {
   char *c_path = g_file_get_path(p_file);
   if (c_path == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "jpeg: non-local file");
      return (NULL);
   }
   GError    *p_sub = NULL;
   GdkPixbuf *p_pix = gdk_pixbuf_new_from_file(c_path, &p_sub);
   g_free(c_path);
   if (p_pix == NULL) {
      g_propagate_error(p_err, p_sub);
      return (NULL);
   }
   GdkPixbuf *p_oriented = gdk_pixbuf_apply_embedded_orientation(p_pix);
   GdkPixbuf *p_use =
      (p_oriented != NULL) ? p_oriented : GDK_PIXBUF(g_object_ref(p_pix));
   g_object_unref(p_pix);
   GdkTexture *p_tex = _jpeg_pixbuf_to_texture(p_use);
   g_object_unref(p_use);
   return (p_tex);
}

static GdkTexture *
_jpeg_load_progressive(GFile *p_file, GCancellable *p_cancel,
                       LoaderProgressCb p_progress, gpointer p_progress_data,
                       GError **p_err) {
   gchar *c_buf = NULL;
   gsize  u_len = 0;
   if (!g_file_load_contents(p_file, p_cancel, &c_buf, &u_len, NULL, p_err)) {
      return (NULL);
   }
   /* Reject an oversized declared header before either phase below touches
    * it: phase 1 already has its own libjpeg-driven bound (currently unable
    * to trigger at 1/8 scale, see top-of-file comment) but phase 2's
    * GdkPixbuf call has none of its own short of a ~28s internal stall. */
   if (!_jpeg_reject_if_oversized((const guint8 *)c_buf, u_len, p_err)) {
      g_free(c_buf);
      return (NULL);
   }
   /* Phase 1: low-res (1/8 scale). Skip once the load was superseded so a
    * stale partial cannot be emitted for a file that is no longer current. */
   int     i_lw, i_lh;
   guint8 *p_lp = NULL;
   if (!g_cancellable_is_cancelled(p_cancel) &&
       _decode_at_scale((const guint8 *)c_buf, u_len, GGAZE_JPEG_LORES_DENOM,
                        &i_lw, &i_lh, &p_lp, NULL)) {
      GdkTexture *p_partial = _make_texture(i_lw, i_lh, p_lp);
      if (!g_cancellable_is_cancelled(p_cancel) && p_progress != NULL) {
         p_progress(p_partial, p_progress_data);
      }
      g_object_unref(p_partial);
   }
   g_free(c_buf);
   /* Bail before the expensive full decode if the load was cancelled (e.g. by
    * a rapid navigation to a different file). The GTask reports the
    * cancellation; _load_finish_cb treats G_IO_ERROR_CANCELLED as benign. */
   if (g_cancellable_is_cancelled(p_cancel)) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                  "jpeg: progressive load cancelled");
      return (NULL);
   }
   return (_jpeg_full_decode_via_pixbuf(p_file, p_err));
}

const GgazeLoaderBackend jpeg_backend = {
   .can_load         = _jpeg_can_load,
   .load             = _jpeg_load,
   .load_progressive = _jpeg_load_progressive,
};