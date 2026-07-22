/*:*
 * ggaze — JPEG XL loader backend (libjxl)
 *
 * Decodes JPEG XL (codestream or container) via libjxl's JxlDecoder into RGBA
 * pixels, then wraps them in a GdkMemoryTexture. Honors EXIF orientation is
 * not needed for JXL (orientation is handled in the container). Compiled only
 * when meson feature `jxl` is enabled.
 *
 * Safety: the decoder is treated as untrusted input. Declared xsize/ysize
 * (from JxlBasicInfo) are bounds-checked against per-side and total-pixel
 * caps BEFORE any allocation, the RGBA buffer size is computed with checked
 * 64-bit arithmetic, dims are confirmed to fit a gint (GdkMemoryTexture
 * takes gint), and every JxlDecoder* status is inspected — a malformed or
 * truncated codestream yields a recoverable GError, never an abort. See
 * AGENTS.md "Architecture invariants" and the decoder rule (bad files never
 * crash).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "../loader.h"
#include "../detect.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <jxl/decode.h>
#include <jxl/types.h>

/* Per-dimension and total-pixel caps. GdkMemoryTexture takes gint dims, so
 * the hard upper bound is G_MAXINT per side; we stay far below that to bound
 * memory and to reject malformed/huge metadata before allocating. 32768 per
 * side and 100M total pixels (~400 MB RGBA) comfortably admit real camera
 * images while preventing the size_t wrap / OOM-abort / gint-overflow cases
 * malicious JXL metadata could otherwise trigger. */
#define GGAZE_JXL_MAX_SIDE 32768
#define GGAZE_JXL_MAX_PIXELS 100000000ULL

/* Validate decoded dimensions and compute the RGBA buffer size with checked
 * arithmetic. On success stores the byte count in *p_bytes and returns TRUE;
 * on failure sets a recoverable GError and returns FALSE. */
static gboolean
_jxl_check_dims(uint32_t u_w, uint32_t u_h, gsize *p_bytes, GError **p_err) {
   if (u_w == 0 || u_h == 0) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "jxl: zero-sized image (%ux%u)", u_w, u_h);
      return (FALSE);
   }
   if (u_w > GGAZE_JXL_MAX_SIDE || u_h > GGAZE_JXL_MAX_SIDE) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "jxl: image too large (%ux%u, max %u per side)", u_w, u_h,
                  GGAZE_JXL_MAX_SIDE);
      return (FALSE);
   }
   /* GdkMemoryTexture width/height are gint; confirm the cast is safe. */
   if (u_w > (uint32_t)G_MAXINT || u_h > (uint32_t)G_MAXINT) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "jxl: dimensions exceed gint range (%ux%u)", u_w, u_h);
      return (FALSE);
   }
   guint64 u_pixels = (guint64)u_w * (guint64)u_h;
   if (u_pixels > GGAZE_JXL_MAX_PIXELS) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "jxl: pixel count too large (%llu, max %llu)",
                  (unsigned long long)u_pixels,
                  (unsigned long long)GGAZE_JXL_MAX_PIXELS);
      return (FALSE);
   }
   /* Checked w*h*4: 64-bit product, then confirm it fits gsize. */
   guint64 u_bytes = u_pixels * 4u;
   if (u_bytes > (guint64)G_MAXSIZE) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "jxl: buffer size overflows gsize (%llu)",
                  (unsigned long long)u_bytes);
      return (FALSE);
   }
   *p_bytes = (gsize)u_bytes;
   return (TRUE);
}

static gboolean
_jxl_can_load(const guint8 *p_head, gsize u_len) {
   return (detect_format(p_head, u_len) == GGAZE_FMT_JXL);
}

static GdkTexture *
_jxl_load(GFile *p_file, GCancellable *p_cancel, GError **p_err) {
   (void)p_cancel;
   gchar *c_buf = NULL;
   gsize  u_len = 0;
   if (!g_file_load_contents(p_file, NULL, &c_buf, &u_len, NULL, p_err)) {
      return (NULL);
   }

   JxlDecoder *p_dec = JxlDecoderCreate(NULL);
   if (p_dec == NULL) {
      g_free(c_buf);
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "jxl: cannot create decoder");
      return (NULL);
   }
   JxlDecoderStatus e_st =
      JxlDecoderSubscribeEvents(p_dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);
   if (e_st != JXL_DEC_SUCCESS) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "jxl: cannot subscribe events (%d)", (int)e_st);
      JxlDecoderDestroy(p_dec);
      g_free(c_buf);
      return (NULL);
   }
   e_st = JxlDecoderSetInput(p_dec, (const uint8_t *)c_buf, u_len);
   if (e_st != JXL_DEC_SUCCESS) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "jxl: cannot set input (status %d)", (int)e_st);
      JxlDecoderDestroy(p_dec);
      g_free(c_buf);
      return (NULL);
   }
   JxlDecoderCloseInput(p_dec);

   JxlBasicInfo st_info;
   memset(&st_info, 0, sizeof(st_info));
   uint8_t    *p_pixels = NULL;
   GdkTexture *p_tex    = NULL;
   gsize       u_bytes  = 0;
   gboolean    b_ok     = TRUE;

   for (;;) {
      e_st = JxlDecoderProcessInput(p_dec);
      if (e_st == JXL_DEC_ERROR || e_st == JXL_DEC_NEED_MORE_INPUT) {
         g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "jxl: decode error (status %d)", (int)e_st);
         b_ok = FALSE;
         break;
      }
      if (e_st == JXL_DEC_SUCCESS) {
         /* Done — build the texture from the decoded pixels. */
         break;
      }
      if (e_st == JXL_DEC_BASIC_INFO) {
         JxlDecoderStatus e_info = JxlDecoderGetBasicInfo(p_dec, &st_info);
         if (e_info != JXL_DEC_SUCCESS) {
            g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "jxl: cannot read basic info (status %d)", (int)e_info);
            b_ok = FALSE;
            break;
         }
         if (!_jxl_check_dims(st_info.xsize, st_info.ysize, &u_bytes, p_err)) {
            b_ok = FALSE;
            break;
         }
         p_pixels                = g_malloc(u_bytes);
         JxlPixelFormat   st_fmt = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
         JxlDecoderStatus e_out =
            JxlDecoderSetImageOutBuffer(p_dec, &st_fmt, p_pixels, u_bytes);
         if (e_out != JXL_DEC_SUCCESS) {
            g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "jxl: cannot set output buffer (status %d)",
                        (int)e_out);
            b_ok = FALSE;
            break;
         }
      } else if (e_st == JXL_DEC_FULL_IMAGE) {
         /* pixels are now filled; continue to SUCCESS */
      } else {
         /* Unexpected status — treat as a decode failure. */
         g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "jxl: unexpected decoder status %d", (int)e_st);
         b_ok = FALSE;
         break;
      }
   }

   if (b_ok && p_pixels != NULL) {
      /* p_pixels is now owned by the GBytes (g_bytes_new_take). */
      GBytes *p_bytes = g_bytes_new_take(p_pixels, u_bytes);
      p_pixels        = NULL;
      p_tex = gdk_memory_texture_new((int)st_info.xsize, (int)st_info.ysize,
                                     GDK_MEMORY_R8G8B8A8, p_bytes,
                                     (gsize)st_info.xsize * 4u);
      g_bytes_unref(p_bytes);
   }

   JxlDecoderDestroy(p_dec);
   g_free(p_pixels);
   g_free(c_buf);
   return (p_tex);
}

const GgazeLoaderBackend jxl_backend = {
   .can_load = _jxl_can_load,
   .load     = _jxl_load,
};