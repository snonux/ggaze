/*:*
 * ggaze — histogram plot widget
 *
 * See histogram-view.h. Snapshot-drawn; no cairo, no CSS beyond what the
 * info card wraps around it.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "histogram-view.h"

#include <gtk/gtk.h>

#include "histogram.h"

/* Natural size of the plot: 3 px per bin at 64 bins, and a height that fits
 * under the EXIF lines without growing the card past the picture. */
#define HISTOGRAM_VIEW_WIDTH (HISTOGRAM_BINS * 3)
#define HISTOGRAM_VIEW_HEIGHT 56

struct _GgazeHistogramView {
   GtkWidget  parent_instance;
   Histogram *p_hist; /* owned; NULL draws nothing */
};

G_DEFINE_FINAL_TYPE(GgazeHistogramView, ggaze_histogram_view, GTK_TYPE_WIDGET)

static void
ggaze_histogram_view_measure(GtkWidget *p_widget, GtkOrientation e_orient,
                             int i_for_size, int *p_min, int *p_nat,
                             int *p_min_bl, int *p_nat_bl) {
   (void)p_widget;
   (void)i_for_size;
   *p_min = *p_nat = (e_orient == GTK_ORIENTATION_HORIZONTAL)
                        ? HISTOGRAM_VIEW_WIDTH
                        : HISTOGRAM_VIEW_HEIGHT;
   *p_min_bl = *p_nat_bl = -1;
}

/* Draw one channel as HISTOGRAM_BINS bottom-anchored bars scaled to the
 * histogram's peak. Translucent colours let the three RGB curves show
 * through each other where they overlap (the usual "additive" look). */
static void
_draw_channel(GtkSnapshot *p_snap, const Histogram *p_hist,
              HistogramChannel e_ch, const GdkRGBA *p_color, float f_w,
              float f_h) {
   float f_bin_w = f_w / (float)HISTOGRAM_BINS;
   for (guint u = 0; u < HISTOGRAM_BINS; u++) {
      guint32 u_count = p_hist->u_bins[e_ch][u];
      if (u_count == 0) {
         continue;
      }
      float           f_bar_h = f_h * (float)u_count / (float)p_hist->u_peak;
      graphene_rect_t rect    = GRAPHENE_RECT_INIT(
         (float)u * f_bin_w, f_h - f_bar_h, f_bin_w, f_bar_h);
      gtk_snapshot_append_color(p_snap, p_color, &rect);
   }
}

static void
ggaze_histogram_view_snapshot(GtkWidget *p_widget, GtkSnapshot *p_snap) {
   GgazeHistogramView *p_view = GGAZE_HISTOGRAM_VIEW(p_widget);
   const Histogram    *p_hist = p_view->p_hist;
   if (p_hist == NULL || p_hist->u_peak == 0) {
      return;
   }
   float f_w = (float)gtk_widget_get_width(p_widget);
   float f_h = (float)gtk_widget_get_height(p_widget);
   /* A faint plot floor so an all-dark image still reads as "a histogram
    * with everything piled left" rather than an empty gap in the card. */
   const GdkRGBA   floor      = {1.0f, 1.0f, 1.0f, 0.12f};
   graphene_rect_t floor_rect = GRAPHENE_RECT_INIT(0.f, 0.f, f_w, f_h);
   gtk_snapshot_append_color(p_snap, &floor, &floor_rect);
   /* Luminance first (light grey, underneath), then the three primaries. */
   const GdkRGBA lum = {0.9f, 0.9f, 0.9f, 0.35f};
   const GdkRGBA red = {1.0f, 0.25f, 0.25f, 0.55f};
   const GdkRGBA grn = {0.25f, 1.0f, 0.25f, 0.55f};
   const GdkRGBA blu = {0.35f, 0.45f, 1.0f, 0.55f};
   _draw_channel(p_snap, p_hist, HISTOGRAM_CHANNEL_LUM, &lum, f_w, f_h);
   _draw_channel(p_snap, p_hist, HISTOGRAM_CHANNEL_R, &red, f_w, f_h);
   _draw_channel(p_snap, p_hist, HISTOGRAM_CHANNEL_G, &grn, f_w, f_h);
   _draw_channel(p_snap, p_hist, HISTOGRAM_CHANNEL_B, &blu, f_w, f_h);
}

static void
ggaze_histogram_view_finalize(GObject *p_obj) {
   GgazeHistogramView *p_view = GGAZE_HISTOGRAM_VIEW(p_obj);
   g_clear_pointer(&p_view->p_hist, histogram_delete);
   G_OBJECT_CLASS(ggaze_histogram_view_parent_class)->finalize(p_obj);
}

static void
ggaze_histogram_view_class_init(GgazeHistogramViewClass *p_klass) {
   GObjectClass   *p_oc = G_OBJECT_CLASS(p_klass);
   GtkWidgetClass *p_wc = GTK_WIDGET_CLASS(p_klass);
   p_oc->finalize       = ggaze_histogram_view_finalize;
   p_wc->measure        = ggaze_histogram_view_measure;
   p_wc->snapshot       = ggaze_histogram_view_snapshot;
   gtk_widget_class_set_css_name(p_wc, "histogram");
}

static void
ggaze_histogram_view_init(GgazeHistogramView *p_view) {
   (void)p_view;
}

GtkWidget *
ggaze_histogram_view_new(void) {
   return (g_object_new(GGAZE_TYPE_HISTOGRAM_VIEW, NULL));
}

void
ggaze_histogram_view_set_histogram(GgazeHistogramView *p_view,
                                   Histogram          *p_hist) {
   g_return_if_fail(GGAZE_IS_HISTOGRAM_VIEW(p_view));
   g_clear_pointer(&p_view->p_hist, histogram_delete);
   p_view->p_hist = p_hist;
   gtk_widget_queue_draw(GTK_WIDGET(p_view));
}

const Histogram *
ggaze_histogram_view_get_histogram(GgazeHistogramView *p_view) {
   g_return_val_if_fail(GGAZE_IS_HISTOGRAM_VIEW(p_view), NULL);
   return (p_view->p_hist);
}
