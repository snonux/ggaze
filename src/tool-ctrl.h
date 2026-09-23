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
 * follows the changing base, transform_rebase_crop, and is kept -- with a
 * status line, cropping nothing -- while the base has shrunk past it; `c`
 * over such a crop starts from the whole base, which is what is cropped in
 * effect), and cancel restores the transform the tool started from. The
 * one-shot `[` /
 * `]` turns need no tool and go straight to enhance_ctrl_rotate_quarter.
 *
 * Both overlays are about the pixels on screen, so both check that the
 * texture shown is exactly what the controller rendered for the state
 * being edited (enhance_ctrl_is_current_render, a pure identity test: the
 * controller learns the original from the window as it is shown, so no
 * texture-cache lookup runs per frame or per pointer motion, and a reload
 * that decodes the same file again is followed -- tool_ctrl_original_
 * changed lays the crop rectangle out again on it): the crop rectangle is
 * drawn over, and its drags measured on, no other picture, and a horizon
 * drag, a crop drag and the crop's Enter are refused while a render is
 * pending or Space holds the original ("Release Space first" wins over
 * "still rendering").
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

#include "croprect.h" /* CropRect, for the rectangle seam below */
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
 * it). Every other key is left alone -- also while the crop tool's base is
 * still unknown and its own keys only report "still rendering". FALSE with
 * no tool active. */
gboolean tool_ctrl_key(ToolCtrl *p_tc, guint u_keyval, GdkModifierType e_state);

/* A pointer drag on the viewer in widget coordinates (the viewer's overlay
 * drag callback ends here; tests call it directly). Crop: inside moves,
 * an edge or corner resizes -- a BEGIN refused while the base is not on
 * screen (a render pending, Space held) says why and grabs nothing, so a
 * later accepted UPDATE cannot continue a stale gesture; straighten: draws
 * the horizon line and, on END, levels it -- refused with a status line
 * under the same conditions. Ignored with no tool active. */
void tool_ctrl_drag(ToolCtrl *p_tc, GgazeViewerDragPhase e_phase, gdouble d_x,
                    gdouble d_y);

/* Enter: commit and leave the tool. FALSE (with a status, tool still
 * active) when the crop cannot be committed yet because the base preview
 * has not rendered or Space holds the original. */
gboolean tool_ctrl_apply(ToolCtrl *p_tc);

/* Esc: restore the transform the tool started from and leave. */
void tool_ctrl_cancel(ToolCtrl *p_tc);

/* The view left the large page under the tool: an Esc without the status
 * line -- the straighten tool's angle goes back to what it started from
 * (every nudge was committed only to preview it), the crop tool drops its
 * rectangle and base override so the crop already applied is back on the
 * preview. The window calls this BEFORE switching the stack, since the
 * restore commits through enhance_ctrl_set_transform, which brings the
 * large view up (a no-op while it still is). */
void tool_ctrl_abandon(ToolCtrl *p_tc);

/* The preview under the tool is being discarded (Esc outside the tool, `0`
 * / the Original card with the panel open, a failed render, the gate's
 * Discard, the slideshow): leave WITHOUT restoring or committing anything
 * -- the discard resets the transform and a crop tool's override together.
 * The enhance controller calls this through its host ops before it resets,
 * so no tool can outlive the state it was editing and re-apply it. */
void tool_ctrl_discarded(ToolCtrl *p_tc);

/* The navigator "changed" choke point: abandon iff the current file is no
 * longer the one the tool started on (a same-file rescan keeps it -- the
 * rectangle follows the reload through tool_ctrl_original_changed, since
 * at this point the rewritten file's decode has not landed yet). */
void tool_ctrl_nav_changed(ToolCtrl *p_tc);

/* The current file's original on screen is another texture object now (the
 * enhance controller learned it as the window showed it: the file's first
 * decode after the tool started, or its reload after a rewrite in place or
 * a discard): a crop tool lays its rectangle out again -- on the new base
 * size, refitted if that changed -- and both tools redraw. With no tool
 * active, nothing. Before this the rectangle stayed hidden, laid out on the
 * old base, until the first key or drag. */
void tool_ctrl_original_changed(ToolCtrl *p_tc);

/* The crop rectangle exactly as the overlay draws it right now: TRUE iff
 * the crop tool is active, its rectangle is laid out and the texture on
 * screen is the base it is laid out on (_draw_crop's own condition), with
 * the rectangle and that base's size in the out-params; FALSE whenever the
 * overlay is hidden. A test seam: a test cannot read a snapshot, and every
 * key or drag lays the rectangle out itself, so this is the only way to
 * check "drawn on the new base without any input" after a rewrite. */
gboolean tool_ctrl_get_crop_rect(ToolCtrl *p_tc, CropRect *p_rect,
                                 gint *p_base_w, gint *p_base_h);

G_END_DECLS

#endif /* GGAZE_TOOL_CTRL_H */
