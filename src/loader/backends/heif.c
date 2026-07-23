/*:*
 * ggaze — HEIF/HEIC loader backend (libheif)
 *
 * Decodes HEIF/HEIC (and AVIF if libavif is absent) via libheif into RGBA
 * pixels, then wraps them in a GdkMemoryTexture. Compiled only when meson
 * feature `heif` is enabled.
 *
 * Safety: heif_image_get_plane_readonly() returns a pointer into a buffer
 * owned by the decoder's heif_image object. That buffer is only valid while
 * the heif_image is alive. The plane is copied into a buffer the GBytes
 * owns (mirrors the fix already applied to the sibling avif.c backend for
 * the identical use-after-free class) rather than freeing the heif_image
 * while a non-owning GBytes still points at its memory. Declared width/
 * height/stride are also validated before any arithmetic on them, guarding
 * against a zero, negative, or internally-inconsistent decoder result.
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

/* Validate the decoded plane's width/height/stride and compute the byte
 * length of the interleaved RGBA buffer with checked arithmetic. On success
 * stores the length in *p_len and returns TRUE; on failure sets a
 * recoverable GError and returns FALSE. Guards against a decoder returning
 * zero/negative dimensions, a zero/negative stride, or a stride too narrow
 * to hold one row of i_w RGBA pixels (any of which would make the plane-size
 * computation below read past the actual buffer). */
static gboolean
_heif_check_plane(int i_w, int i_h, int i_stride, gsize *p_len,
                  GError **p_err) {
   if (i_w <= 0 || i_h <= 0) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "heif: invalid dimensions (%dx%d)", i_w, i_h);
      return (FALSE);
   }
   if (i_stride <= 0) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "heif: invalid stride (%d)", i_stride);
      return (FALSE);
   }
   guint64 u_row_bytes = (guint64)i_w * 4u;
   if ((guint64)i_stride < u_row_bytes) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "heif: stride %d smaller than row width %llu bytes", i_stride,
                  (unsigned long long)u_row_bytes);
      return (FALSE);
   }
   guint64 u_len = (guint64)(i_h - 1) * (guint64)i_stride + u_row_bytes;
   if (u_len > (guint64)G_MAXSIZE) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "heif: buffer size overflows gsize (%llu)",
                  (unsigned long long)u_len);
      return (FALSE);
   }
   *p_len = (gsize)u_len;
   return (TRUE);
}

/* Open the file as a heif_context, fetch its primary image handle, and
 * decode it to interleaved RGB(A). On success pp_ctx, pp_handle, and pp_img
 * are all set and owned by the caller (release in that order: image,
 * handle, context). On failure sets a recoverable GError, releases whatever
 * it already opened, and returns FALSE. */
static gboolean
_heif_open_and_decode(const char *c_path, struct heif_context **pp_ctx,
                      struct heif_image_handle **pp_handle,
                      struct heif_image **pp_img, GError **p_err) {
   struct heif_context *p_ctx = heif_context_alloc();
   struct heif_error st_err = heif_context_read_from_file(p_ctx, c_path, NULL);
   if (st_err.code != heif_error_Ok) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "heif: %s",
                  st_err.message);
      heif_context_free(p_ctx);
      return (FALSE);
   }

   struct heif_image_handle *p_handle = NULL;
   st_err = heif_context_get_primary_image_handle(p_ctx, &p_handle);
   if (st_err.code != heif_error_Ok) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "heif: %s",
                  st_err.message);
      heif_context_free(p_ctx);
      return (FALSE);
   }

   struct heif_image *p_img = NULL;
   st_err = heif_decode_image(p_handle, &p_img, heif_colorspace_RGB,
                              heif_chroma_interleaved_RGBA, NULL);
   if (st_err.code != heif_error_Ok) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "heif: %s",
                  st_err.message);
      heif_image_handle_release(p_handle);
      heif_context_free(p_ctx);
      return (FALSE);
   }

   *pp_ctx    = p_ctx;
   *pp_handle = p_handle;
   *pp_img    = p_img;
   return (TRUE);
}

/* Extract the interleaved RGBA plane from a decoded heif_image, validate its
 * dimensions/stride, copy it into a buffer the returned texture's GBytes
 * owns, and build the texture. Unconditionally releases p_img/p_handle/p_ctx
 * (success or error) — ownership of the decoder's pixel buffer never crosses
 * out of this call, only the copy does. */
static GdkTexture *
_heif_build_texture(struct heif_context      *p_ctx,
                    struct heif_image_handle *p_handle,
                    struct heif_image *p_img, GError **p_err) {
   int            i_stride = 0;
   const uint8_t *p_data =
      heif_image_get_plane_readonly(p_img, heif_channel_interleaved, &i_stride);
   int i_w = heif_image_get_width(p_img, heif_channel_interleaved);
   int i_h = heif_image_get_height(p_img, heif_channel_interleaved);

   GdkTexture *p_tex = NULL;
   gsize       u_len = 0;
   if (p_data == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "heif: decoder returned no pixel data");
   } else if (_heif_check_plane(i_w, i_h, i_stride, &u_len, p_err)) {
      /* Copy before releasing p_img below: p_data points into memory owned
       * by p_img, so it must never be read (or referenced by a non-owning
       * GBytes) once p_img is released. */
      guint8 *p_own   = g_memdup2(p_data, u_len);
      GBytes *p_bytes = g_bytes_new_take(p_own, u_len);
      p_tex = gdk_memory_texture_new(i_w, i_h, GDK_MEMORY_R8G8B8A8, p_bytes,
                                     (gsize)i_stride);
      g_bytes_unref(p_bytes);
   }

   heif_image_release(p_img);
   heif_image_handle_release(p_handle);
   heif_context_free(p_ctx);
   return (p_tex);
}

static GdkTexture *
_heif_load(GFile *p_file, GCancellable *p_cancel, GError **p_err) {
   (void)p_cancel;
   char *c_path = g_file_get_path(p_file);
   if (c_path == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED, "heif: non-local file");
      return (NULL);
   }

   struct heif_context      *p_ctx    = NULL;
   struct heif_image_handle *p_handle = NULL;
   struct heif_image        *p_img    = NULL;
   gboolean                  b_ok =
      _heif_open_and_decode(c_path, &p_ctx, &p_handle, &p_img, p_err);
   g_free(c_path);
   if (!b_ok) {
      return (NULL);
   }

   return (_heif_build_texture(p_ctx, p_handle, p_img, p_err));
}

const GgazeLoaderBackend heif_backend = {
   .can_load = _heif_can_load,
   .load     = _heif_load,
};
