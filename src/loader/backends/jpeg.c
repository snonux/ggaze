/*:*
 * ggaze — libjpeg-turbo direct JPEG backend (progressive low-res preview)
 *
 * Two-phase decode: a quick 1/8-scale decode (coarse frame, <50 ms for a
 * 40 MP JPEG) is emitted via the progress callback, then the full decode
 * completes and is returned. Uses libjpeg-turbo's jpeg_mem_src + scale_num/
 * scale_denom for the low-res phase. Compiled when meson feature `jpeg` is on.
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

/* Decode JPEG data at the given scale denominator, returning RGBA pixels.
 * Caller frees *pp_pixels. Returns TRUE on success. */
static gboolean
_decode_at_scale(const guint8 *p_data, gsize u_len, int i_denom, int *p_w,
                 int *p_h, guint8 **pp_pixels, GError **p_err) {
   struct jpeg_decompress_struct cinfo;
   struct _jerr_jmp              jerr;
   cinfo.err           = jpeg_std_error(&jerr.pub);
   jerr.pub.error_exit = _jerr_exit;
   if (setjmp(jerr.buf)) {
      jpeg_destroy_decompress(&cinfo);
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

   int     i_w         = (int)cinfo.output_width;
   int     i_h         = (int)cinfo.output_height;
   int     i_rowstride = i_w * 3;
   guint8 *p_rgb       = g_malloc((gsize)i_h * (gsize)i_rowstride);

   while (cinfo.output_scanline < (JDIMENSION)i_h) {
      guint8 *p_rows[1] = {p_rgb + (gsize)cinfo.output_scanline * i_rowstride};
      jpeg_read_scanlines(&cinfo, p_rows, 1);
   }
   jpeg_finish_decompress(&cinfo);
   jpeg_destroy_decompress(&cinfo);

   /* Convert RGB → RGBA. */
   guint8 *p_rgba = g_malloc((gsize)i_w * (gsize)i_h * 4u);
   for (int i = 0; i < i_w * i_h; i++) {
      p_rgba[i * 4 + 0] = p_rgb[i * 3 + 0];
      p_rgba[i * 4 + 1] = p_rgb[i * 3 + 1];
      p_rgba[i * 4 + 2] = p_rgb[i * 3 + 2];
      p_rgba[i * 4 + 3] = 255;
   }
   g_free(p_rgb);
   *p_w       = i_w;
   *p_h       = i_h;
   *pp_pixels = p_rgba;
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

static GdkTexture *
_jpeg_load_progressive(GFile *p_file, GCancellable *p_cancel,
                       LoaderProgressCb p_progress, gpointer p_progress_data,
                       GError **p_err) {
   (void)p_cancel;
   gchar *c_buf = NULL;
   gsize  u_len = 0;
   if (!g_file_load_contents(p_file, NULL, &c_buf, &u_len, NULL, p_err)) {
      return (NULL);
   }
   /* Phase 1: low-res (1/8 scale). */
   int     i_lw, i_lh;
   guint8 *p_lp = NULL;
   if (_decode_at_scale((const guint8 *)c_buf, u_len, GGAZE_JPEG_LORES_DENOM,
                        &i_lw, &i_lh, &p_lp, NULL)) {
      GdkTexture *p_partial = _make_texture(i_lw, i_lh, p_lp);
      if (p_progress != NULL) {
         p_progress(p_partial, p_progress_data);
      }
      g_object_unref(p_partial);
   }
   /* Phase 2: full decode via GdkPixbuf (applies EXIF orientation). */
   char *c_path = g_file_get_path(p_file);
   g_free(c_buf);
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
   /* Convert to GdkTexture (RGBA). */
   int        i_w         = gdk_pixbuf_get_width(p_use);
   int        i_h         = gdk_pixbuf_get_height(p_use);
   GdkPixbuf *p_rgba      = gdk_pixbuf_get_has_alpha(p_use)
                               ? GDK_PIXBUF(g_object_ref(p_use))
                               : gdk_pixbuf_add_alpha(p_use, FALSE, 0, 0, 0);
   int        i_rowstride = gdk_pixbuf_get_rowstride(p_rgba);
   guchar    *p_px        = gdk_pixbuf_get_pixels(p_rgba);
   gsize      u_len2  = (gsize)(i_h - 1) * (gsize)i_rowstride + (gsize)i_w * 4u;
   GBytes    *p_bytes = g_bytes_new_with_free_func(
      p_px, u_len2, (GDestroyNotify)g_object_unref, p_rgba);
   GdkTexture *p_tex = gdk_memory_texture_new(i_w, i_h, GDK_MEMORY_R8G8B8A8,
                                              p_bytes, (gsize)i_rowstride);
   g_bytes_unref(p_bytes);
   g_object_unref(p_use);
   return (p_tex);
}

const GgazeLoaderBackend jpeg_backend = {
   .can_load         = _jpeg_can_load,
   .load             = _jpeg_load,
   .load_progressive = _jpeg_load_progressive,
};