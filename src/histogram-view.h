#ifndef GGAZE_HISTOGRAM_VIEW_H
#define GGAZE_HISTOGRAM_VIEW_H

/*:*
 * ggaze — histogram plot widget
 *
 * GgazeHistogramView : GtkWidget draws a Histogram (histogram.h) with GTK4
 * render nodes: one translucent bar per bin for red, green and blue over a
 * grey luminance curve, all on the one scale of the histogram's peak bin.
 * It owns nothing but the Histogram it is handed; the info overlay decides
 * when it is shown. Kept separate from histogram.c so the builder stays a
 * plain-C module with no widget and from info-overlay.c so the card keeps
 * one job (what to show and when) and this widget the other (how to draw).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gtk/gtk.h>

#include "histogram.h"

G_BEGIN_DECLS

#define GGAZE_TYPE_HISTOGRAM_VIEW (ggaze_histogram_view_get_type())
G_DECLARE_FINAL_TYPE(GgazeHistogramView, ggaze_histogram_view, GGAZE,
                     HISTOGRAM_VIEW, GtkWidget)

GtkWidget *ggaze_histogram_view_new(void);

/* Take p_hist (transfer full; NULL clears the plot) and redraw. */
void ggaze_histogram_view_set_histogram(GgazeHistogramView *p_view,
                                        Histogram          *p_hist);

/* The histogram being drawn (borrowed), or NULL -- tests assert on it. */
const Histogram *ggaze_histogram_view_get_histogram(GgazeHistogramView *p_view);

G_END_DECLS

#endif /* GGAZE_HISTOGRAM_VIEW_H */
