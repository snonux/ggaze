#ifndef GGAZE_TOOL_CTRL_H
#define GGAZE_TOOL_CTRL_H

/*:*
 * ggaze — the interactive crop (c) and straighten (R) tools
 *
 * A modal editing session over the large view: while a tool is active it
 * draws its overlay on the viewer (the crop rectangle with its handles and
 * thirds, or the straighten grid and the horizon line being dragged), takes
 * the pointer drag over from panning, and claims a small set of keys
 * (docs/ui-and-interactions.md "Crop, straighten & rotate tools") until
 * Enter applies or Esc cancels. Everything it edits is the EnhanceCtrl's
 * Transform: the crop tool shows the base image (the committed transform
 * with the crop switched off) through enhance_ctrl_set_preview_transform --
 * an override of what is rendered, never a commit, so a crop already
 * applied keeps counting as work (`s` exports it, navigation prompts for
 * it) while its rectangle is adjusted -- and commits a rectangle laid out
 * on it; the straighten tool pushes every nudge / horizon drag through
 * enhance_ctrl_set_transform so the image levels live (a committed crop
 * follows the changing base, transform_rebase_crop, or is dropped with a
 * status line when nothing of it is left), and cancel restores the
 * transform the tool started from. The one-shot `[` / `]` turns need no
 * tool and go straight to enhance_ctrl_rotate_quarter.
 *
 * Geometry is delegated: croprect.c owns how the rectangle moves,
 * transform.c the angles and sizes, viewer.c the image-to-widget mapping.
 * This module is the glue -- state machine, key table, overlay painting --
 * and is GEGL-only like enhance-ctrl.c (no GEGL, no Transform to edit). It
 * reaches the window through a small host vtable, mirroring the other
 * controllers, so it can be driven by tests through the window's public
 * tool_key / tool_drag hooks without a real keyboard or pointer.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

#include "enhance-ctrl.h"
#include "ggaze-enums.h" /* GgazeTool */
#include "viewer.h"

G_BEGIN_DECLS

typedef struct ToolCtrl ToolCtrl;

/* Window-side operations. p_host is the window, borrowed per call; getters
 * return borrowed references. */
typedef struct {
   /* The large-view widget the overlay is drawn on. */
   GgazeViewer *(*get_viewer)(gpointer p_host);
   /* Transient status line (the window's info overlay): the tools narrate
    * their keys and the current angle here. */
   void (*show_status)(gpointer p_host, const char *c_msg);
   /* Bring the large view on screen (a tool only makes sense there). */
   void (*ensure_large_view)(gpointer p_host);
   /* The navigator's current file, NULL when none. */
   GFile *(*get_current_file)(gpointer p_host);
} ToolCtrlHostOps;

/* Construct a controller editing p_ec's transform. p_ops and p_ec are
 * borrowed for the controller's lifetime. */
ToolCtrl *tool_ctrl_new(EnhanceCtrl *p_ec, const ToolCtrlHostOps *p_ops,
                        gpointer p_host);
void      tool_ctrl_delete(ToolCtrl *p_tc);

/* Detach the overlay and forget the session (window dispose, before the
 * viewer goes). */
void tool_ctrl_dispose(ToolCtrl *p_tc);

/* Which tool is active (GGAZE_TOOL_NONE when none). */
GgazeTool tool_ctrl_get_tool(ToolCtrl *p_tc);

/* `c` / `R`: start the tool, or -- pressed again while that same tool is
 * active -- cancel it. With the OTHER tool active the key is refused with a
 * status line (finish that one first). A no-op with a status when no file is
 * open. */
void tool_ctrl_toggle_crop(ToolCtrl *p_tc);
void tool_ctrl_toggle_straighten(ToolCtrl *p_tc);

/* Offer a key press to the active tool. TRUE iff it consumed the key (the
 * caller then stops propagation so the global shortcuts never see it):
 * crop: h/l/j/k move, H/L/J/K resize the right/bottom edge, 1-4 aspect
 * 1:1 / 3:2 / 4:3 / 16:9, 0 free; straighten: h / - counter-clockwise and
 * l / + clockwise by 0.5 degrees, A toggles auto-crop; both: Enter applies,
 * Esc cancels, and c / R / [ / ] are answered with "finish the tool first"
 * (a tool switch or a turn under a laid-out rectangle would silently move
 * it). Every other key is left alone. FALSE with no tool active. */
gboolean tool_ctrl_key(ToolCtrl *p_tc, guint u_keyval, GdkModifierType e_state);

/* A pointer drag on the viewer in widget coordinates (the viewer's overlay
 * drag callback ends here; tests call it directly). Crop: inside moves,
 * an edge or corner resizes; straighten: draws the horizon line and, on
 * END, levels it. Ignored with no tool active. */
void tool_ctrl_drag(ToolCtrl *p_tc, GgazeViewerDragPhase e_phase, gdouble d_x,
                    gdouble d_y);

/* Enter: commit and leave the tool. FALSE (with a status, tool still
 * active) when the crop cannot be committed yet because the base preview
 * has not rendered. */
gboolean tool_ctrl_apply(ToolCtrl *p_tc);

/* Esc: restore the transform the tool started from and leave. */
void tool_ctrl_cancel(ToolCtrl *p_tc);

/* Leave without touching the transform -- the image under the tool changed
 * hands (navigation reset the preview, the view left the large page). */
void tool_ctrl_abandon(ToolCtrl *p_tc);

/* The navigator "changed" choke point: abandon iff the current file is no
 * longer the one the tool started on (a same-file rescan keeps it). */
void tool_ctrl_nav_changed(ToolCtrl *p_tc);

G_END_DECLS

#endif /* GGAZE_TOOL_CTRL_H */
