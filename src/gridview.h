#ifndef GGAZE_GRIDVIEW_H
#define GGAZE_GRIDVIEW_H

/*:*
 * ggaze — thumbnail grid view
 *
 * GgazeGrid : GtkWidget is the folder-overview grid: one cell per navigator
 * file, thumbnails decoded lazily (async, on realize) from the thumbnail cache,
 * resizable (+/-), trashed/deleted cells dimmed, marked cells badged,
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

/* Rebuild the cells from the navigator (call after structural changes). */
void ggaze_grid_refresh(GgazeGrid *p_grid);

/* Update each cell's mark badge in place from the navigator (no rebuild). */
void ggaze_grid_refresh_mark_badges(GgazeGrid *p_grid);

/* Sync navigator.current to the flowbox's currently-selected cell, so leaving
 * the grid (Enter / toggle-to-large) opens the highlighted image. */
gboolean ggaze_grid_sync_current(GgazeGrid *p_grid);

/* Move the grid cursor one row down (i_dy = +1) or up (i_dy = -1), updating
 * navigator.current so the header / large-view preview track the move. */
void ggaze_grid_move_cursor(GgazeGrid *p_grid, int i_dy);

/* Borrowed pointer to the currently-selected cell's file (NULL if none). The
 * pointer is owned by the cell; only valid while the cell lives. */
GFile *ggaze_grid_get_selected_file(GgazeGrid *p_grid);

/* Update one cell's "ggaze-marked" badge from the navigator's mark set,
 * without rebuilding the grid. No-op if the file's cell isn't present. */
void ggaze_grid_update_mark_badge(GgazeGrid *p_grid, GFile *p_file);

/* Number of cells currently in the grid. */
guint ggaze_grid_get_count(GgazeGrid *p_grid);

/* Toggle the mark on the cell at (i_x, i_y) in the grid's flowbox
 * coordinates, exactly as a middle-click does: select the cell, sync
 * navigator.current to it THROUGH the select gate above, then dispatch the
 * shared "win.mark" action. Returns TRUE iff a cell was found there.
 *
 * Public because that is the only way the middle-click path can be tested:
 * the gesture itself would need a synthesized pointer press at real
 * coordinates, and GTK4 removed both gtk_test_widget_click and the public
 * GdkEvent constructors that GTK3 tests used for it. The gesture callback is
 * a two-line wrapper around this. */
gboolean ggaze_grid_mark_at_pos(GgazeGrid *p_grid, gint i_x, gint i_y);

/* Disconnect from the navigator (call before the navigator is freed). */
void ggaze_grid_detach(GgazeGrid *p_grid);

/* "activate": emitted when the user presses Enter or double-clicks a cell (the
 * window switches to the large view on the current file). */
guint ggaze_grid_activate_signal(void);

G_END_DECLS

#endif /* GGAZE_GRIDVIEW_H */