/*:*
 * ggaze — JPEG XL loader backend (libjxl)
 *
 * Decodes JPEG XL (codestream or container) via libjxl's JxlDecoder into RGBA
 * pixels, then wraps them in a GdkMemoryTexture. libjxl applies the
 * container's orientation itself by default, so the texture is upright --
 * but JxlBasicInfo's xsize/ysize are the PRE-orientation dimensions, so for
 * the transposing orientations (5-8) the texture is declared ysize x xsize.
 * Compiled only when meson feature `jxl` is enabled.
 *
 * Safety: the decoder is treated as untrusted input. Declared xsize/ysize
 * (from JxlBasicInfo) are bounds-checked with detect_dims_within_bounds()
 * BEFORE any allocation (per-side and total-pixel caps, checked 64-bit RGBA
 * size, dims confirmed to fit a gint), and every JxlDecoder* status is
 * inspected -- a malformed or truncated codestream yields a recoverable
 * GError, never an abort. See AGENTS.md "Architecture invariants" and the
 * decoder rule (bad files never crash).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <jxl/decode.h>
#include <jxl/types.h>
#include <string.h>

#include "../detect.h"
#include "../loader.h"

/* Decoder state shared by the helpers below: the basic info once it has
 * arrived, the output buffer (owned until handed to a GBytes) and its size,
 * and the oriented texture dimensions. */
typedef struct {
   JxlBasicInfo st_info;
   uint8_t     *p_pixels;
   gsize        u_bytes;
   int          i_tex_w; /* post-orientation width */
   int          i_tex_h; /* post-orientation height */
} JxlState;

static gboolean
_jxl_can_load(const guint8 *p_head, gsize u_len) {
   return (detect_format(p_head, u_len) == GGAZE_FMT_JXL);
}

/* Create the decoder, subscribe to the two events we need, and hand it the
 * whole file. Returns NULL with a GError on any libjxl failure. */
static JxlDecoder *
_jxl_decoder_setup(const gchar *c_buf, gsize u_len, GError **p_err) {
   JxlDecoder *p_dec = JxlDecoderCreate(NULL);
   if (p_dec == NULL) {
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
      return (NULL);
   }
   e_st = JxlDecoderSetInput(p_dec, (const uint8_t *)c_buf, u_len);
   if (e_st != JXL_DEC_SUCCESS) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "jxl: cannot set input (status %d)", (int)e_st);
      JxlDecoderDestroy(p_dec);
      return (NULL);
   }
   JxlDecoderCloseInput(p_dec);
   return (p_dec);
}

/* JXL_DEC_BASIC_INFO: read the header, bounds-check it, size the output
 * buffer (in the oriented layout) and register it with the decoder. */
static gboolean
_jxl_on_basic_info(JxlDecoder *p_dec, JxlState *p_st, GError **p_err) {
   JxlDecoderStatus e_info = JxlDecoderGetBasicInfo(p_dec, &p_st->st_info);
   if (e_info != JXL_DEC_SUCCESS) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "jxl: cannot read basic info (status %d)", (int)e_info);
      return (FALSE);
   }
   uint32_t u_w = p_st->st_info.xsize;
   uint32_t u_h = p_st->st_info.ysize;
   if (!detect_dims_within_bounds("jxl", u_w, u_h, &p_st->u_bytes, p_err)) {
      return (FALSE);
   }
   /* libjxl writes the image already rotated into our buffer (keep_orientation
    * is off by default); orientations 5-8 transpose, so swap the declared
    * dimensions or the rows are cut at the wrong width. */
   gboolean b_transposed   = p_st->st_info.orientation >= JXL_ORIENT_TRANSPOSE;
   p_st->i_tex_w           = (int)(b_transposed ? u_h : u_w);
   p_st->i_tex_h           = (int)(b_transposed ? u_w : u_h);
   p_st->p_pixels          = g_malloc(p_st->u_bytes);
   JxlPixelFormat   st_fmt = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
   JxlDecoderStatus e_out  = JxlDecoderSetImageOutBuffer(
      p_dec, &st_fmt, p_st->p_pixels, p_st->u_bytes);
   if (e_out != JXL_DEC_SUCCESS) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "jxl: cannot set output buffer (status %d)", (int)e_out);
      return (FALSE);
   }
   return (TRUE);
}

/* Drive the decoder to JXL_DEC_SUCCESS. Returns FALSE with a GError on a
 * decode error, truncated input, or any status we did not subscribe to. */
static gboolean
_jxl_process(JxlDecoder *p_dec, JxlState *p_st, GError **p_err) {
   for (;;) {
      JxlDecoderStatus e_st = JxlDecoderProcessInput(p_dec);
      switch (e_st) {
      case JXL_DEC_SUCCESS:
         return (p_st->p_pixels != NULL);
      case JXL_DEC_BASIC_INFO:
         if (!_jxl_on_basic_info(p_dec, p_st, p_err)) {
            return (FALSE);
         }
         break;
      case JXL_DEC_FULL_IMAGE:
         break; /* pixels are filled; continue to SUCCESS */
      case JXL_DEC_ERROR:
      case JXL_DEC_NEED_MORE_INPUT:
         g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "jxl: decode error (status %d)", (int)e_st);
         return (FALSE);
      default:
         g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "jxl: unexpected decoder status %d", (int)e_st);
         return (FALSE);
      }
   }
}

static GdkTexture *
_jxl_load(GFile *p_file, GCancellable *p_cancel, GError **p_err) {
   gchar *c_buf = NULL;
   gsize  u_len = 0;
   if (!g_file_load_contents(p_file, p_cancel, &c_buf, &u_len, NULL, p_err)) {
      return (NULL);
   }
   /* The decode is the expensive step: honour a cancel (superseded load,
    * prefetch round dropped) before paying for it. */
   if (g_cancellable_set_error_if_cancelled(p_cancel, p_err)) {
      g_free(c_buf);
      return (NULL);
   }
   JxlDecoder *p_dec = _jxl_decoder_setup(c_buf, u_len, p_err);
   if (p_dec == NULL) {
      g_free(c_buf);
      return (NULL);
   }
   JxlState st;
   memset(&st, 0, sizeof(st));
   GdkTexture *p_tex = NULL;
   if (_jxl_process(p_dec, &st, p_err)) {
      /* st.p_pixels is now owned by the GBytes (g_bytes_new_take). */
      GBytes *p_bytes = g_bytes_new_take(st.p_pixels, st.u_bytes);
      st.p_pixels     = NULL;
      p_tex =
         gdk_memory_texture_new(st.i_tex_w, st.i_tex_h, GDK_MEMORY_R8G8B8A8,
                                p_bytes, (gsize)st.i_tex_w * 4u);
      g_bytes_unref(p_bytes);
   }
   JxlDecoderDestroy(p_dec);
   g_free(st.p_pixels);
   g_free(c_buf);
   return (p_tex);
}

const GgazeLoaderBackend jxl_backend = {
   .can_load = _jxl_can_load,
   .load     = _jxl_load,
};
