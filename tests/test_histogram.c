/*:*
 * ggaze — histogram builder unit test
 *
 * Bins hand-built pixel buffers and GdkMemoryTextures (no display needed)
 * and asserts the exact per-channel counts, the byte-order handling across
 * the supported layouts, stride padding, stride-based subsampling and the
 * sampling budget. Single-primary pixels pin red and blue to their own
 * channels through every texture path (native RGBA, native premultiplied
 * BGRA -- GDK_MEMORY_DEFAULT on little-endian, which a straight-RGBA read
 * once swapped -- and the 16-bit / float conversion path). Negative cases:
 * NULL buffer, zero / negative dimensions, a too-short stride, an
 * unsupported pixel format, a step above INT_MAX, a NULL texture, a
 * converted texture over the copy budget. A non-memory texture (GL, dmabuf)
 * is not constructible without a display, so that refusal is inspected, not
 * run.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "histogram.h"

#include <gdk/gdk.h>
#include <glib.h>
#include <string.h>

/* Bin index of an 8-bit value, the same mapping histogram.c uses. */
#define BIN(v) ((v) * HISTOGRAM_BINS / 256)

/* The 2x2 reference image: pure red, pure green, pure blue, white. */
static const guint8 RGBA_2X2[16] = {255, 0, 0,   255, 0,   255, 0,   255,
                                    0,   0, 255, 255, 255, 255, 255, 255};

/* Rec.709 luminance of the four reference pixels (54/183/19 weights). */
static void
assert_reference_bins(const Histogram *p_hist) {
   g_assert_cmpuint(p_hist->u_samples, ==, 4);
   g_assert_cmpuint(p_hist->u_peak, ==, 2);
   for (guint u_ch = HISTOGRAM_CHANNEL_R; u_ch <= HISTOGRAM_CHANNEL_B; u_ch++) {
      /* each primary is 255 in two pixels (its own + white), 0 in the rest */
      g_assert_cmpuint(p_hist->u_bins[u_ch][BIN(255)], ==, 2);
      g_assert_cmpuint(p_hist->u_bins[u_ch][BIN(0)], ==, 2);
   }
   const guint32 *p_lum = p_hist->u_bins[HISTOGRAM_CHANNEL_LUM];
   g_assert_cmpuint(p_lum[BIN(54)], ==, 1);  /* red */
   g_assert_cmpuint(p_lum[BIN(182)], ==, 1); /* green */
   g_assert_cmpuint(p_lum[BIN(19)], ==, 1);  /* blue */
   g_assert_cmpuint(p_lum[BIN(255)], ==, 1); /* white */
}

static void
test_new_is_empty(void) {
   Histogram *p_hist = histogram_new();
   g_assert_nonnull(p_hist);
   g_assert_cmpuint(p_hist->u_samples, ==, 0);
   g_assert_cmpuint(p_hist->u_peak, ==, 0);
   g_assert_cmpuint(p_hist->u_step, ==, 1);
   histogram_delete(p_hist);
   histogram_delete(NULL); /* must be a no-op */
}

static void
test_rgba8_bins_each_channel(void) {
   Histogram *p_hist =
      histogram_new_from_pixels(RGBA_2X2, 2, 2, 8, GDK_MEMORY_R8G8B8A8, 1);
   g_assert_nonnull(p_hist);
   g_assert_cmpuint(p_hist->u_step, ==, 1);
   assert_reference_bins(p_hist);
   histogram_delete(p_hist);
}

/* The same four pixels in every other supported layout must bin the same. */
static void
test_layouts_agree(void) {
   const guint8 c_rgb[12]  = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
   const guint8 c_bgr[12]  = {0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255};
   const guint8 c_bgra[16] = {0,   0, 255, 255, 0,   255, 0,   255,
                              255, 0, 0,   255, 255, 255, 255, 255};
   const guint8 c_argb[16] = {255, 255, 0, 0,   255, 0,   255, 0,
                              255, 0,   0, 255, 255, 255, 255, 255};
   Histogram   *p_ref =
      histogram_new_from_pixels(RGBA_2X2, 2, 2, 8, GDK_MEMORY_R8G8B8A8, 1);
   struct {
      const guint8   *p_px;
      gsize           u_stride;
      GdkMemoryFormat e_fmt;
   } a_cases[] = {
      {c_rgb, 6, GDK_MEMORY_R8G8B8},
      {c_bgr, 6, GDK_MEMORY_B8G8R8},
      {c_bgra, 8, GDK_MEMORY_B8G8R8A8},
      {c_bgra, 8, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED},
      {c_argb, 8, GDK_MEMORY_A8R8G8B8},
      {c_argb, 8, GDK_MEMORY_A8R8G8B8_PREMULTIPLIED},
      {RGBA_2X2, 8, GDK_MEMORY_R8G8B8A8_PREMULTIPLIED},
   };
   for (gsize u = 0; u < G_N_ELEMENTS(a_cases); u++) {
      Histogram *p_hist = histogram_new_from_pixels(
         a_cases[u].p_px, 2, 2, a_cases[u].u_stride, a_cases[u].e_fmt, 1);
      g_assert_nonnull(p_hist);
      assert_reference_bins(p_hist);
      g_assert_cmpmem(p_hist->u_bins, sizeof(p_hist->u_bins), p_ref->u_bins,
                      sizeof(p_ref->u_bins));
      histogram_delete(p_hist);
   }
   histogram_delete(p_ref);
}

/* Row padding beyond width * bpp is never read: fill it with 255s that would
 * land in the top bin if they were. */
static void
test_stride_padding_ignored(void) {
   guint8 c_px[2 * 12]; /* 2 rows of 12 bytes: 2 RGBA pixels + 4 padding */
   memset(c_px, 255, sizeof(c_px));
   memcpy(c_px, RGBA_2X2, 8);          /* row 0: red, green */
   memcpy(c_px + 12, RGBA_2X2 + 8, 8); /* row 1: blue, white */
   Histogram *p_hist =
      histogram_new_from_pixels(c_px, 2, 2, 12, GDK_MEMORY_R8G8B8A8, 1);
   g_assert_nonnull(p_hist);
   assert_reference_bins(p_hist);
   histogram_delete(p_hist);
}

/* A stride of 2 over a 4x4 image reads the 4 pixels at even coordinates
 * only; step 0 means 1 (every pixel). */
static void
test_step_subsamples(void) {
   guint8 c_px[4 * 4 * 4];
   memset(c_px, 0, sizeof(c_px)); /* black everywhere ... */
   for (int i_y = 0; i_y < 4; i_y += 2) {
      for (int i_x = 0; i_x < 4; i_x += 2) {
         c_px[(i_y * 4 + i_x) * 4] = 255; /* ... red at even coordinates */
      }
   }
   Histogram *p_two =
      histogram_new_from_pixels(c_px, 4, 4, 16, GDK_MEMORY_R8G8B8A8, 2);
   g_assert_nonnull(p_two);
   g_assert_cmpuint(p_two->u_step, ==, 2);
   g_assert_cmpuint(p_two->u_samples, ==, 4);
   g_assert_cmpuint(p_two->u_bins[HISTOGRAM_CHANNEL_R][BIN(255)], ==, 4);
   g_assert_cmpuint(p_two->u_bins[HISTOGRAM_CHANNEL_R][BIN(0)], ==, 0);
   histogram_delete(p_two);

   Histogram *p_all =
      histogram_new_from_pixels(c_px, 4, 4, 16, GDK_MEMORY_R8G8B8A8, 0);
   g_assert_nonnull(p_all);
   g_assert_cmpuint(p_all->u_step, ==, 1);
   g_assert_cmpuint(p_all->u_samples, ==, 16);
   g_assert_cmpuint(p_all->u_bins[HISTOGRAM_CHANNEL_R][BIN(255)], ==, 4);
   g_assert_cmpuint(p_all->u_bins[HISTOGRAM_CHANNEL_R][BIN(0)], ==, 12);
   /* the peak is over ALL channels: green and blue are 0 in every pixel */
   g_assert_cmpuint(p_all->u_bins[HISTOGRAM_CHANNEL_G][BIN(0)], ==, 16);
   g_assert_cmpuint(p_all->u_peak, ==, 16);
   histogram_delete(p_all);
}

static void
test_rejects_bad_input(void) {
   g_assert_null(
      histogram_new_from_pixels(NULL, 2, 2, 8, GDK_MEMORY_R8G8B8A8, 1));
   g_assert_null(
      histogram_new_from_pixels(RGBA_2X2, 0, 2, 8, GDK_MEMORY_R8G8B8A8, 1));
   g_assert_null(
      histogram_new_from_pixels(RGBA_2X2, 2, -1, 8, GDK_MEMORY_R8G8B8A8, 1));
   /* stride shorter than one row of pixels */
   g_assert_null(
      histogram_new_from_pixels(RGBA_2X2, 2, 2, 7, GDK_MEMORY_R8G8B8A8, 1));
   /* layouts the binner does not read: float and 16-bit */
   g_assert_null(histogram_new_from_pixels(RGBA_2X2, 1, 1, 16,
                                           GDK_MEMORY_R32G32B32A32_FLOAT, 1));
   g_assert_null(
      histogram_new_from_pixels(RGBA_2X2, 1, 1, 8, GDK_MEMORY_R16G16B16A16, 1));
}

/* A step above INT_MAX used to go negative through the (int) loop
 * increment and walk the rows backwards off the buffer. It is clamped to
 * INT_MAX now, which still reads exactly pixel (0,0). */
static void
test_step_above_int_max_is_clamped(void) {
   Histogram *p_hist = histogram_new_from_pixels(
      RGBA_2X2, 2, 2, 8, GDK_MEMORY_R8G8B8A8, G_MAXUINT);
   g_assert_nonnull(p_hist);
   g_assert_cmpuint(p_hist->u_step, ==, (guint)G_MAXINT);
   g_assert_cmpuint(p_hist->u_samples, ==, 1);
   g_assert_cmpuint(p_hist->u_bins[HISTOGRAM_CHANNEL_R][BIN(255)], ==, 1);
   g_assert_cmpuint(p_hist->u_bins[HISTOGRAM_CHANNEL_G][BIN(0)], ==, 1);
   histogram_delete(p_hist);
}

static void
test_step_for_budget(void) {
   g_assert_cmpuint(histogram_step_for(0, 0), ==, 1);
   g_assert_cmpuint(histogram_step_for(-1, 5), ==, 1);
   g_assert_cmpuint(histogram_step_for(1, 1), ==, 1);
   g_assert_cmpuint(histogram_step_for(512, 512), ==, 1);
   g_assert_cmpuint(histogram_step_for(513, 512), ==, 2);
   g_assert_cmpuint(histogram_step_for(1024, 1024), ==, 2);
   g_assert_cmpuint(histogram_step_for(32768, 32768), ==, 64);
   /* the subsample really fits the budget, for a spread of sizes */
   const int a_sizes[][2] = {{4000, 3000}, {6000, 4000}, {1, 100000}};
   for (gsize u = 0; u < G_N_ELEMENTS(a_sizes); u++) {
      guint   u_step = histogram_step_for(a_sizes[u][0], a_sizes[u][1]);
      guint64 u_cols = ((guint64)a_sizes[u][0] + u_step - 1) / u_step;
      guint64 u_rows = ((guint64)a_sizes[u][1] + u_step - 1) / u_step;
      g_assert_cmpuint(u_cols * u_rows, <=, HISTOGRAM_MAX_SAMPLES);
   }
}

static GdkTexture *
texture_from(gconstpointer p_px, gsize u_len, int i_w, int i_h,
             GdkMemoryFormat e_fmt, gsize u_stride) {
   GBytes     *p_b = g_bytes_new(p_px, u_len);
   GdkTexture *p_t = gdk_memory_texture_new(i_w, i_h, e_fmt, p_b, u_stride);
   g_bytes_unref(p_b);
   return (p_t);
}

/* A small texture bins like its pixel buffer, whatever layout GDK stores
 * it in (read natively, no conversion, for every 8-bit layout). */
static void
test_from_texture_matches_pixels(void) {
   g_assert_null(histogram_new_from_texture(NULL));

   GdkTexture *p_rgba =
      texture_from(RGBA_2X2, sizeof(RGBA_2X2), 2, 2, GDK_MEMORY_R8G8B8A8, 8);
   Histogram *p_hist = histogram_new_from_texture(p_rgba);
   g_assert_nonnull(p_hist);
   g_assert_cmpuint(p_hist->u_step, ==, 1);
   assert_reference_bins(p_hist);
   histogram_delete(p_hist);
   g_object_unref(p_rgba);

   const guint8 c_bgra[16] = {0,   0, 255, 255, 0,   255, 0,   255,
                              255, 0, 0,   255, 255, 255, 255, 255};
   GdkTexture  *p_bgra     = texture_from(c_bgra, sizeof(c_bgra), 2, 2,
                                          GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, 8);
   p_hist                  = histogram_new_from_texture(p_bgra);
   g_assert_nonnull(p_hist);
   assert_reference_bins(p_hist);
   histogram_delete(p_hist);
   g_object_unref(p_bgra);
}

/* A texture over the budget is subsampled: the stride is the one
 * histogram_step_for() picks and the sample count is the subsample's. */
static void
test_from_texture_downsamples(void) {
   const int i_w  = 2048;
   const int i_h  = 1024;
   guint8   *p_px = g_malloc0((gsize)i_w * i_h * 4);
   for (gsize u = 0; u < (gsize)i_w * i_h; u++) {
      p_px[u * 4 + 1] = 200; /* uniform mid-green ... */
      p_px[u * 4 + 3] = 255; /* ... opaque: GDK's download premultiplies */
   }
   GdkTexture *p_tex = texture_from(p_px, (gsize)i_w * i_h * 4, i_w, i_h,
                                    GDK_MEMORY_R8G8B8A8, (gsize)i_w * 4);
   g_free(p_px);
   Histogram *p_hist = histogram_new_from_texture(p_tex);
   g_assert_nonnull(p_hist);
   guint u_step = histogram_step_for(i_w, i_h);
   g_assert_cmpuint(u_step, >, 1);
   g_assert_cmpuint(p_hist->u_step, ==, u_step);
   guint64 u_expect = (guint64)((i_w + u_step - 1) / u_step) *
                      (guint64)((i_h + u_step - 1) / u_step);
   g_assert_cmpuint(p_hist->u_samples, ==, u_expect);
   g_assert_cmpuint(p_hist->u_samples, <=, HISTOGRAM_MAX_SAMPLES);
   g_assert_cmpuint(p_hist->u_bins[HISTOGRAM_CHANNEL_G][BIN(200)], ==,
                    u_expect);
   g_assert_cmpuint(p_hist->u_peak, ==, u_expect);
   histogram_delete(p_hist);
   g_object_unref(p_tex);
}

/* One pure-primary pixel: its own channel has the top bin, the two others
 * the bottom bin, and luminance lands in the bin only that primary can
 * produce (54 for red, 19 for blue). Not invariant under an R/B swap. */
static void
assert_single_primary(const Histogram *p_hist, HistogramChannel e_own,
                      guint u_lum) {
   g_assert_nonnull(p_hist);
   g_assert_cmpuint(p_hist->u_samples, ==, 1);
   for (guint u_ch = HISTOGRAM_CHANNEL_R; u_ch <= HISTOGRAM_CHANNEL_B; u_ch++) {
      g_assert_cmpuint(p_hist->u_bins[u_ch][BIN(255)], ==, u_ch == e_own);
      g_assert_cmpuint(p_hist->u_bins[u_ch][BIN(0)], ==, u_ch != e_own);
   }
   g_assert_cmpuint(p_hist->u_bins[HISTOGRAM_CHANNEL_LUM][BIN(u_lum)], ==, 1);
}

/* Red and blue stay on their own channels through every texture path:
 * native 8-bit RGBA and premultiplied BGRA (GDK_MEMORY_DEFAULT on
 * little-endian -- reading it as straight RGBA swapped R and B), and the
 * converting fallback for 16-bit and float layouts. */
static void
test_texture_primaries_not_swapped(void) {
   const guint8  c_red_rgba[4]   = {255, 0, 0, 255};
   const guint8  c_blue_rgba[4]  = {0, 0, 255, 255};
   const guint8  c_red_bgra[4]   = {0, 0, 255, 255};
   const guint8  c_blue_bgra[4]  = {255, 0, 0, 255};
   const guint16 c_red_16[4]     = {0xFFFF, 0, 0, 0xFFFF};
   const guint16 c_blue_16[4]    = {0, 0, 0xFFFF, 0xFFFF};
   const float   c_red_float[4]  = {1.f, 0.f, 0.f, 1.f};
   const float   c_blue_float[4] = {0.f, 0.f, 1.f, 1.f};
   struct {
      const void      *p_px;
      gsize            u_len;
      GdkMemoryFormat  e_fmt;
      HistogramChannel e_own;
      guint            u_lum;
   } a_cases[] = {
      {c_red_rgba, 4, GDK_MEMORY_R8G8B8A8, HISTOGRAM_CHANNEL_R, 54},
      {c_blue_rgba, 4, GDK_MEMORY_R8G8B8A8, HISTOGRAM_CHANNEL_B, 19},
      {c_red_bgra, 4, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, HISTOGRAM_CHANNEL_R,
       54},
      {c_blue_bgra, 4, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, HISTOGRAM_CHANNEL_B,
       19},
      {c_red_16, 8, GDK_MEMORY_R16G16B16A16, HISTOGRAM_CHANNEL_R, 54},
      {c_blue_16, 8, GDK_MEMORY_R16G16B16A16_PREMULTIPLIED, HISTOGRAM_CHANNEL_B,
       19},
      {c_red_float, 16, GDK_MEMORY_R32G32B32A32_FLOAT, HISTOGRAM_CHANNEL_R, 54},
      {c_blue_float, 16, GDK_MEMORY_R32G32B32A32_FLOAT_PREMULTIPLIED,
       HISTOGRAM_CHANNEL_B, 19},
   };
   for (gsize u = 0; u < G_N_ELEMENTS(a_cases); u++) {
      GdkTexture *p_tex  = texture_from(a_cases[u].p_px, a_cases[u].u_len, 1, 1,
                                        a_cases[u].e_fmt, a_cases[u].u_len);
      Histogram  *p_hist = histogram_new_from_texture(p_tex);
      assert_single_primary(p_hist, a_cases[u].e_own, a_cases[u].u_lum);
      histogram_delete(p_hist);
      g_object_unref(p_tex);
   }
}

/* The copy budget bounds only the converting fallback: a 16-bit 2x2 texture
 * (4 pixels) is refused at a budget of 3 and read at 4 -- and then bins
 * like the 8-bit reference -- while a native 8-bit texture ignores the
 * budget entirely because no copy is made for it. */
static void
test_from_texture_convert_budget(void) {
   const guint16 c_px16[16] = {0xFFFF, 0,      0,      0xFFFF, 0,      0xFFFF,
                               0,      0xFFFF, 0,      0,      0xFFFF, 0xFFFF,
                               0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
   GdkTexture   *p_16 =
      texture_from(c_px16, sizeof(c_px16), 2, 2, GDK_MEMORY_R16G16B16A16, 16);
   g_assert_null(histogram_new_from_texture_full(p_16, 3));
   Histogram *p_hist = histogram_new_from_texture_full(p_16, 4);
   g_assert_nonnull(p_hist);
   assert_reference_bins(p_hist);
   histogram_delete(p_hist);
   g_object_unref(p_16);

   GdkTexture *p_rgba =
      texture_from(RGBA_2X2, sizeof(RGBA_2X2), 2, 2, GDK_MEMORY_R8G8B8A8, 8);
   p_hist = histogram_new_from_texture_full(p_rgba, 1);
   g_assert_nonnull(p_hist);
   assert_reference_bins(p_hist);
   histogram_delete(p_hist);
   g_object_unref(p_rgba);
}

int
main(int argc, char **argv) {
   g_test_init(&argc, &argv, NULL);
   g_test_add_func("/histogram/new_is_empty", test_new_is_empty);
   g_test_add_func("/histogram/rgba8_bins_each_channel",
                   test_rgba8_bins_each_channel);
   g_test_add_func("/histogram/layouts_agree", test_layouts_agree);
   g_test_add_func("/histogram/stride_padding_ignored",
                   test_stride_padding_ignored);
   g_test_add_func("/histogram/step_subsamples", test_step_subsamples);
   g_test_add_func("/histogram/rejects_bad_input", test_rejects_bad_input);
   g_test_add_func("/histogram/step_above_int_max_is_clamped",
                   test_step_above_int_max_is_clamped);
   g_test_add_func("/histogram/step_for_budget", test_step_for_budget);
   g_test_add_func("/histogram/from_texture_matches_pixels",
                   test_from_texture_matches_pixels);
   g_test_add_func("/histogram/from_texture_downsamples",
                   test_from_texture_downsamples);
   g_test_add_func("/histogram/texture_primaries_not_swapped",
                   test_texture_primaries_not_swapped);
   g_test_add_func("/histogram/from_texture_convert_budget",
                   test_from_texture_convert_budget);
   return (g_test_run());
}
