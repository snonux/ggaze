#ifndef GGAZE_HISTOGRAM_H
#define GGAZE_HISTOGRAM_H

/*:*
 * ggaze — RGB + luminance histogram builder
 *
 * Bins the pixels of a decoded image into HISTOGRAM_BINS buckets per channel
 * (red, green, blue, Rec.709 luminance) so the info card (`i`) can show an
 * exposure histogram while culling. Plain-C: no GtkWidget, no display; the
 * only GDK use is reading pixels out of an immutable GdkMemoryTexture, which
 * is safe from the GTask worker info-overlay.c runs it in. Drawing is the
 * histogram-view widget's job.
 *
 * Cost model. Binning is subsampled: histogram_new_from_texture() picks a
 * stride so at most HISTOGRAM_MAX_SAMPLES pixels are read whatever the image
 * size (the shape of a histogram is statistically identical on a regular
 * subsample). Getting at the pixels is where the size could bite: GDK has no
 * region or scaled download, so the pixels are read in the texture's NATIVE
 * layout, which GdkTextureDownloader hands back without a copy for a
 * GdkMemoryTexture (every texture the loader and the enhancer produce). Only
 * a texture in a layout the binner cannot read (16-bit, float) is converted,
 * and that copy is bounded: above HISTOGRAM_MAX_CONVERT_PIXELS there is no
 * plot, and the buffer is g_try_malloc'd so an allocation failure means "no
 * plot" too, never an abort.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk/gdk.h>
#include <glib.h>

G_BEGIN_DECLS

/* Bins per channel. 64 gives a readable shape on a ~200 px wide card plot
 * (3 px per bin) without the noise of a 256-bin histogram of few samples. */
#define HISTOGRAM_BINS 64

/* Pixel budget per histogram (512x512). Enough for a smooth curve; a stride
 * is chosen above it so binning never scales with the image. */
#define HISTOGRAM_MAX_SAMPLES (512u * 512u)

/* Largest texture (in pixels) the converting fallback will copy: 32 MP is a
 * 128 MiB transient buffer, the most the card is allowed to cost on top of
 * the texture itself. The native (copy-free) path has no such limit. */
#define HISTOGRAM_MAX_CONVERT_PIXELS (32u * 1024u * 1024u)

typedef enum {
   HISTOGRAM_CHANNEL_R = 0,
   HISTOGRAM_CHANNEL_G,
   HISTOGRAM_CHANNEL_B,
   HISTOGRAM_CHANNEL_LUM,
   HISTOGRAM_CHANNEL_COUNT
} HistogramChannel;

typedef struct {
   guint32 u_bins[HISTOGRAM_CHANNEL_COUNT][HISTOGRAM_BINS];
   guint32 u_peak;    /* highest bin count over all channels (plot scale) */
   guint64 u_samples; /* pixels binned (0 = empty histogram) */
   guint   u_step;    /* sampling stride used, 1 = every pixel */
} Histogram;

/* An empty histogram (all bins 0, u_samples 0). */
Histogram *histogram_new(void);

/* Bin an 8-bit pixel buffer. e_format selects the byte order; supported are
 * all of GDK's 8-bit RGB(A)/BGR(A) layouts (premultiplied and X8-padded
 * variants included -- alpha is ignored, a photo is opaque). u_stride is the
 * row pitch in bytes; the last row may be as short as one row of pixels.
 * Every u_step-th pixel of every u_step-th row is read (0 means 1; anything
 * above INT_MAX is clamped to it, which still reads pixel (0,0)).
 * Returns NULL for a NULL buffer, a zero/negative dimension, a stride
 * shorter than one row, or an unsupported format. */
Histogram *histogram_new_from_pixels(const guint8 *p_pixels, int i_width,
                                     int i_height, gsize u_stride,
                                     GdkMemoryFormat e_format, guint u_step);

/* Bin a GdkMemoryTexture, sampled with a stride that keeps the read pixels
 * within HISTOGRAM_MAX_SAMPLES; see the file comment for how the pixels are
 * reached. NULL for a NULL texture, a texture that is not a GdkMemoryTexture
 * (a GL/dmabuf texture cannot be downloaded off the main thread without its
 * context), one with a zero dimension, or one whose layout must be converted
 * and is larger than HISTOGRAM_MAX_CONVERT_PIXELS / cannot be allocated. */
Histogram *histogram_new_from_texture(GdkTexture *p_tex);

/* histogram_new_from_texture() with the conversion budget as a parameter
 * (u_max_convert_pixels), so the bound is unit-testable on a small texture.
 * The budget applies only to the converting fallback, never to a texture
 * read in its native layout. */
Histogram *histogram_new_from_texture_full(GdkTexture *p_tex,
                                           guint64     u_max_convert_pixels);

void histogram_delete(Histogram *p_hist);

/* Sampling stride for a i_width x i_height image: the smallest step whose
 * subsample fits HISTOGRAM_MAX_SAMPLES (1 for anything up to the budget).
 * Public so the choice is unit-testable independently of a texture. */
guint histogram_step_for(int i_width, int i_height);

G_END_DECLS

#endif /* GGAZE_HISTOGRAM_H */
