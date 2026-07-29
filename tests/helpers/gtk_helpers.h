/*:*
 * ggaze — shared GTK test helpers (AGENTS.md: "Shared helpers go in
 * tests/helpers/")
 *
 * Two groups, both needed by more than one suite:
 *
 *   - grid cell activation: reach the GtkFlowBox GgazeGrid keeps private
 *     inside its GtkScrolledWindow and emit "child-activated" on one of its
 *     cells (what a double-click / Enter on a thumbnail does). Needs no
 *     laid-out geometry, so callers never have to present a toplevel.
 *   - alert-dialog driving: find, count and press buttons on the dialogs
 *     gtk_alert_dialog_choose() puts up.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#ifndef GTK_HELPERS_H
#define GTK_HELPERS_H

#include <gtk/gtk.h>

#include "gridview.h"

/* --- grid cells ---------------------------------------------------------- */

/* Depth-first search for the GtkFlowBox inside p_w (GgazeGrid does not expose
 * its own). NULL if there is none. */
GtkFlowBox *ggtest_find_flow_box(GtkWidget *p_w);

/* Activate cell i_idx of p_grid exactly as a double-click / Enter on it
 * would: emits the flowbox's "child-activated", which gridview.c handles by
 * routing the cell's file through the installed select gate. Asserts the
 * flowbox and the cell exist. */
void ggtest_activate_cell(GgazeGrid *p_grid, gint i_idx);

/* --- alert dialogs -------------------------------------------------------- */

/* Iterate the main context for roughly u_ms milliseconds. */
void ggtest_drain_main(guint u_ms);

/* Depth-first search for a GtkButton labelled c_label below p_root. */
GtkWidget *ggtest_find_button(GtkWidget *p_root, const char *c_label);

/* The first toplevel other than p_skip that carries a button labelled
 * c_label, i.e. the dialog gtk_alert_dialog_choose() opened. NULL if none is
 * up. p_skip is the caller's own window (its popovers are part of its widget
 * tree and could otherwise match). */
GtkWindow *ggtest_find_dialog(GtkWindow *p_skip, const char *c_label);

/* How many such dialogs are live at once -- the check for "a second trigger
 * must not stack a second dialog". */
guint ggtest_count_dialogs(GtkWindow *p_skip, const char *c_label);

/* Iterate the main context until such a dialog appears or u_timeout_ms
 * elapses. Returns it, or NULL on timeout. */
GtkWindow *ggtest_wait_for_dialog(GtkWindow *p_skip, const char *c_label,
                                  guint u_timeout_ms);

/* Click p_btn by emitting "clicked" -- what a real pointer click ends in.
 * NOT gtk_widget_activate(): GtkButton turns that into a keyboard-activation
 * animation that only runs on a REALIZED widget and only emits "clicked" when
 * its timeout expires, so it silently does nothing for a button in a popover
 * of a window the test never presented. */
void ggtest_click_button(GtkWidget *p_btn);

/* TRUE while p_win is still a live toplevel. GTK4's gtk_window_destroy()
 * (which is also what an unhandled "close-request" ends in) hides and
 * unrealizes the window and takes it out of gtk_window_list_toplevels()
 * WITHOUT emitting anything the caller can watch for -- and it does not
 * finalize a window the test still holds a reference to -- so "did the
 * window actually close?" is asked this way. */
gboolean ggtest_is_open_toplevel(GtkWindow *p_win);

/* Press the button labelled c_label on the first such dialog, driving
 * gtk_alert_dialog_choose()'s async callback to completion (the dialog is an
 * ordinary GtkWindow, so gtk_widget_activate() on its button is enough --
 * no response-injection API is needed). Returns FALSE if no dialog with that
 * button is up. Does NOT iterate the main context; the caller drains. */
gboolean ggtest_click_dialog_button(GtkWindow *p_skip, const char *c_label);

#endif /* GTK_HELPERS_H */
