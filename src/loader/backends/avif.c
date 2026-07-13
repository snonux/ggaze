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

   avifRGBImage st_rgb;
   avifRGBImageSetDefaults(&st_rgb, &p_dec->image);
   st_rgb.format = AVIF_RGB_FORMAT_RGBA;
   st_rgb.depth  = 8;
   avifRGBImageAllocatePixels(&st_rgb);
   avifImageYCToRGB(&p_dec->image, &st_rgb);

   GdkTexture *p_tex = NULL;
   if (st_rgb.pixels != NULL) {
      GBytes *p_bytes = g_bytes_new_static(
         st_rgb.pixels, (gsize)st_rgb.rowBytes * (gsize)st_rgb.height);
      p_tex = gdk_memory_texture_new((int)st_rgb.width, (int)st_rgb.height,
                                     GDK_MEMORY_R8G8B8A8, p_bytes,
                                     (gsize)st_rgb.rowBytes);
      g_bytes_unref(p_bytes);
   }

   avifRGBImageFreePixels(&st_rgb);
   avifDecoderDestroy(p_dec);
   return (p_tex);
}

const GgazeLoaderBackend avif_backend = {
   .can_load = _avif_can_load,
   .load     = _avif_load,
};