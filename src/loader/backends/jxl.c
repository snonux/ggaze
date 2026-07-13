/*:*
 * ggaze — JPEG XL loader backend (libjxl)
 *
 * Decodes JPEG XL (codestream or container) via libjxl's JxlDecoder into RGBA
 * pixels, then wraps them in a GdkMemoryTexture. Honors EXIF orientation is
 * not needed for JXL (orientation is handled in the container). Compiled only
 * when meson feature `jxl` is enabled.
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
   JxlDecoderSubscribeEvents(p_dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);
   JxlDecoderSetInput(p_dec, (const uint8_t *)c_buf, u_len);
   JxlDecoderCloseInput(p_dec);

   JxlBasicInfo st_info;
   memset(&st_info, 0, sizeof(st_info));
   uint8_t    *p_pixels = NULL;
   size_t      u_w = 0, u_h = 0;
   GdkTexture *p_tex = NULL;

   for (;;) {
      JxlDecoderStatus e_st = JxlDecoderProcessInput(p_dec);
      if (e_st == JXL_DEC_ERROR) {
         g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "jxl: decode error");
         break;
      }
      if (e_st == JXL_DEC_SUCCESS) {
         /* Done — build the texture. */
         if (p_pixels != NULL) {
            GBytes *p_bytes = g_bytes_new_take(p_pixels, u_w * u_h * 4u);
            p_pixels        = NULL;
            p_tex =
               gdk_memory_texture_new((int)u_w, (int)u_h, GDK_MEMORY_R8G8B8A8,
                                      p_bytes, (gsize)u_w * 4u);
            g_bytes_unref(p_bytes);
         }
         break;
      }
      if (e_st == JXL_DEC_BASIC_INFO) {
         JxlDecoderGetBasicInfo(p_dec, &st_info);
         u_w                   = st_info.xsize;
         u_h                   = st_info.ysize;
         p_pixels              = g_malloc(u_w * u_h * 4u);
         JxlPixelFormat st_fmt = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
         JxlDecoderSetImageOutBuffer(p_dec, &st_fmt, p_pixels, u_w * u_h * 4u);
      } else if (e_st == JXL_DEC_FULL_IMAGE) {
         /* pixels are now filled; continue to SUCCESS */
      }
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