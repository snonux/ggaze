#ifndef GGAZE_EDIT_MODE_H
#define GGAZE_EDIT_MODE_H

/*:*
 * ggaze — the edit key modes: one router for the edit keys + the hint bar
 *
 * The edit panel (`a`, enhance-ctrl.c) and the crop / straighten tools
 * (tool-ctrl.c) each own some keys only while they are on screen: the
 * digits toggle presets only with the panel open, `h` moves the crop
 * rectangle only while the crop tool is up. This module is the ONE place
 * that decides which of them a key press goes to, before the window's
 * global shortcut table sees it (the window's capture-phase key controller
 * calls edit_mode_key):
 *
 *   1. a tool is active   -> the tool (its rows in shortcuts.c's table);
 *   2. the panel is open  -> the panel's own rows (shortcuts_mode_action),
 *      AND on screen         fired as their win.* actions (the panel is
 *                            hidden, not closed, beside the grid: its
 *                            digits are dead there, not aimed at an image
 *                            that is not on screen);
 *   3. otherwise, or when neither claims the key -> nothing here: the key
 *      goes on to the global table (so `h` is win.prev again the moment the
 *      tool ends, and a digit with the panel closed does nothing).
 *
 * Adding a panel key (an undo, a strength nudge on a selected card) is a
 * table row scoped to GGAZE_KEY_MODE_PANEL plus its action: this router
 * does not change.
 *
 * It also owns the KEY-HINT BAR: a strip under the large view, shown while
 * the panel or a tool is the key mode, listing that mode's live keys as
 * shortcuts_hint_for_mode generates them from the same table -- so the bar,
 * the `?` help, the F10 menu and the tooltips cannot drift apart.
 *
 * GEGL-only (like the two controllers it routes to). It owns no state of
 * its own beyond the widgets of the bar: the mode is read off the
 * controllers every time. The window packs the bar and calls
 * edit_mode_sync whenever the mode or the view may have changed (the
 * controllers' mode_changed host ops, a view switch).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gtk/gtk.h>

#include "enhance-ctrl.h"
#include "shortcuts.h"
#include "tool-ctrl.h"

G_BEGIN_DECLS

/* The CSS class on the hint bar's root, so styling and tests can find it. */
#define GGAZE_HINT_BAR_CLASS "ggaze-hint-bar"

typedef struct EditMode EditMode;

/* A router over p_ec (the panel) and p_tc (the tools), firing panel
 * actions on p_window. All three are borrowed for the router's lifetime.
 * Builds the (hidden, unparented) hint bar; the caller packs it. */
EditMode *edit_mode_new(GtkWidget *p_window, EnhanceCtrl *p_ec, ToolCtrl *p_tc);
void      edit_mode_delete(EditMode *p_em);

/* The hint bar's root widget (borrowed; the window's tree owns it once
 * packed). */
GtkWidget *edit_mode_get_hint_bar(EditMode *p_em);

/* The key mode in effect: a tool's, else PANEL while the panel is open
 * and the large view up (as of the last edit_mode_sync), else NONE. */
GgazeKeyMode edit_mode_get_mode(EditMode *p_em);

/* Route one key press (see the header). TRUE iff it was consumed. */
gboolean edit_mode_key(EditMode *p_em, guint u_keyval, GdkModifierType e_state);

/* Record whether the large view is up (b_large: the panel's keys are live
 * only then) and bring the hint bar in line with the mode: shown, with
 * that mode's keys -- the preset digits for the presets that exist --
 * while b_large and the mode is not NONE; hidden otherwise. The window
 * calls it on every view switch, so the router always knows the view.
 * Cheap when nothing changed. */
void edit_mode_sync(EditMode *p_em, gboolean b_large);

/* The bar's text as shown (plain, no markup), or NULL while it is hidden.
 * Caller frees. */
char *edit_mode_get_hint_text(EditMode *p_em);

G_END_DECLS

#endif /* GGAZE_EDIT_MODE_H */
