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

/* Number of cells currently in the grid. */
guint ggaze_grid_get_count(GgazeGrid *p_grid);

/* Disconnect from the navigator (call before the navigator is freed). */
void ggaze_grid_detach(GgazeGrid *p_grid);

/* "activate": emitted when the user presses Enter or double-clicks a cell (the
 * window switches to the large view on the current file). */
guint ggaze_grid_activate_signal(void);

G_END_DECLS

#endif /* GGAZE_GRIDVIEW_H */