/*:*
 * ggaze — AVIF loader backend (libavif)
 *
 * Decodes AVIF via libavif. Compiled only when meson feature `avif` is enabled.
 * If libavif is absent, the heif backend (libheif) also handles AVIF (its
 * can_load accepts GGAZE_FMT_AVIF). This backend is preferred when present.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "../loader.h"
#include "../detect.h"

#include <avif/avif.h>
#include <gdk/gdk.h>
#include <gio/gio.h>

static gboolean
_avif_can_load(const guint8 *p_head, gsize u_len) {
   return (detect_format(p_head, u_len) == GGAZE_FMT_AVIF);
}

static GdkTexture *
_avif_load(GFile *p_file, GCancellable *p_cancel, GError **p_err) {
   (void)p_cancel;
   char *c_path = g_file_get_path(p_file);
   if (c_path == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "avif: non-local file");
      return (NULL);
   }

   avifDecoder *p_dec = avifDecoderCreate();
   avifResult   e_r   = avifDecoderSetIOFile(p_dec, c_path);
   g_free(c_path);
   if (e_r != AVIF_RESULT_OK) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "avif: %s",
                  avifResultToString(e_r));
      avifDecoderDestroy(p_dec);
      return (NULL);
   }
   e_r = avifDecoderParse(p_dec);
   if (e_r != AVIF_RESULT_OK) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "avif: %s",
                  avifResultToString(e_r));
      avifDecoderDestroy(p_dec);
      return (NULL);
   }
   e_r = avifDecoderNextImage(p_dec);
   if (e_r != AVIF_RESULT_OK) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "avif: %s",
                  avifResultToString(e_r));
      avifDecoderDestroy(p_dec);
      return (NULL);
   }

   /* p_dec->image is already an avifImage *, so pass it (not &p_dec->image)
    * to the RGB conversion APIs. avifRGBImageAllocatePixels and
    * avifImageYUVToRGB return avifResult and must be checked. */
   avifRGBImage st_rgb;
   avifRGBImageSetDefaults(&st_rgb, p_dec->image);
   st_rgb.format = AVIF_RGB_FORMAT_RGBA;
   st_rgb.depth  = 8;
   e_r           = avifRGBImageAllocatePixels(&st_rgb);
   if (e_r != AVIF_RESULT_OK) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "avif: %s",
                  avifResultToString(e_r));
      avifDecoderDestroy(p_dec);
      return (NULL);
   }
   e_r = avifImageYUVToRGB(p_dec->image, &st_rgb);
   if (e_r != AVIF_RESULT_OK) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "avif: %s",
                  avifResultToString(e_r));
      avifRGBImageFreePixels(&st_rgb);
      avifDecoderDestroy(p_dec);
      return (NULL);
   }

   /* Copy the pixels into a buffer the GdkMemoryTexture owns. libavif's
    * pixel buffer is freed below; a non-owning g_bytes_new_static would
    * leave the texture backed by freed memory (use-after-free). */
   int     i_w        = (int)st_rgb.width;
   int     i_h        = (int)st_rgb.height;
   gsize   u_rowbytes = (gsize)st_rgb.rowBytes;
   gsize   u_len      = u_rowbytes * (gsize)st_rgb.height;
   guint8 *p_own      = g_memdup2(st_rgb.pixels, u_len);
   avifRGBImageFreePixels(&st_rgb);
   avifDecoderDestroy(p_dec);

   GBytes     *p_bytes = g_bytes_new_take(p_own, u_len);
   GdkTexture *p_tex   = gdk_memory_texture_new(i_w, i_h, GDK_MEMORY_R8G8B8A8,
                                                p_bytes, u_rowbytes);
   g_bytes_unref(p_bytes);
   return (p_tex);
}

const GgazeLoaderBackend avif_backend = {
   .can_load = _avif_can_load,
   .load     = _avif_load,
};