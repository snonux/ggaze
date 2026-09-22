/*:*
 * ggaze — libjpeg-turbo direct JPEG backend (progressive low-res preview)
 *
 * Two-phase decode: a quick 1/8-scale decode (coarse frame, <50 ms for a
 * 40 MP JPEG) is emitted via the progress callback, then the full decode
 * completes and is returned. Uses libjpeg-turbo's jpeg_mem_src + scale_num/
 * scale_denom for the low-res phase. Compiled when meson feature `jpeg` is
 * on; when it is off, pixbuf.c decodes JPEG through GdkPixbuf instead.
 *
 * Dispatch (loader.c): with `jpeg` on, jpeg_backend is in BACKENDS[] ahead of
 * the GdkPixbuf fallback and claims every JPEG, so BOTH the synchronous
 * loader_load() (enhancer, tests) and every async load go through this file
 * -- _jpeg_load() for sync/prefetch, _jpeg_load_progressive() for the
 * visible load with a progress callback. The pixbuf backend never sees a
 * JPEG in that build.
 *
 * Safety, libjpeg side: the low-res *direct* decode (_decode_at_scale())
 * treats libjpeg's reported dimensions as untrusted. A JPEG SOF marker can
 * declare up to 65535x65535 using only a few header bytes, so
 * _jpeg_check_dims() bounds width/height with detect_dims_within_bounds()
 * and computes the RGB/RGBA buffer sizes with checked guint64 arithmetic
 * BEFORE either g_malloc() call -- g_malloc() aborts the process on
 * failure. A bound violation yields a recoverable G_IO_ERROR, never an
 * abort. At 1/8 scale libjpeg's own JPEG_MAX_DIMENSION (65500) tops out at
 * 8188x8188 (~67M pixels), under the cap, so the guard is defense in depth
 * here; tests/test_loader_jpeg.c covers it directly.
 *
 * Safety, GdkPixbuf side: the full decode (_jpeg_full_decode_via_pixbuf(),
 * used by _jpeg_load() and phase 2 of the progressive load) goes through
 * gdk_pixbuf_new_from_file() for EXIF-orientation-aware decoding, which has
 * no bound of its own: gdk-pixbuf 2.44's JPEG loader (glycin) pre-allocates
 * a huge sparse memfd sized off the declared dimensions and stalls ~28s
 * before its internal 8 GB cap rejects the file (mu0 review) -- a reachable
 * unbounded-latency DoS. Both entry points therefore run
 * _jpeg_reject_if_oversized() on the file's real bytes first, using
 * detect_jpeg_peek_dims() (no decoder invoked).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gio/gio.h>
#include <jpeglib.h>
#include <setjmp.h>

#include "../detect.h"
#include "../loader.h"
#include "../pixbuf-util.h"

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
   /* Shared per-side / pixel-count caps + checked RGBA size. */
   if (!detect_dims_within_bounds("jpeg", (guint64)i_w, (guint64)i_h,
                                  p_rgba_len, p_err)) {
      return (FALSE);
   }
   /* Sides are capped, so the RGB product fits every 64-bit gsize; the check
    * stays for 32-bit portability. */
   guint64 u_rowstride = (guint64)i_w * 3u;
   guint64 u_rgb_len   = (guint64)i_h * u_rowstride;
   if (u_rgb_len > (guint64)G_MAXSIZE) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "jpeg: buffer size overflows gsize");
      return (FALSE);
   }
   *p_rowstride = (gsize)u_rowstride;
   *p_rgb_len   = (gsize)u_rgb_len;
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

static gboolean    _jpeg_reject_if_oversized(const guint8 *p_buf, gsize u_len,
                                             GError **p_err);
static GdkTexture *_jpeg_full_decode_via_pixbuf(GFile *p_file, GError **p_err);

static gboolean
_jpeg_can_load(const guint8 *p_head, gsize u_len) {
   return (detect_format(p_head, u_len) == GGAZE_FMT_JPEG);
}

static GdkTexture *
_jpeg_load(GFile *p_file, GCancellable *p_cancel, GError **p_err) {
   /* Sync / prefetch load: the orientation-aware GdkPixbuf path the
    * progressive backend uses for its full-decode phase, so a sync
    * loader_load() of a JPEG (e.g. the GEGL enhancer) is upright (decision
    * #26). The libjpeg _decode_at_scale() fast path is only used for the
    * progressive low-res preview, which does not need orientation. Reject
    * an oversized header before the GdkPixbuf call -- see
    * _jpeg_load_progressive(). */
   gchar *c_buf = NULL;
   gsize  u_len = 0;
   if (!g_file_load_contents(p_file, p_cancel, &c_buf, &u_len, NULL, p_err)) {
      return (NULL);
   }
   gboolean b_ok =
      _jpeg_reject_if_oversized((const guint8 *)c_buf, u_len, p_err);
   g_free(c_buf);
   if (!b_ok) {
      return (NULL);
   }
   /* A superseded prefetch/visible load stops here instead of paying for
    * the full decode it would only throw away. */
   if (g_cancellable_set_error_if_cancelled(p_cancel, p_err)) {
      return (NULL);
   }
   return (_jpeg_full_decode_via_pixbuf(p_file, p_err));
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
   GdkTexture *p_tex = pixbuf_util_to_upright_texture(p_pix);
   g_object_unref(p_pix);
   if (p_tex == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "jpeg: could not build texture from decoded pixels");
   }
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