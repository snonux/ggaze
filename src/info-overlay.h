#ifndef GGAZE_INFO_OVERLAY_H
#define GGAZE_INFO_OVERLAY_H

/*:*
 * ggaze — info overlay + transient status line
 *
 * The one card floating over the grid/large stack: the EXIF/dimensions text
 * plus an RGB/luminance histogram of the displayed texture (`i`, both
 * gathered asynchronously in one GTask -- info.c for the metadata,
 * histogram.c for the binning -- so a big PNG/TIFF cannot freeze the UI)
 * and every transient status message ("Copied image", "Move to X failed:
 * ..."). It owns the card box (label + histogram plot), the auto-hide timer
 * and the in-flight request, so window.c only says "show status", "show info
 * for this file with this texture", "dismiss". Extracted from window.c
 * (SRP): the window was the only door into this logic and had grown a test
 * hook for the label because of it.
 *
 * The struct is reference counted internally: the async info request holds a
 * ref, so a window that closes mid-decode never leaves the completion
 * callback pointing at freed memory. info_overlay_dispose() (window dispose,
 * no widget touch afterwards) + info_overlay_delete() (window finalize) is the
 * lifecycle, mirroring save-gate/delete-confirm.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct InfoOverlay InfoOverlay;

/* Create the card (label + histogram plot) and add it as an overlay child of
 * p_overlay (top-left, hidden). The overlay widget is borrowed; it must
 * outlive the InfoOverlay's dispose. */
InfoOverlay *info_overlay_new(GtkOverlay *p_overlay);

/* Cancel the auto-hide timer and any in-flight info request, and stop
 * touching the label from now on (window dispose). */
void info_overlay_dispose(InfoOverlay *p_io);

/* Drop the owner's reference (window finalize, after dispose). */
void info_overlay_delete(InfoOverlay *p_io);

/* Show a transient status line. The auto-hide delay grows with the message
 * length (2 s for a short confirmation, up to 8 s for a long failure text)
 * so an error is readable before it disappears. */
void info_overlay_show_status(InfoOverlay *p_io, const char *c_msg);

/* Gather + show the EXIF/dimensions card for p_file, with a histogram of
 * p_tex when one is given (the texture the viewer is displaying for that
 * file; NULL -- grid view, still decoding -- shows the card without a plot).
 * Async; cancels any request still in flight, last-write-wins. Stays up for
 * 5 s. The texture is never binned before `i` is pressed, so the plot can
 * neither block the UI nor delay the first paint of the picture. */
void info_overlay_show_for_file(InfoOverlay *p_io, GFile *p_file,
                                GdkTexture *p_tex);

/* `i` semantics: hide the card if it is showing for p_file, else show it. */
void info_overlay_toggle_for_file(InfoOverlay *p_io, GFile *p_file,
                                  GdkTexture *p_tex);

/* Hide the label, cancel the timer and any in-flight request (navigation:
 * the previous file's card must never outlive the file it describes). */
void info_overlay_dismiss(InfoOverlay *p_io);

/* The label widget (borrowed) -- tests assert on its visibility/text. Its
 * own visible flag follows the card's, so "is the card up" reads the same
 * way it did when the label WAS the card. */
GtkWidget *info_overlay_get_label(InfoOverlay *p_io);

/* The histogram plot (a GgazeHistogramView, borrowed): visible only while
 * the card shows a file that had a displayed texture. Tests assert on its
 * visibility and on ggaze_histogram_view_get_histogram(). */
GtkWidget *info_overlay_get_histogram(InfoOverlay *p_io);

G_END_DECLS

#endif /* GGAZE_INFO_OVERLAY_H */
