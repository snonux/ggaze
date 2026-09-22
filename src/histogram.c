/*:*
 * ggaze — RGB + luminance histogram builder
 *
 * See histogram.h. Pure binning over an 8-bit pixel buffer plus a GdkTexture
 * front end that downloads and subsamples. No GtkWidget, no display.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "histogram.h"

#include <gdk/gdk.h>
#include <glib.h>

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
   p_hist->au_bins[HISTOGRAM_CHANNEL_R][u_r * HISTOGRAM_BINS / 256]++;
   p_hist->au_bins[HISTOGRAM_CHANNEL_G][u_g * HISTOGRAM_BINS / 256]++;
   p_hist->au_bins[HISTOGRAM_CHANNEL_B][u_b * HISTOGRAM_BINS / 256]++;
   p_hist->au_bins[HISTOGRAM_CHANNEL_LUM][u_lum * HISTOGRAM_BINS / 256]++;
   p_hist->u_samples++;
}

/* The plot scale: the fullest bin over every channel, so the four curves
 * share one vertical axis and stay comparable to each other. */
static void
_find_peak(Histogram *p_hist) {
   p_hist->u_peak = 0;
   for (guint u_ch = 0; u_ch < HISTOGRAM_CHANNEL_COUNT; u_ch++) {
      for (guint u = 0; u < HISTOGRAM_BINS; u++) {
         if (p_hist->au_bins[u_ch][u] > p_hist->u_peak) {
            p_hist->u_peak = p_hist->au_bins[u_ch][u];
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
   if (u_step == 0) {
      u_step = 1;
   }
   Histogram *p_hist = histogram_new();
   p_hist->u_step    = u_step;
   for (int i_y = 0; i_y < i_height; i_y += (int)u_step) {
      const guint8 *p_row = p_pixels + (gsize)i_y * u_stride;
      for (int i_x = 0; i_x < i_width; i_x += (int)u_step) {
         const guint8 *p_px = p_row + (gsize)i_x * u_bpp;
         _bin_pixel(p_hist, p_px[u_r], p_px[u_g], p_px[u_b]);
      }
   }
   _find_peak(p_hist);
   return (p_hist);
}

Histogram *
histogram_new_from_texture(GdkTexture *p_tex) {
   if (p_tex == NULL) {
      return (NULL);
   }
   int i_w = gdk_texture_get_width(p_tex);
   int i_h = gdk_texture_get_height(p_tex);
   if (i_w <= 0 || i_h <= 0) {
      return (NULL);
   }
   /* gdk_texture_download() has no region or scale variant, so the whole
    * texture is converted to straight RGBA once (a transient copy the size
    * of the texture itself, bounded by the loader's dimension cap); the
    * stride then keeps the BINNING cost constant. The texture is immutable,
    * so this is safe off the main thread. */
   gsize   u_stride = (gsize)i_w * 4;
   guint8 *p_buf    = g_malloc(u_stride * (gsize)i_h);
   gdk_texture_download(p_tex, p_buf, u_stride);
   Histogram *p_hist =
      histogram_new_from_pixels(p_buf, i_w, i_h, u_stride, GDK_MEMORY_R8G8B8A8,
                                histogram_step_for(i_w, i_h));
   g_free(p_buf);
   return (p_hist);
}
