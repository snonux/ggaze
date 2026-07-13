/*:*
 * ggaze — HEIF/HEIC loader backend (libheif)
 *
 * Decodes HEIF/HEIC (and AVIF if libavif is absent) via libheif into RGBA
 * pixels, then wraps them in a GdkMemoryTexture. Compiled only when meson
 * feature `heif` is enabled.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "../loader.h"
#include "../detect.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <libheif/heif.h>

static gboolean
_heif_can_load(const guint8 *p_head, gsize u_len) {
   return (detect_format(p_head, u_len) == GGAZE_FMT_HEIF ||
           detect_format(p_head, u_len) == GGAZE_FMT_AVIF);
}

static GdkTexture *
_heif_load(GFile *p_file, GCancellable *p_cancel, GError **p_err) {
   (void)p_cancel;
   char *c_path = g_file_get_path(p_file);
   if (c_path == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "heif: non-local file");
      return (NULL);
   }

   struct heif_context *p_ctx = heif_context_alloc();
   struct heif_error st_err = heif_context_read_from_file(p_ctx, c_path, NULL);
   g_free(c_path);
   if (st_err.code != heif_error_Ok) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "heif: %s",
                  st_err.message);
      heif_context_free(p_ctx);
      return (NULL);
   }

   struct heif_image_handle *p_handle = NULL;
   st_err = heif_context_get_primary_image_handle(p_ctx, &p_handle);
   if (st_err.code != heif_error_Ok) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "heif: %s",
                  st_err.message);
      heif_context_free(p_ctx);
      return (NULL);
   }

   struct heif_image *p_img = NULL;
   st_err = heif_decode_image(p_handle, &p_img, heif_colorspace_RGB,
                              heif_chroma_interleaved_RGBA, NULL);
   if (st_err.code != heif_error_Ok) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "heif: %s",
                  st_err.message);
      heif_image_handle_release(p_handle);
      heif_context_free(p_ctx);
      return (NULL);
   }

   int            i_stride = 0;
   const uint8_t *p_data =
      heif_image_get_plane_readonly(p_img, heif_channel_interleaved, &i_stride);
   int i_w = heif_image_get_width(p_img, heif_channel_interleaved);
   int i_h = heif_image_get_height(p_img, heif_channel_interleaved);

   GdkTexture *p_tex = NULL;
   if (p_data != NULL && i_w > 0 && i_h > 0) {
      gsize   u_len   = (gsize)(i_h - 1) * (gsize)i_stride + (gsize)i_w * 4u;
      GBytes *p_bytes = g_bytes_new_static(p_data, u_len);
      p_tex = gdk_memory_texture_new(i_w, i_h, GDK_MEMORY_R8G8B8A8, p_bytes,
                                     (gsize)i_stride);
      g_bytes_unref(p_bytes);
   }

   heif_image_release(p_img);
   heif_image_handle_release(p_handle);
   heif_context_free(p_ctx);
   return (p_tex);
}

const GgazeLoaderBackend heif_backend = {
   .can_load = _heif_can_load,
   .load     = _heif_load,
};