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

void ggaze_grid_set_thumbnail_size(GgazeGrid *p_grid, int i_size);
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

/* Disconnect from the navigator (call before the navigator is freed). */
void ggaze_grid_detach(GgazeGrid *p_grid);

/* "activate": emitted when the user presses Enter or double-clicks a cell (the
 * window switches to the large view on the current file). */
guint ggaze_grid_activate_signal(void);

G_END_DECLS

#endif /* GGAZE_GRIDVIEW_H */