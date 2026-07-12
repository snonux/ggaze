#ifndef GGAZE_VIEWER_H
#define GGAZE_VIEWER_H

/*:*
 * ggaze — large single-image viewer
 *
 * GgazeViewer : GtkWidget is the custom large-view canvas (decision #31, not
 * GtkPicture). It owns a GdkTexture plus zoom/pan/fit state and draws via GTK4
 * render nodes. Zoom is cursor-centered (mouse/pinch) or window-centered
 * (keys); panning clamps so the image can't drift off-screen. See
 * docs/ui-and-interactions.md "Zoom behavior" and docs/architecture.md
 * "Responsibilities / viewer".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk/gdk.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GGAZE_TYPE_VIEWER (ggaze_viewer_get_type())
G_DECLARE_FINAL_TYPE(GgazeViewer, ggaze_viewer, GGAZE, VIEWER, GtkWidget)

GtkWidget *ggaze_viewer_new(void);

/* Take p_texture (refs it; the caller still owns its own ref and should unref
 * when done). Resets to fit-to-window, clears pan. NULL clears the display. */
void ggaze_viewer_set_texture(GgazeViewer *p_viewer, GdkTexture *p_texture);
GdkTexture *ggaze_viewer_get_texture(GgazeViewer *p_viewer); /* (transfer none) */

/* Zoom + pan actions (also reachable via the on-widget controllers). */
void ggaze_viewer_zoom_in(GgazeViewer *p_viewer);
void ggaze_viewer_zoom_out(GgazeViewer *p_viewer);
void ggaze_viewer_toggle_fit_100(GgazeViewer *p_viewer);
void ggaze_viewer_fit(GgazeViewer *p_viewer);
void ggaze_viewer_pan(GgazeViewer *p_viewer, gdouble d_dx, gdouble d_dy);

G_END_DECLS

#endif /* GGAZE_VIEWER_H */