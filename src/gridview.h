#ifndef GGAZE_GRIDVIEW_H
#define GGAZE_GRIDVIEW_H

/*:*
 * ggaze — thumbnail grid view
 *
 * GgazeGrid : GtkWidget is the folder-overview grid: one cell per navigator
 * file, thumbnails decoded lazily (async, while visible) from the thumbnail
 * cache, resizable (+/-), trashed/deleted cells dimmed, marked cells badged,
 * Enter/double-click emits "activate" (grid->large). Cursor follows the
 * navigator. See docs/architecture.md "gridview" + docs/ui-and-interactions.md
 * "Grid view behavior".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "navigator.h"
#include "thumbnail.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GGAZE_TYPE_GRID (ggaze_grid_get_type())
G_DECLARE_FINAL_TYPE(GgazeGrid, ggaze_grid, GGAZE, GRID, GtkWidget)

GtkWidget *ggaze_grid_new(Navigator *p_nav, Thumbnail *p_thumb, int i_size,
                          gboolean b_hide_trashed);

/* Callback the grid calls instead of navigator_set_current_file() directly
 * for every selection path (double-click/Enter, middle-click mark, j/k
 * cursor move, toggle-to-large sync) -- see ggaze_grid_set_select_func. This
 * lets a window-level gate (Save/Discard/Cancel over an unsaved GEGL
 * enhance preview) run BEFORE the navigator's "changed" signal actually
 * fires, instead of the window finding out only after the fact and having
 * nothing left to prompt for (tu0 review round 2, issue 1). Must return TRUE
 * iff navigator.current changed SYNCHRONOUSLY as a result, mirroring
 * navigator_set_current_file's own contract; FALSE covers both "no-op" and
 * "deferred behind an async prompt" (the change still applies once the
 * prompt resolves). */
typedef gboolean (*GgazeGridSelectFunc)(GgazeGrid *p_grid, GFile *p_file,
                                        gpointer p_user_data);

/* Install the select gate (call once, right after ggaze_grid_new). Every
 * grid selection call site routes through fn when one is installed; falls
 * back to calling navigator_set_current_file() directly when none is (so a
 * grid built without a gate-aware owner still works, e.g. a future
 * standalone test). */
void ggaze_grid_set_select_func(GgazeGrid *p_grid, GgazeGridSelectFunc fn,
                                gpointer p_user_data);

void ggaze_grid_set_thumbnail_size(GgazeGrid *p_grid, int i_size);
int  ggaze_grid_get_thumbnail_size(GgazeGrid *p_grid);
void ggaze_grid_set_hide_trashed(GgazeGrid *p_grid, gboolean b_hide);

/* Bring the cells in line with the navigator's listing in place (call after
 * structural changes): cells of files still listed are kept, thumbnail
 * included. */
void ggaze_grid_refresh(GgazeGrid *p_grid);

/* Update each cell's mark badge in place from the navigator (no rebuild). */
void ggaze_grid_refresh_mark_badges(GgazeGrid *p_grid);

/* Sync navigator.current to the flowbox's currently-selected cell, so leaving
 * the grid (Enter / toggle-to-large) opens the highlighted image. */
gboolean ggaze_grid_sync_current(GgazeGrid *p_grid);

/* Move the grid cursor one row down (i_dy = +1) or up (i_dy = -1), updating
 * navigator.current so the header / large-view preview track the move. */
void ggaze_grid_move_cursor(GgazeGrid *p_grid, int i_dy);

/* Shift + a cursor key: move the highlight one cell (i_dx = -1 / +1) or one
 * row (i_dy = -1 / +1) and mark everything between the cell the run of
 * such moves started on and the new one; moving back unmarks again. */
void ggaze_grid_extend_selection(GgazeGrid *p_grid, int i_dx, int i_dy);

/* The same by one page: the rows that fit the viewport less one (at least
 * one), stopping at the grid's first / last row. */
void ggaze_grid_move_page(GgazeGrid *p_grid, int i_dy);

/* Borrowed pointer to the currently-selected cell's file (NULL if none). The
 * pointer is owned by the cell; only valid while the cell lives. */
GFile *ggaze_grid_get_selected_file(GgazeGrid *p_grid);

/* Update one cell's "ggaze-marked" badge from the navigator's mark set,
 * without rebuilding the grid. No-op if the file's cell isn't present. */
void ggaze_grid_update_mark_badge(GgazeGrid *p_grid, GFile *p_file);

/* Number of image cells currently in the grid (folder cells not counted). */
guint ggaze_grid_get_count(GgazeGrid *p_grid);

/* Toggle the mark on the cell at (i_x, i_y) in the grid's flowbox
 * coordinates, exactly as a middle-click does: select the cell, sync
 * navigator.current to it THROUGH the select gate above, then emit
 * "mark-requested". Returns TRUE iff a cell was found there.
 *
 * Public because that is the only way the middle-click path can be tested:
 * the gesture itself would need a synthesized pointer press at real
 * coordinates, and GTK4 removed both gtk_test_widget_click and the public
 * GdkEvent constructors that GTK3 tests used for it. The gesture callback is
 * a two-line wrapper around this. */
gboolean ggaze_grid_mark_at_pos(GgazeGrid *p_grid, gint i_x, gint i_y);

/* --- folder cells ---------------------------------------------------------
 *
 * Ahead of the image cells the grid shows one cell per sub-folder of the
 * navigator's folder and, when a parent is set, a ".." cell before those.
 * A folder cell is never navigator.current and cannot be marked; the
 * highlight may rest on one while current stays where it was. */

/* The folder the ".." cell leads to (refs it), or NULL for no such cell. */
void ggaze_grid_set_parent_folder(GgazeGrid *p_grid, GFile *p_parent);

/* Number of folder cells (".." included). */
guint ggaze_grid_get_folder_count(GgazeGrid *p_grid);

/* The folder of the highlighted cell (transfer none), NULL when the
 * highlight is on an image or nowhere. */
GFile *ggaze_grid_get_selected_folder(GgazeGrid *p_grid);

/* Put the highlight on p_folder's cell. FALSE when it has none. */
gboolean ggaze_grid_select_folder(GgazeGrid *p_grid, GFile *p_folder);

/* One linear step (i_dir < 0 back, else forward) of the highlight when a
 * folder cell is involved: from one, or from an image onto the folder cell
 * next to it. TRUE iff the grid took the step (also at the grid's edge,
 * where it stays put); FALSE for an image-to-image step, which is the
 * owner's prev/next. */
gboolean ggaze_grid_step_folder(GgazeGrid *p_grid, int i_dir);

/* Disconnect from the navigator (call before the navigator is freed). */
void ggaze_grid_detach(GgazeGrid *p_grid);

/* Signals (the grid names no window action; the owner connects these):
 *   "open-folder"         Enter / double-click on a folder cell; one GFile
 *                         argument, the folder to open
 *   "activate"            Enter / double-click on a cell (owner opens the
 *                         large view on the current file).
 *   "navigate" (gint dir) Left (-1) / Right (+1) pressed in the grid.
 *   "mark-requested"      middle-click on a cell (owner toggles the mark on
 *                         the selected cell).
 *   "mark-all-requested"  Ctrl+a pressed in the grid (GtkFlowBox would
 *                         otherwise swallow it as its own select-all).
 * A single click (or a GtkFlowBox Home/End/PageUp/PageDown move) syncs
 * navigator.current to the highlighted cell through the select gate, so the
 * highlighted cell is always the target of per-image actions. */

G_END_DECLS

#endif /* GGAZE_GRIDVIEW_H */
