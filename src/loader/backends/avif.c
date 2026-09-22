/*:*
 * ggaze — AVIF loader backend (libavif)
 *
 * Decodes AVIF via libavif. Compiled only when meson feature `avif` is enabled.
 * If libavif is absent, the heif backend (libheif) also handles AVIF (its
 * can_load accepts GGAZE_FMT_AVIF). This backend is preferred when present.
 *
 * Safety: the decoded image's width/height are bounds-checked with
 * detect_dims_within_bounds() before the RGBA conversion allocates anything
 * -- libavif's own default limits admit ~268 M pixels, i.e. a ~1 GB copy,
 * which is exactly the "bad file must not abort the process" class the other
 * backends guard against.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <avif/avif.h>
#include <gdk/gdk.h>
#include <gio/gio.h>

#include "../detect.h"
#include "../loader.h"

static gboolean
_avif_can_load(const guint8 *p_head, gsize u_len) {
   return (detect_format(p_head, u_len) == GGAZE_FMT_AVIF);
}

static void
_avif_set_error(GError **p_err, avifResult e_r) {
   g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "avif: %s",
               avifResultToString(e_r));
}

/* Open c_path, parse the container and decode the first image. Returns the
 * decoder (caller destroys) or NULL with a GError. */
static avifDecoder *
_avif_decode(const char *c_path, GError **p_err) {
   avifDecoder *p_dec = avifDecoderCreate();
   avifResult   e_r   = avifDecoderSetIOFile(p_dec, c_path);
   if (e_r == AVIF_RESULT_OK) {
      e_r = avifDecoderParse(p_dec);
   }
   if (e_r == AVIF_RESULT_OK) {
      e_r = avifDecoderNextImage(p_dec);
   }
   if (e_r != AVIF_RESULT_OK) {
      _avif_set_error(p_err, e_r);
      avifDecoderDestroy(p_dec);
      return (NULL);
   }
   return (p_dec);
}

/* Convert the decoded YUV image to RGBA and copy the pixels into a buffer
 * the GdkMemoryTexture owns. libavif's pixel buffer is freed here; a
 * non-owning g_bytes_new_static would leave the texture backed by freed
 * memory (use-after-free). */
static GdkTexture *
_avif_to_texture(avifDecoder *p_dec, GError **p_err) {
   /* p_dec->image is already an avifImage *, so pass it (not &p_dec->image)
    * to the RGB conversion APIs. */
   const avifImage *p_img = p_dec->image;
   if (!detect_dims_within_bounds("avif", p_img->width, p_img->height, NULL,
                                  p_err)) {
      return (NULL);
   }
   avifRGBImage st_rgb;
   avifRGBImageSetDefaults(&st_rgb, p_img);
   st_rgb.format  = AVIF_RGB_FORMAT_RGBA;
   st_rgb.depth   = 8;
   avifResult e_r = avifRGBImageAllocatePixels(&st_rgb);
   if (e_r != AVIF_RESULT_OK) {
      _avif_set_error(p_err, e_r);
      return (NULL);
   }
   e_r = avifImageYUVToRGB(p_img, &st_rgb);
   if (e_r != AVIF_RESULT_OK) {
      _avif_set_error(p_err, e_r);
      avifRGBImageFreePixels(&st_rgb);
      return (NULL);
   }
   int     i_w        = (int)st_rgb.width;
   int     i_h        = (int)st_rgb.height;
   gsize   u_rowbytes = (gsize)st_rgb.rowBytes;
   gsize   u_len      = u_rowbytes * (gsize)st_rgb.height;
   guint8 *p_own      = g_memdup2(st_rgb.pixels, u_len);
   avifRGBImageFreePixels(&st_rgb);

   GBytes     *p_bytes = g_bytes_new_take(p_own, u_len);
   GdkTexture *p_tex   = gdk_memory_texture_new(i_w, i_h, GDK_MEMORY_R8G8B8A8,
                                                p_bytes, u_rowbytes);
   g_bytes_unref(p_bytes);
   return (p_tex);
}

static GdkTexture *
_avif_load(GFile *p_file, GCancellable *p_cancel, GError **p_err) {
   char *c_path = g_file_get_path(p_file);
   if (c_path == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "avif: non-local file");
      return (NULL);
   }
   /* Honour a cancel before the expensive decode (superseded load). */
   if (g_cancellable_set_error_if_cancelled(p_cancel, p_err)) {
      g_free(c_path);
      return (NULL);
   }
   avifDecoder *p_dec = _avif_decode(c_path, p_err);
   g_free(c_path);
   if (p_dec == NULL) {
      return (NULL);
   }
   GdkTexture *p_tex = _avif_to_texture(p_dec, p_err);
   avifDecoderDestroy(p_dec);
   return (p_tex);
}

const GgazeLoaderBackend avif_backend = {
   .can_load = _avif_can_load,
   .load     = _avif_load,
};
