#ifndef GGAZE_HISTOGRAM_H
#define GGAZE_HISTOGRAM_H

/*:*
 * ggaze — RGB + luminance histogram builder
 *
 * Bins the pixels of a decoded image into HISTOGRAM_BINS buckets per channel
 * (red, green, blue, Rec.709 luminance) so the info card (`i`) can show an
 * exposure histogram while culling. Plain-C: no GtkWidget, no display; the
 * only GDK use is reading pixels out of an immutable GdkTexture, which is
 * safe from the GTask worker info-overlay.c runs it in. Drawing is the
 * histogram-view widget's job.
 *
 * Large images are downsampled BEFORE binning: histogram_new_from_texture()
 * picks a sampling stride so at most HISTOGRAM_MAX_SAMPLES pixels are read,
 * which keeps a 100-megapixel photo at the same cost as a small one. The
 * shape of a histogram is statistically identical on a regular subsample.
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

typedef enum {
   HISTOGRAM_CHANNEL_R = 0,
   HISTOGRAM_CHANNEL_G,
   HISTOGRAM_CHANNEL_B,
   HISTOGRAM_CHANNEL_LUM,
   HISTOGRAM_CHANNEL_COUNT
} HistogramChannel;

typedef struct {
   guint32 au_bins[HISTOGRAM_CHANNEL_COUNT][HISTOGRAM_BINS];
   guint32 u_peak;    /* highest bin count over all channels (plot scale) */
   guint64 u_samples; /* pixels binned (0 = empty histogram) */
   guint   u_step;    /* sampling stride used, 1 = every pixel */
} Histogram;

/* An empty histogram (all bins 0, u_samples 0). */
Histogram *histogram_new(void);

/* Bin an 8-bit pixel buffer. e_format selects the byte order; supported are
 * the 8-bit RGB(A)/BGR(A) layouts (premultiplied variants included -- alpha
 * is ignored, a photo is opaque). u_stride is the row pitch in bytes.
 * Every u_step-th pixel of every u_step-th row is read (0 means 1).
 * Returns NULL for a NULL buffer, a zero/negative dimension, a stride
 * shorter than one row, or an unsupported format. */
Histogram *histogram_new_from_pixels(const guint8 *p_pixels, int i_width,
                                     int i_height, gsize u_stride,
                                     GdkMemoryFormat e_format, guint u_step);

/* Bin a GdkTexture: downloads it as R8G8B8A8 and samples it with a stride
 * that keeps the read pixels within HISTOGRAM_MAX_SAMPLES. NULL for a NULL
 * texture or one with a zero dimension. GDK converts through premultiplied
 * alpha on the way out, so a fully transparent pixel bins as black -- a
 * photo is opaque, and that is what the viewer composites anyway. */
Histogram *histogram_new_from_texture(GdkTexture *p_tex);

void histogram_delete(Histogram *p_hist);

/* Sampling stride for a i_width x i_height image: the smallest step whose
 * subsample fits HISTOGRAM_MAX_SAMPLES (1 for anything up to the budget).
 * Public so the choice is unit-testable independently of a texture. */
guint histogram_step_for(int i_width, int i_height);

G_END_DECLS

#endif /* GGAZE_HISTOGRAM_H */
