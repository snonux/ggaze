/*:*
 * ggaze — RGB + luminance histogram builder
 *
 * See histogram.h. Pure binning over an 8-bit pixel buffer plus a
 * GdkMemoryTexture front end that reaches the pixels copy-free where GDK
 * allows and through a bounded conversion otherwise. No GtkWidget, no
 * display.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "histogram.h"

#include <gdk/gdk.h>
#include <glib.h>
#include <limits.h>

/* Byte offsets of R, G, B inside one pixel plus its size, per layout. The
 * premultiplied variants bin exactly like their straight siblings: the card
 * plots what is on screen, and a photo is opaque anyway. Returns FALSE for
 * every layout this table does not know (16-bit, float, grayscale, ...) so
 * the caller fails cleanly instead of reading garbage. */
static gboolean
_layout(GdkMemoryFormat e_format, guint *p_bpp, guint *p_r, guint *p_g,
        guint *p_b) {
   switch (e_format) {
   case GDK_MEMORY_R8G8B8A8:
   case GDK_MEMORY_R8G8B8A8_PREMULTIPLIED:
      *p_bpp = 4;
      *p_r   = 0;
      *p_g   = 1;
      *p_b   = 2;
      return (TRUE);
   case GDK_MEMORY_B8G8R8A8:
   case GDK_MEMORY_B8G8R8A8_PREMULTIPLIED:
      *p_bpp = 4;
      *p_r   = 2;
      *p_g   = 1;
      *p_b   = 0;
      return (TRUE);
   case GDK_MEMORY_A8R8G8B8:
   case GDK_MEMORY_A8R8G8B8_PREMULTIPLIED:
      *p_bpp = 4;
      *p_r   = 1;
      *p_g   = 2;
      *p_b   = 3;
      return (TRUE);
   case GDK_MEMORY_R8G8B8:
      *p_bpp = 3;
      *p_r   = 0;
      *p_g   = 1;
      *p_b   = 2;
      return (TRUE);
   case GDK_MEMORY_B8G8R8:
      *p_bpp = 3;
      *p_r   = 2;
      *p_g   = 1;
      *p_b   = 0;
      return (TRUE);
   default:
      return (FALSE);
   }
}

Histogram *
histogram_new(void) {
   Histogram *p_hist = g_new0(Histogram, 1);
   p_hist->u_step    = 1;
   return (p_hist);
}

void
histogram_delete(Histogram *p_hist) {
   g_free(p_hist);
}

guint
histogram_step_for(int i_width, int i_height) {
   if (i_width <= 0 || i_height <= 0) {
      return (1);
   }
   /* Smallest stride whose regular subsample fits the budget. The loop is
    * bounded: at the loader's 32768-per-side cap it ends by step 64. */
   guint u_step = 1;
   for (;;) {
      guint64 u_cols = ((guint64)i_width + u_step - 1) / u_step;
      guint64 u_rows = ((guint64)i_height + u_step - 1) / u_step;
      if (u_cols * u_rows <= HISTOGRAM_MAX_SAMPLES) {
         return (u_step);
      }
      u_step++;
   }
}

/* Add one pixel to the four channel histograms. Luminance is Rec.709 with
 * integer weights that sum to exactly 256 (54 + 183 + 19), so the >> 8
 * cannot overshoot 255. The bin index maps 0..255 onto HISTOGRAM_BINS. */
static void
_bin_pixel(Histogram *p_hist, guint8 u_r, guint8 u_g, guint8 u_b) {
   guint u_lum = (54u * u_r + 183u * u_g + 19u * u_b + 128u) >> 8;
   p_hist->u_bins[HISTOGRAM_CHANNEL_R][u_r * HISTOGRAM_BINS / 256]++;
   p_hist->u_bins[HISTOGRAM_CHANNEL_G][u_g * HISTOGRAM_BINS / 256]++;
   p_hist->u_bins[HISTOGRAM_CHANNEL_B][u_b * HISTOGRAM_BINS / 256]++;
   p_hist->u_bins[HISTOGRAM_CHANNEL_LUM][u_lum * HISTOGRAM_BINS / 256]++;
   p_hist->u_samples++;
}

/* The plot scale: the fullest bin over every channel, so the four curves
 * share one vertical axis and stay comparable to each other. */
static void
_find_peak(Histogram *p_hist) {
   p_hist->u_peak = 0;
   for (guint u_ch = 0; u_ch < HISTOGRAM_CHANNEL_COUNT; u_ch++) {
      for (guint u = 0; u < HISTOGRAM_BINS; u++) {
         if (p_hist->u_bins[u_ch][u] > p_hist->u_peak) {
            p_hist->u_peak = p_hist->u_bins[u_ch][u];
         }
      }
   }
}

Histogram *
histogram_new_from_pixels(const guint8 *p_pixels, int i_width, int i_height,
                          gsize u_stride, GdkMemoryFormat e_format,
                          guint u_step) {
   guint u_bpp, u_r, u_g, u_b;
   if (p_pixels == NULL || i_width <= 0 || i_height <= 0 ||
       !_layout(e_format, &u_bpp, &u_r, &u_g, &u_b) ||
       u_stride < (gsize)i_width * u_bpp) {
      return (NULL);
   }
   /* The stride is clamped to INT_MAX and the loop counters are unsigned
    * and at least as wide as the stride, so a huge step can neither turn
    * negative (which once walked the loop backwards) nor overflow the sum
    * of a valid coordinate and the step. */
   u_step            = u_step == 0 ? 1 : MIN(u_step, (guint)INT_MAX);
   Histogram *p_hist = histogram_new();
   p_hist->u_step    = u_step;
   for (gsize u_y = 0; u_y < (gsize)i_height; u_y += u_step) {
      const guint8 *p_row = p_pixels + u_y * u_stride;
      for (gsize u_x = 0; u_x < (gsize)i_width; u_x += u_step) {
         const guint8 *p_px = p_row + u_x * u_bpp;
         _bin_pixel(p_hist, p_px[u_r], p_px[u_g], p_px[u_b]);
      }
   }
   _find_peak(p_hist);
   return (p_hist);
}

/* TRUE iff GDK hands back p_tex's own pixel bytes without a copy when asked
 * for layout e_fmt: gdk_texture_downloader_download_bytes() refs a
 * GdkMemoryTexture's bytes when the requested format is its native one
 * (verified in GTK 4.14, the CI toolchain, and 4.22) and, from 4.16 on, the
 * colour state matches too -- the sRGB default every ggaze texture carries.
 * Any other combination converts into a fresh g_malloc_n buffer inside GDK,
 * which aborts on failure; that case goes through _from_converted() so the
 * allocation is ours to size and to fail softly. */
static gboolean
_native_bytes_shareable(GdkTexture *p_tex, GdkMemoryFormat e_fmt) {
   guint u_bpp, u_r, u_g, u_b;
   (void)p_tex; /* only consulted from GDK 4.16 on (colour state) */
   if (!_layout(e_fmt, &u_bpp, &u_r, &u_g, &u_b)) {
      return (FALSE);
   }
#if GDK_MAJOR_VERSION > 4 || (GDK_MAJOR_VERSION == 4 && GDK_MINOR_VERSION >= 16)
   return (gdk_color_state_equal(gdk_texture_get_color_state(p_tex),
                                 gdk_color_state_get_srgb()));
#else
   return (TRUE);
#endif
}

/* Bin the texture straight out of its own bytes (no copy, so no size
 * limit): the binner reads the native layout, red and blue included in the
 * order GDK actually stores them. */
static Histogram *
_from_native(GdkTexture *p_tex, int i_w, int i_h, GdkMemoryFormat e_fmt) {
   GdkTextureDownloader *p_dl = gdk_texture_downloader_new(p_tex);
   gdk_texture_downloader_set_format(p_dl, e_fmt);
   gsize   u_stride = 0;
   GBytes *p_bytes  = gdk_texture_downloader_download_bytes(p_dl, &u_stride);
   gdk_texture_downloader_free(p_dl);
   gsize         u_len  = 0;
   const guint8 *p_px   = g_bytes_get_data(p_bytes, &u_len);
   Histogram    *p_hist = NULL;
   /* Belt and braces: never read past the bytes GDK handed over. */
   if (p_px != NULL && u_len >= u_stride * (gsize)i_h) {
      p_hist = histogram_new_from_pixels(p_px, i_w, i_h, u_stride, e_fmt,
                                         histogram_step_for(i_w, i_h));
   }
   g_bytes_unref(p_bytes);
   return (p_hist);
}

/* Bin a texture whose native layout the binner cannot read: GDK converts it
 * to straight R8G8B8A8 into a buffer of ours. The copy is the full texture
 * (GDK has no region or scaled download), so it is refused above
 * u_max_pixels and g_try_malloc'd below it -- either failure is "no plot",
 * never an abort. Fully transparent pixels bin as black on the way through
 * premultiplied alpha; a photo is opaque, and that is what is composited. */
static Histogram *
_from_converted(GdkTexture *p_tex, int i_w, int i_h, guint64 u_max_pixels) {
   if ((guint64)i_w * (guint64)i_h > u_max_pixels) {
      return (NULL);
   }
   gsize   u_stride = (gsize)i_w * 4;
   guint8 *p_buf    = g_try_malloc_n((gsize)i_h, u_stride);
   if (p_buf == NULL) {
      return (NULL);
   }
   GdkTextureDownloader *p_dl = gdk_texture_downloader_new(p_tex);
   gdk_texture_downloader_set_format(p_dl, GDK_MEMORY_R8G8B8A8);
   gdk_texture_downloader_download_into(p_dl, p_buf, u_stride);
   gdk_texture_downloader_free(p_dl);
   Histogram *p_hist =
      histogram_new_from_pixels(p_buf, i_w, i_h, u_stride, GDK_MEMORY_R8G8B8A8,
                                histogram_step_for(i_w, i_h));
   g_free(p_buf);
   return (p_hist);
}

Histogram *
histogram_new_from_texture_full(GdkTexture *p_tex,
                                guint64     u_max_convert_pixels) {
   /* Only a GdkMemoryTexture has its pixels in RAM: a GL or dmabuf texture
    * needs its GdkGLContext / display to download, which the info overlay's
    * GTask worker has no business touching. The loader and the enhancer only
    * ever produce memory textures, so this refuses nothing in practice and
    * keeps the worker display-free by construction. */
   if (p_tex == NULL || !GDK_IS_MEMORY_TEXTURE(p_tex)) {
      return (NULL);
   }
   int i_w = gdk_texture_get_width(p_tex);
   int i_h = gdk_texture_get_height(p_tex);
   if (i_w <= 0 || i_h <= 0) {
      return (NULL);
   }
   GdkMemoryFormat e_fmt = gdk_texture_get_format(p_tex);
   if (_native_bytes_shareable(p_tex, e_fmt)) {
      return (_from_native(p_tex, i_w, i_h, e_fmt));
   }
   return (_from_converted(p_tex, i_w, i_h, u_max_convert_pixels));
}

Histogram *
histogram_new_from_texture(GdkTexture *p_tex) {
   return (
      histogram_new_from_texture_full(p_tex, HISTOGRAM_MAX_CONVERT_PIXELS));
}
