/*:*
 * ggaze — keybinding -> GAction map (single source of truth)
 *
 * The SHORTCUTS[] table binds keys to "win.*" actions (shortcuts_install) AND
 * drives the "?" help window (shortcuts_build_help), so the help can never
 * drift from the live keybindings. Each row carries a human-readable c_title
 * and a help c_group; entries sharing a title within a group merge into one
 * help row with a space-joined accelerator (h Left, 1 2 3 4 5 6 7 8, ...).
 * A row may add a short c_label for the F10 menu ("Crop" where the help
 * says "Crop tool (Enter applies, Esc cancels)").
 * Rows may also be scoped to a key mode (the edit panel, a tool) and name a
 * short hint for the modes' key-hint bars -- see shortcuts.h, "KEY MODES".
 * See docs/ui-and-interactions.md keybindings table.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "shortcuts.h"

#include <gtk/gtk.h>
#include <string.h>

typedef struct {
   guint           u_keyval;
   GdkModifierType e_mods;
   const char     *c_action; /* e.g. "win.next"; NULL for a help-only row or
                              * a tool key (answered through e_op) */
   const char *c_title;      /* help label; NULL hides the row from help */
   const char *c_group;      /* help group; NULL hides the row from help */
   guint       u_scope;      /* 0: a GLOBAL row (bound by shortcuts_install
                              * when it has an action); else the modes
                              * (GGAZE_KEY_MODE_BIT) whose router answers
                              * it -- never bound globally */
   guint       u_hints;      /* the modes whose key-hint bar lists it */
   const char *c_hint;       /* its short label there ("crop", "move") */
   guint       u_rank;       /* its place in those bars (ascending); rows
                              * that sort next to each other with the same
                              * c_hint merge into one segment */
   GgazeKeyOp  e_op;         /* a tool row's meaning (tool-ctrl.c) */
   const char *c_label;      /* the short menu label ("Crop"); NULL: the
                              * menu uses c_title */
   guint u_preset;           /* a preset digit row's preset number (1-8),
                              * 0 otherwise: the hint bar lists it only
                              * while that many presets exist */
} ShortcutEntry;

/* Mode masks, to keep the rows below readable. */
#define _PANEL GGAZE_KEY_MODE_BIT(GGAZE_KEY_MODE_PANEL)
#define _CROP GGAZE_KEY_MODE_BIT(GGAZE_KEY_MODE_CROP)
#define _STR GGAZE_KEY_MODE_BIT(GGAZE_KEY_MODE_STRAIGHTEN)

/* A GLOBAL row the edit panel's hint bar also lists. */
#define _PANEL_HINT(hint, rank)                                                \
   .u_hints = _PANEL, .c_hint = (hint), .u_rank = (rank)

/* One preset digit: scoped to the open panel (the cards are its GUI), so a
 * digit with the panel closed does nothing edit-related. */
#define _PRESET_ROW(key, n)                                                    \
   {key,                                                                       \
    0,                                                                         \
    "win.enhance-" #n,                                                         \
    "Toggle preset 1-8 (layered; panel open)",                                 \
    "Edit panel",                                                              \
    .u_scope  = _PANEL,                                                        \
    .u_hints  = _PANEL,                                                        \
    .c_hint   = "presets",                                                     \
    .u_rank   = 10,                                                            \
    .u_preset = (n)}

/* A tool key: no action -- the tool answers it by its op while it is the
 * mode on screen -- and listed in that tool's hint bar. */
#define _TOOL_ROW(key, mods, title, group, mode, hint, rank, op)               \
   {key,                                                                       \
    mods,                                                                      \
    NULL,                                                                      \
    title,                                                                     \
    group,                                                                     \
    .u_scope = (mode),                                                         \
    .u_hints = (mode),                                                         \
    .c_hint  = (hint),                                                         \
    .u_rank  = (rank),                                                         \
    .e_op    = (op)}

/* A key of the open edit panel that fires a win.* action (the panel's
 * rows beside the digits), in the "Edit panel" help group; hint NULL
 * leaves it out of the bar (a second key for the same thing). */
#define _PANEL_ROW(key, mods, action, title, hint, rank, label)                \
   {key,                                                                       \
    mods,                                                                      \
    action,                                                                    \
    title,                                                                     \
    "Edit panel",                                                              \
    .u_scope = _PANEL,                                                         \
    .u_hints = _PANEL,                                                         \
    .c_hint  = (hint),                                                         \
    .u_rank  = (rank),                                                         \
    .c_label = (label)}

#define _UNDO_TITLE "Undo the last edit step (panel open)"
#define _REDO_TITLE "Redo the edit step undone last (panel open)"

#define _CROP_GROUP "Crop tool (c)"
#define _STR_GROUP "Straighten tool (r)"

/* Ordered by help group so shortcuts_build_help can render groups in this
 * order; within a group, rows that share a title (and so merge into one help
 * row) are kept adjacent. The binding controller does not depend on this
 * order -- triggers are distinct -- so reordering is safe; the hint bars
 * order by u_rank, not by position. */
/* Keyboard first (decision #7): every action has a vi-style key, listed
 * first, AND a traditional one (cursor keys, Home/End, PageUp/PageDown,
 * Delete, F-keys, Ctrl chords) so both habits work; the help merges the two
 * into one row per action. */
/* The plain global rows leave the mode fields out (zero: global, no hint,
 * no op) rather than spell five zeros on every line; -Wextra's
 * missing-field-initializers would flag each of them, so it is silenced
 * for the table alone. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const ShortcutEntry SHORTCUTS[] = {
   /* Navigation */
   {GDK_KEY_h, 0, "win.prev", "Previous image", "Navigation"},
   {GDK_KEY_Left, 0, "win.prev", "Previous image", "Navigation"},
   {GDK_KEY_Page_Up, 0, "win.prev", "Previous image", "Navigation"},
   {GDK_KEY_l, 0, "win.next", "Next image", "Navigation"},
   {GDK_KEY_Right, 0, "win.next", "Next image", "Navigation"},
   {GDK_KEY_Page_Down, 0, "win.next", "Next image", "Navigation"},
   {GDK_KEY_j, 0, "win.cursor-down", "Cursor down one row (grid) / pan down",
    "Navigation"},
   {GDK_KEY_Down, 0, "win.cursor-down", "Cursor down one row (grid) / pan down",
    "Navigation"},
   {GDK_KEY_k, 0, "win.cursor-up", "Cursor up one row (grid) / pan up",
    "Navigation"},
   {GDK_KEY_Up, 0, "win.cursor-up", "Cursor up one row (grid) / pan up",
    "Navigation"},
   {GDK_KEY_H, GDK_SHIFT_MASK, "win.pan-left", "Pan left (large view)",
    "Navigation"},
   {GDK_KEY_Left, GDK_SHIFT_MASK, "win.pan-left", "Pan left (large view)",
    "Navigation"},
   {GDK_KEY_L, GDK_SHIFT_MASK, "win.pan-right", "Pan right (large view)",
    "Navigation"},
   {GDK_KEY_Right, GDK_SHIFT_MASK, "win.pan-right", "Pan right (large view)",
    "Navigation"},
   {GDK_KEY_g, 0, "win.first", "First image", "Navigation"},
   {GDK_KEY_Home, 0, "win.first", "First image", "Navigation"},
   {GDK_KEY_G, GDK_SHIFT_MASK, "win.last", "Last image", "Navigation"},
   {GDK_KEY_End, 0, "win.last", "Last image", "Navigation"},
   /* View */
   {GDK_KEY_t, 0, "win.toggle-view", "Toggle large / grid", "View"},
   {GDK_KEY_Return, 0, "win.enter-large", "Open highlighted image (grid)",
    "View"},
   {GDK_KEY_f, 0, "win.fullscreen", "Fullscreen", "View"},
   {GDK_KEY_F11, 0, "win.fullscreen", "Fullscreen", "View"},
   {GDK_KEY_S, GDK_SHIFT_MASK, "win.slideshow", "Start / stop slideshow",
    "View"},
   {GDK_KEY_F5, 0, "win.slideshow", "Start / stop slideshow", "View"},
   {GDK_KEY_i, 0, "win.info", "Toggle info overlay", "View"},
   {GDK_KEY_F10, 0, "win.menu", "Main menu", "View"},
   {GDK_KEY_comma, 0, "win.preferences", "Preferences", "View"},
   {GDK_KEY_comma, GDK_CONTROL_MASK, "win.preferences", "Preferences", "View"},
   /* Selection (marks) */
   {GDK_KEY_v, 0, "win.mark", "Toggle mark on highlighted",
    "Selection (marks)"},
   {GDK_KEY_V, GDK_SHIFT_MASK, "win.mark-range",
    "Range-mark from last mark to current", "Selection (marks)"},
   {GDK_KEY_a, GDK_CONTROL_MASK, "win.mark-all", "Mark all",
    "Selection (marks)"},
   /* Esc never discards an edit: it closes the panel and keeps the
    * preview (x reverts; the gate asks on navigate / quit). */
   {GDK_KEY_Escape, 0, "win.back",
    "Stop slideshow / close the edit panel (keeps the edit) / leave "
    "fullscreen / clear marks / back to grid / quit (twice in the grid)",
    "Selection (marks)", _PANEL_HINT(SHORTCUTS_HINT_CLOSE, 91)},
   /* Files */
   {GDK_KEY_o, 0, "win.open", "Open image", "Files"},
   {GDK_KEY_o, GDK_CONTROL_MASK, "win.open", "Open image", "Files"},
   {GDK_KEY_O, GDK_SHIFT_MASK, "win.open-folder", "Open folder", "Files"},
   {GDK_KEY_O, GDK_CONTROL_MASK | GDK_SHIFT_MASK, "win.open-folder",
    "Open folder", "Files"},
   {GDK_KEY_e, 0, "win.open-external", "Open in external program", "Files"},
   {GDK_KEY_exclam, 0, "win.run-script", "Run a configured shell script",
    "Files"},
   {GDK_KEY_m, 0, "win.move", "Move marked/current to a destination", "Files"},
   {GDK_KEY_d, 0, "win.trash", "Trash", "Files"},
   {GDK_KEY_Delete, 0, "win.trash", "Trash", "Files"},
   {GDK_KEY_D, GDK_SHIFT_MASK, "win.delete", "Delete permanently", "Files"},
   {GDK_KEY_Delete, GDK_SHIFT_MASK, "win.delete", "Delete permanently",
    "Files"},
   {GDK_KEY_u, 0, "win.undo", "Undo last trash or move", "Files"},
   {GDK_KEY_z, GDK_CONTROL_MASK, "win.undo", "Undo last trash or move",
    "Files"},
   {GDK_KEY_E, GDK_SHIFT_MASK, "win.empty-trash",
    "Empty this folder's Trash (asks first)", "Files"},
   {GDK_KEY_c, GDK_CONTROL_MASK, "win.copy", "Copy image / marked files",
    "Files"},
   {GDK_KEY_q, 0, "win.quit", "Quit", "Files"},
   {GDK_KEY_q, GDK_CONTROL_MASK, "win.quit", "Quit", "Files"},
   /* Edit panel (GEGL; docs/ui-and-interactions.md "The edit panel"). `a`
    * opens the one panel every edit lives in; c / r / [ / ] open it first
    * when it is closed, then act; the digits and x belong to it alone. */
   {GDK_KEY_a, 0, "win.enhance",
    "Open / close the edit panel (presets, crop, straighten, rotate, save)",
    "Edit panel", _PANEL_HINT(SHORTCUTS_HINT_CLOSE, 90),
    .c_label = "Edit panel"},
   _PRESET_ROW(GDK_KEY_1, 1),
   _PRESET_ROW(GDK_KEY_2, 2),
   _PRESET_ROW(GDK_KEY_3, 3),
   _PRESET_ROW(GDK_KEY_4, 4),
   _PRESET_ROW(GDK_KEY_5, 5),
   _PRESET_ROW(GDK_KEY_6, 6),
   _PRESET_ROW(GDK_KEY_7, 7),
   _PRESET_ROW(GDK_KEY_8, 8),
   {GDK_KEY_c, 0, "win.crop", "Crop tool (Enter applies, Esc cancels)",
    "Edit panel", _PANEL_HINT("crop", 20), .c_label = "Crop"},
   {GDK_KEY_r, 0, "win.straighten",
    "Straighten tool (Enter applies, Esc cancels)", "Edit panel",
    _PANEL_HINT("straighten", 21), .c_label = "Straighten"},
   {GDK_KEY_bracketleft, 0, "win.rotate-ccw", "Rotate 90° counter-clockwise",
    "Edit panel", _PANEL_HINT("rotate", 22), .c_label = "Rotate left"},
   {GDK_KEY_bracketright, 0, "win.rotate-cw",
    "Rotate 90° clockwise (repeat for 180 / 270)", "Edit panel",
    _PANEL_HINT("rotate", 23), .c_label = "Rotate right"},
   {GDK_KEY_s, 0, "win.enhance-save",
    "Save an edited copy (never the original)", "Edit panel",
    _PANEL_HINT("save copy", 30), .c_label = "Save edited copy"},
   {GDK_KEY_s, GDK_CONTROL_MASK, "win.enhance-save",
    "Save an edited copy (never the original)", "Edit panel"},
   {GDK_KEY_x, 0, "win.edit-revert", "Revert every edit (panel open)",
    "Edit panel", _PANEL_HINT("revert", 31), .c_label = "Revert all edits"},
   /* Undo / redo of edit steps (7i2): the open panel's own u and Ctrl+z,
    * which shadow the global file undo only while it is the key mode --
    * with the panel closed or hidden they are win.undo again. One hint
    * segment for both ("u/Shift+u undo/redo"). */
   _PANEL_ROW(GDK_KEY_u, 0, "win.edit-undo", _UNDO_TITLE, "undo/redo", 32,
              "Undo edit"),
   _PANEL_ROW(GDK_KEY_z, GDK_CONTROL_MASK, "win.edit-undo", _UNDO_TITLE, NULL,
              32, "Undo edit"),
   _PANEL_ROW(GDK_KEY_U, GDK_SHIFT_MASK, "win.edit-redo", _REDO_TITLE,
              "undo/redo", 33, "Redo edit"),
   _PANEL_ROW(GDK_KEY_Z, GDK_CONTROL_MASK | GDK_SHIFT_MASK, "win.edit-redo",
              _REDO_TITLE, NULL, 33, "Redo edit"),
   {GDK_KEY_space, 0, NULL, "Hold to compare with the original", "Edit panel",
    _PANEL_HINT("hold: original", 40)},
   /* Crop tool: modal -- answered by the tool (edit-mode.c routes the key
    * to tool-ctrl.c, which switches on e_op) while it is active and free
    * otherwise (`h` is win.prev the moment it ends). All four sides are
    * symmetric: Shift grows the side the key points at, Ctrl shrinks it. */
   _TOOL_ROW(GDK_KEY_h, 0, "Move the rectangle", _CROP_GROUP, _CROP, "move", 10,
             GGAZE_KEY_OP_CROP_MOVE_LEFT),
   _TOOL_ROW(GDK_KEY_j, 0, "Move the rectangle", _CROP_GROUP, _CROP, "move", 11,
             GGAZE_KEY_OP_CROP_MOVE_DOWN),
   _TOOL_ROW(GDK_KEY_k, 0, "Move the rectangle", _CROP_GROUP, _CROP, "move", 12,
             GGAZE_KEY_OP_CROP_MOVE_UP),
   _TOOL_ROW(GDK_KEY_l, 0, "Move the rectangle", _CROP_GROUP, _CROP, "move", 13,
             GGAZE_KEY_OP_CROP_MOVE_RIGHT),
   _TOOL_ROW(GDK_KEY_H, GDK_SHIFT_MASK, "Grow that side outward", _CROP_GROUP,
             _CROP, "grow", 20, GGAZE_KEY_OP_CROP_GROW_LEFT),
   _TOOL_ROW(GDK_KEY_J, GDK_SHIFT_MASK, "Grow that side outward", _CROP_GROUP,
             _CROP, "grow", 21, GGAZE_KEY_OP_CROP_GROW_BOTTOM),
   _TOOL_ROW(GDK_KEY_K, GDK_SHIFT_MASK, "Grow that side outward", _CROP_GROUP,
             _CROP, "grow", 22, GGAZE_KEY_OP_CROP_GROW_TOP),
   _TOOL_ROW(GDK_KEY_L, GDK_SHIFT_MASK, "Grow that side outward", _CROP_GROUP,
             _CROP, "grow", 23, GGAZE_KEY_OP_CROP_GROW_RIGHT),
   _TOOL_ROW(GDK_KEY_h, GDK_CONTROL_MASK, "Shrink that side inward",
             _CROP_GROUP, _CROP, "shrink", 30, GGAZE_KEY_OP_CROP_SHRINK_LEFT),
   _TOOL_ROW(GDK_KEY_j, GDK_CONTROL_MASK, "Shrink that side inward",
             _CROP_GROUP, _CROP, "shrink", 31, GGAZE_KEY_OP_CROP_SHRINK_BOTTOM),
   _TOOL_ROW(GDK_KEY_k, GDK_CONTROL_MASK, "Shrink that side inward",
             _CROP_GROUP, _CROP, "shrink", 32, GGAZE_KEY_OP_CROP_SHRINK_TOP),
   _TOOL_ROW(GDK_KEY_l, GDK_CONTROL_MASK, "Shrink that side inward",
             _CROP_GROUP, _CROP, "shrink", 33, GGAZE_KEY_OP_CROP_SHRINK_RIGHT),
   _TOOL_ROW(GDK_KEY_a, 0,
             "Cycle the aspect: free, 1:1, 3:2, 4:3, 16:9, original",
             _CROP_GROUP, _CROP, "aspect", 40, GGAZE_KEY_OP_CROP_ASPECT),
   _TOOL_ROW(GDK_KEY_Return, 0, "Apply the crop", _CROP_GROUP, _CROP, "apply",
             80, GGAZE_KEY_OP_TOOL_APPLY),
   _TOOL_ROW(GDK_KEY_KP_Enter, 0, "Apply the crop", _CROP_GROUP, _CROP, NULL,
             81, GGAZE_KEY_OP_TOOL_APPLY),
   _TOOL_ROW(GDK_KEY_Escape, 0, "Cancel (restore what the tool started from)",
             _CROP_GROUP, _CROP, "cancel", 90, GGAZE_KEY_OP_TOOL_CANCEL),
   /* Straighten tool: modal like the crop tool. */
   _TOOL_ROW(GDK_KEY_h, 0, "Nudge ½° counter-clockwise", _STR_GROUP, _STR,
             "nudge ½°", 10, GGAZE_KEY_OP_STRAIGHTEN_CCW),
   _TOOL_ROW(GDK_KEY_minus, 0, "Nudge ½° counter-clockwise", _STR_GROUP, _STR,
             "nudge ½°", 12, GGAZE_KEY_OP_STRAIGHTEN_CCW),
   _TOOL_ROW(GDK_KEY_underscore, 0, "Nudge ½° counter-clockwise", _STR_GROUP,
             _STR, NULL, 14, GGAZE_KEY_OP_STRAIGHTEN_CCW),
   _TOOL_ROW(GDK_KEY_l, 0, "Nudge ½° clockwise", _STR_GROUP, _STR, "nudge ½°",
             11, GGAZE_KEY_OP_STRAIGHTEN_CW),
   _TOOL_ROW(GDK_KEY_plus, 0, "Nudge ½° clockwise", _STR_GROUP, _STR,
             "nudge ½°", 13, GGAZE_KEY_OP_STRAIGHTEN_CW),
   _TOOL_ROW(GDK_KEY_equal, 0, "Nudge ½° clockwise", _STR_GROUP, _STR, NULL, 15,
             GGAZE_KEY_OP_STRAIGHTEN_CW),
   _TOOL_ROW(GDK_KEY_a, 0, "Toggle the auto-crop of the rotated corners",
             _STR_GROUP, _STR, "auto-crop", 20,
             GGAZE_KEY_OP_STRAIGHTEN_AUTOCROP),
   _TOOL_ROW(GDK_KEY_Return, 0, "Apply the angle", _STR_GROUP, _STR, "apply",
             80, GGAZE_KEY_OP_TOOL_APPLY),
   _TOOL_ROW(GDK_KEY_KP_Enter, 0, "Apply the angle", _STR_GROUP, _STR, NULL, 81,
             GGAZE_KEY_OP_TOOL_APPLY),
   _TOOL_ROW(GDK_KEY_Escape, 0, "Cancel (back to the starting angle)",
             _STR_GROUP, _STR, "cancel", 90, GGAZE_KEY_OP_TOOL_CANCEL),
   /* Zoom: `0` is zoom only -- it no longer reverts edits (that is x). */
   {GDK_KEY_plus, 0, "win.zoom-in", "Zoom in (large) / bigger thumbnails",
    "Zoom"},
   {GDK_KEY_equal, 0, "win.zoom-in", "Zoom in (large) / bigger thumbnails",
    "Zoom"},
   {GDK_KEY_minus, 0, "win.zoom-out", "Zoom out (large) / smaller thumbnails",
    "Zoom"},
   {GDK_KEY_underscore, 0, "win.zoom-out",
    "Zoom out (large) / smaller thumbnails", "Zoom"},
   {GDK_KEY_plus, GDK_CONTROL_MASK, "win.zoom-in",
    "Zoom in (large) / bigger thumbnails", "Zoom"},
   {GDK_KEY_minus, GDK_CONTROL_MASK, "win.zoom-out",
    "Zoom out (large) / smaller thumbnails", "Zoom"},
   {GDK_KEY_0, 0, "win.zoom-reset",
    "Toggle fit / 100% (large), reset thumbnail size (grid)", "Zoom"},
   {GDK_KEY_0, GDK_CONTROL_MASK, "win.zoom-reset",
    "Toggle fit / 100% (large), reset thumbnail size (grid)", "Zoom"},
   /* Help */
   {GDK_KEY_question, 0, "win.shortcuts", "Show this help", "Help"},
   {GDK_KEY_F1, 0, "win.shortcuts", "Show this help", "Help"},
};
#pragma GCC diagnostic pop

void
shortcuts_install(GtkWidget *p_widget) {
   g_return_if_fail(GTK_IS_WIDGET(p_widget));
   GtkEventController *p_ctrl = gtk_shortcut_controller_new();
   /* GLOBAL scope: the viewer installs its own GtkEventControllerKey that
    * consumes key events before a MANAGED-scope window controller would see
    * them. GLOBAL-scope shortcuts are consulted for every key event at the
    * toplevel first, so the win.* bindings fire regardless of which child has
    * focus. Note: this is the right scope while the app has no text-entry
    * widgets; if a search entry / settings text field is added later, bare
    * letter shortcuts (h/l/g/o/d/u/t/f/i/...) would intercept typing, and the
    * dispatch will need to skip editable/IM-context focus or revisit scope. */
   gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(p_ctrl),
                                     GTK_SHORTCUT_SCOPE_GLOBAL);
   for (gsize u_i = 0; u_i < G_N_ELEMENTS(SHORTCUTS); u_i++) {
      if (SHORTCUTS[u_i].c_action == NULL || SHORTCUTS[u_i].u_scope != 0) {
         continue; /* help-only (hold-Space), or a mode's key (edit-mode.c) */
      }
      GtkShortcut *p_s =
         gtk_shortcut_new(GTK_SHORTCUT_TRIGGER(gtk_keyval_trigger_new(
                             SHORTCUTS[u_i].u_keyval, SHORTCUTS[u_i].e_mods)),
                          gtk_named_action_new(SHORTCUTS[u_i].c_action));
      gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(p_ctrl),
                                           p_s);
   }
   gtk_widget_add_controller(p_widget, GTK_EVENT_CONTROLLER(p_ctrl));
}

const char *
shortcuts_title_for_action(const char *c_action) {
   g_return_val_if_fail(c_action != NULL, NULL);
   for (gsize u_i = 0; u_i < G_N_ELEMENTS(SHORTCUTS); u_i++) {
      if (g_strcmp0(SHORTCUTS[u_i].c_action, c_action) == 0) {
         return (SHORTCUTS[u_i].c_title);
      }
   }
   return (NULL);
}

const char *
shortcuts_label_for_action(const char *c_action) {
   g_return_val_if_fail(c_action != NULL, NULL);
   for (gsize u_i = 0; u_i < G_N_ELEMENTS(SHORTCUTS); u_i++) {
      if (g_strcmp0(SHORTCUTS[u_i].c_action, c_action) == 0 &&
          SHORTCUTS[u_i].c_label != NULL) {
         return (SHORTCUTS[u_i].c_label);
      }
   }
   return (shortcuts_title_for_action(c_action));
}

char *
shortcuts_keys_for_action(const char *c_action) {
   g_return_val_if_fail(c_action != NULL, NULL);
   GString *p_out = g_string_new(NULL);
   for (gsize u_i = 0; u_i < G_N_ELEMENTS(SHORTCUTS); u_i++) {
      if (g_strcmp0(SHORTCUTS[u_i].c_action, c_action) != 0) {
         continue;
      }
      char *c_label = gtk_accelerator_get_label(SHORTCUTS[u_i].u_keyval,
                                                SHORTCUTS[u_i].e_mods);
      if (p_out->len > 0) {
         g_string_append(p_out, " / ");
      }
      g_string_append(p_out, c_label);
      g_free(c_label);
   }
   if (p_out->len == 0) {
      g_string_free(p_out, TRUE);
      return (NULL);
   }
   return (g_string_free(p_out, FALSE));
}

char *
shortcuts_tooltip_for_action(const char *c_action) {
   const char *c_title = shortcuts_title_for_action(c_action);
   char       *c_keys  = shortcuts_keys_for_action(c_action);
   if (c_title == NULL) {
      return (c_keys);
   }
   char *c_tip = c_keys != NULL ? g_strdup_printf("%s (%s)", c_title, c_keys)
                                : g_strdup(c_title);
   g_free(c_keys);
   return (c_tip);
}

/* --- key modes: matching ------------------------------------------------- */

/* The modifiers a row and a key press are compared on. Shift is handled
 * apart (_shift_matches); Caps Lock (_unlocked_keyval), NumLock and the
 * pointer buttons never count. */
#define _CHORD_MASK (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)

/* TRUE iff u_keyval is a letter (it has two cases). */
static gboolean
_is_letter(guint u_keyval) {
   guint u_lower = gdk_keyval_to_lower(u_keyval);
   return (gdk_keyval_to_upper(u_lower) != u_lower);
}

/* TRUE iff the letter u_keyval is shifted: by the modifier, or by arriving
 * upper-cased (what a real Shift+h press delivers). */
static gboolean
_letter_shifted(guint u_keyval, GdkModifierType e_mods) {
   return ((e_mods & GDK_SHIFT_MASK) != 0 ||
           u_keyval != gdk_keyval_to_lower(u_keyval));
}

/* Shift is compared only for letters, where it is the difference between
 * `h` (move) and `Shift+h` (grow) whether the keyval arrives upper-cased
 * (`H`, a real key press) or with the modifier (`h` + Shift). For any other
 * key the layout decides what Shift produces (`+`, `?`), so it is part of
 * the keyval and the state's Shift bit says nothing more. */
static gboolean
_shift_matches(const ShortcutEntry *p_row, guint u_keyval,
               GdkModifierType e_state) {
   if (!_is_letter(u_keyval)) {
      return (TRUE);
   }
   return (_letter_shifted(u_keyval, e_state) ==
           _letter_shifted(p_row->u_keyval, p_row->e_mods));
}

/* TRUE iff a press of u_keyval with e_state is p_row's key. */
static gboolean
_row_matches(const ShortcutEntry *p_row, guint u_keyval,
             GdkModifierType e_state) {
   if (gdk_keyval_to_lower(p_row->u_keyval) != gdk_keyval_to_lower(u_keyval)) {
      return (FALSE);
   }
   if ((p_row->e_mods & _CHORD_MASK) != (e_state & _CHORD_MASK)) {
      return (FALSE);
   }
   return (_shift_matches(p_row, u_keyval, e_state));
}

/* Caps Lock without Shift delivers a letter upper-cased (`H` + Lock for a
 * plain `h` press), which _letter_shifted would read as Shift+h: the crop
 * tool's `h` grew the rectangle instead of moving it, and its `a` fell
 * through to the global `a` and closed the panel under the tool. Such a
 * press is the unshifted letter, so match its lower case. Shift with Caps
 * Lock is still Shift (the modifier says so, whatever case the keyval
 * arrives in), and the Lock bit itself never counts (_CHORD_MASK). */
static guint
_unlocked_keyval(guint u_keyval, GdkModifierType e_state) {
   if ((e_state & GDK_LOCK_MASK) != 0 && (e_state & GDK_SHIFT_MASK) == 0) {
      return (gdk_keyval_to_lower(u_keyval));
   }
   return (u_keyval);
}

/* The first row scoped to e_mode (0: a global row) that u_keyval + e_state
 * matches and that has an action (b_action) or an op (!b_action). */
static const ShortcutEntry *
_find_row(GgazeKeyMode e_mode, gboolean b_action, guint u_keyval,
          GdkModifierType e_state) {
   guint u_bit = e_mode == GGAZE_KEY_MODE_NONE ? 0 : GGAZE_KEY_MODE_BIT(e_mode);
   u_keyval    = _unlocked_keyval(u_keyval, e_state);
   for (gsize u_i = 0; u_i < G_N_ELEMENTS(SHORTCUTS); u_i++) {
      const ShortcutEntry *p_row = &SHORTCUTS[u_i];
      gboolean             b_scoped =
         u_bit == 0 ? p_row->u_scope == 0 : (p_row->u_scope & u_bit) != 0;
      gboolean b_has =
         b_action ? p_row->c_action != NULL : p_row->e_op != GGAZE_KEY_OP_NONE;
      if (b_scoped && b_has && _row_matches(p_row, u_keyval, e_state)) {
         return (p_row);
      }
   }
   return (NULL);
}

const char *
shortcuts_mode_action(GgazeKeyMode e_mode, guint u_keyval,
                      GdkModifierType e_state) {
   if (e_mode == GGAZE_KEY_MODE_NONE) {
      return (NULL); /* a global key is the shortcut controller's */
   }
   const ShortcutEntry *p_row = _find_row(e_mode, TRUE, u_keyval, e_state);
   return (p_row != NULL ? p_row->c_action : NULL);
}

GgazeKeyOp
shortcuts_mode_op(GgazeKeyMode e_mode, guint u_keyval,
                  GdkModifierType e_state) {
   if (e_mode == GGAZE_KEY_MODE_NONE) {
      return (GGAZE_KEY_OP_NONE);
   }
   const ShortcutEntry *p_row = _find_row(e_mode, FALSE, u_keyval, e_state);
   return (p_row != NULL ? p_row->e_op : GGAZE_KEY_OP_NONE);
}

const char *
shortcuts_global_action(guint u_keyval, GdkModifierType e_state) {
   const ShortcutEntry *p_row =
      _find_row(GGAZE_KEY_MODE_NONE, TRUE, u_keyval, e_state);
   return (p_row != NULL ? p_row->c_action : NULL);
}

/* --- key modes: labels and hint bars ------------------------------------- */

/* The modifier prefix ("Ctrl+", "Shift+", ...) of a key as the hint bar
 * prints it, appended to p_out. A letter's Shift may be carried by its
 * upper-case keyval alone. */
static void
_append_mods(GString *p_out, guint u_keyval, GdkModifierType e_mods) {
   if ((e_mods & GDK_CONTROL_MASK) != 0) {
      g_string_append(p_out, "Ctrl+");
   }
   if ((e_mods & GDK_ALT_MASK) != 0) {
      g_string_append(p_out, "Alt+");
   }
   if ((e_mods & GDK_SHIFT_MASK) != 0 ||
       (_is_letter(u_keyval) && gdk_keyval_to_lower(u_keyval) != u_keyval)) {
      g_string_append(p_out, "Shift+");
   }
}

/* The key itself without modifiers, appended to p_out: a letter in lower
 * case, the few named keys by the name on the keycap, anything printable
 * as its character, the rest by its keysym name. */
static void
_append_key(GString *p_out, guint u_keyval) {
   switch (u_keyval) {
   case GDK_KEY_Return:
   case GDK_KEY_KP_Enter:
      g_string_append(p_out, "Enter");
      return;
   case GDK_KEY_Escape:
      g_string_append(p_out, "Esc");
      return;
   case GDK_KEY_space:
      g_string_append(p_out, "Space");
      return;
   default:
      break;
   }
   gunichar u_ch = gdk_keyval_to_unicode(gdk_keyval_to_lower(u_keyval));
   if (u_ch > 0x20 && g_unichar_isprint(u_ch)) {
      g_string_append_unichar(p_out, u_ch);
   } else {
      const char *c_name = gdk_keyval_name(u_keyval);
      g_string_append(p_out, c_name != NULL ? c_name : "?");
   }
}

char *
shortcuts_key_label(guint u_keyval, GdkModifierType e_mods) {
   GString *p_out = g_string_new(NULL);
   _append_mods(p_out, u_keyval, e_mods);
   _append_key(p_out, u_keyval);
   return (g_string_free(p_out, FALSE));
}

/* TRUE iff the rows are three or more plain digit keys in ascending steps
 * of one -- printed as a range ("1–8") rather than eight keys. */
static gboolean
_is_digit_run(const GPtrArray *p_rows) {
   if (p_rows->len < 3) {
      return (FALSE);
   }
   const ShortcutEntry *p_first = g_ptr_array_index((GPtrArray *)p_rows, 0);
   for (guint u = 0; u < p_rows->len; u++) {
      const ShortcutEntry *p_row = g_ptr_array_index((GPtrArray *)p_rows, u);
      if (p_row->e_mods != 0 || p_row->u_keyval < GDK_KEY_0 ||
          p_row->u_keyval > GDK_KEY_9 ||
          p_row->u_keyval != p_first->u_keyval + u) {
         return (FALSE);
      }
   }
   return (TRUE);
}

/* The merged keys of one hint segment: a digit run as a range, keys that
 * share their modifiers with the prefix said once ("Shift+h/j/k/l"),
 * anything else "/"-joined in full ("a/Esc"). Caller frees. */
static char *
_join_keys(const GPtrArray *p_rows) {
   GString *p_out = g_string_new(NULL);
   if (_is_digit_run(p_rows)) {
      const ShortcutEntry *p_a = g_ptr_array_index((GPtrArray *)p_rows, 0);
      const ShortcutEntry *p_z =
         g_ptr_array_index((GPtrArray *)p_rows, p_rows->len - 1);
      g_string_append_printf(p_out, "%c–%c", (char)p_a->u_keyval,
                             (char)p_z->u_keyval);
      return (g_string_free(p_out, FALSE));
   }
   char *c_prefix = NULL;
   for (guint u = 0; u < p_rows->len; u++) {
      const ShortcutEntry *p_row = g_ptr_array_index((GPtrArray *)p_rows, u);
      GString             *p_mod = g_string_new(NULL);
      _append_mods(p_mod, p_row->u_keyval, p_row->e_mods);
      if (u == 0) {
         c_prefix = g_strdup(p_mod->str);
         g_string_append(p_out, p_mod->str);
      } else {
         g_string_append_c(p_out, '/');
         if (g_strcmp0(c_prefix, p_mod->str) != 0) {
            g_string_append(p_out, p_mod->str);
         }
      }
      _append_key(p_out, p_row->u_keyval);
      g_string_free(p_mod, TRUE);
   }
   g_free(c_prefix);
   return (g_string_free(p_out, FALSE));
}

/* Hint order: by rank, then by position in the table (the rows are one
 * static array, so their addresses are table order). */
static gint
_by_rank(gconstpointer p_a, gconstpointer p_b) {
   const ShortcutEntry *p_ra = *(const ShortcutEntry *const *)p_a;
   const ShortcutEntry *p_rb = *(const ShortcutEntry *const *)p_b;
   if (p_ra->u_rank != p_rb->u_rank) {
      return (p_ra->u_rank < p_rb->u_rank ? -1 : 1);
   }
   return (p_ra < p_rb ? -1 : (p_ra > p_rb ? 1 : 0));
}

/* The rows a hint bar lists for e_mode -- only those under c_hint when it
 * is non-NULL, only those of c_action when that is, and only the preset
 * digits of presets that exist (u_n_presets; G_MAXUINT: all) -- in hint
 * order. */
static GPtrArray *
_hint_rows(GgazeKeyMode e_mode, const char *c_hint, const char *c_action,
           guint u_n_presets) {
   GPtrArray *p_rows = g_ptr_array_new();
   guint      u_bit =
      e_mode == GGAZE_KEY_MODE_NONE ? ~0u : GGAZE_KEY_MODE_BIT(e_mode);
   for (gsize u_i = 0; u_i < G_N_ELEMENTS(SHORTCUTS); u_i++) {
      const ShortcutEntry *p_row = &SHORTCUTS[u_i];
      if ((p_row->u_hints & u_bit) == 0 || p_row->c_hint == NULL ||
          p_row->u_preset > u_n_presets ||
          (c_hint != NULL && g_strcmp0(p_row->c_hint, c_hint) != 0) ||
          (c_action != NULL && g_strcmp0(p_row->c_action, c_action) != 0)) {
         continue;
      }
      g_ptr_array_add(p_rows, (gpointer)p_row);
   }
   g_ptr_array_sort(p_rows, _by_rank);
   return (p_rows);
}

/* The merged keys of p_rows, or NULL when there are none. Frees p_rows. */
static char *
_keys_of(GPtrArray *p_rows) {
   char *c_keys = p_rows->len > 0 ? _join_keys(p_rows) : NULL;
   g_ptr_array_unref(p_rows);
   return (c_keys);
}

char *
shortcuts_hint_keys_for_action(const char *c_action) {
   g_return_val_if_fail(c_action != NULL, NULL);
   return (
      _keys_of(_hint_rows(GGAZE_KEY_MODE_NONE, NULL, c_action, G_MAXUINT)));
}

char *
shortcuts_hint_keys(GgazeKeyMode e_mode, const char *c_hint) {
   g_return_val_if_fail(c_hint != NULL, NULL);
   if (e_mode == GGAZE_KEY_MODE_NONE) {
      return (NULL);
   }
   return (_keys_of(_hint_rows(e_mode, c_hint, NULL, G_MAXUINT)));
}

/* A segment's label: the rows' c_hint -- but one preset digit alone ("1")
 * is "preset", not "presets". */
static const char *
_segment_label(const GPtrArray *p_seg) {
   const ShortcutEntry *p_first = g_ptr_array_index((GPtrArray *)p_seg, 0);
   if (p_first->u_preset != 0 && p_seg->len == 1) {
      return ("preset");
   }
   return (p_first->c_hint);
}

/* Append one "keys label" segment (keys in bold when b_markup). */
static void
_append_segment(GString *p_out, const GPtrArray *p_seg, gboolean b_markup) {
   const char *c_label = _segment_label(p_seg);
   char       *c_keys  = _join_keys(p_seg);
   if (p_out->len > 0) {
      g_string_append(p_out, "  ·  ");
   }
   if (b_markup) {
      char *c_k = g_markup_escape_text(c_keys, -1);
      char *c_l = g_markup_escape_text(c_label, -1);
      g_string_append_printf(p_out, "<b>%s</b>\u00a0%s", c_k, c_l);
      g_free(c_k);
      g_free(c_l);
   } else {
      g_string_append_printf(p_out, "%s\u00a0%s", c_keys, c_label);
   }
   g_free(c_keys);
}

char *
shortcuts_hint_for_mode(GgazeKeyMode e_mode, guint u_n_presets,
                        gboolean b_markup) {
   if (e_mode == GGAZE_KEY_MODE_NONE) {
      return (NULL);
   }
   GPtrArray *p_rows = _hint_rows(e_mode, NULL, NULL, u_n_presets);
   GString   *p_out  = g_string_new(NULL);
   GPtrArray *p_seg  = g_ptr_array_new();
   for (guint u = 0; u < p_rows->len; u++) {
      const ShortcutEntry *p_row = g_ptr_array_index(p_rows, u);
      const ShortcutEntry *p_seg0 =
         p_seg->len > 0 ? g_ptr_array_index(p_seg, 0) : NULL;
      if (p_seg0 != NULL && g_strcmp0(p_seg0->c_hint, p_row->c_hint) != 0) {
         _append_segment(p_out, p_seg, b_markup); /* the next label starts */
         g_ptr_array_set_size(p_seg, 0);
      }
      g_ptr_array_add(p_seg, (gpointer)p_row);
   }
   if (p_seg->len > 0) {
      _append_segment(p_out, p_seg, b_markup);
   }
   g_ptr_array_unref(p_seg);
   g_ptr_array_unref(p_rows);
   return (g_string_free(p_out, FALSE));
}

const char *
shortcuts_mode_title(GgazeKeyMode e_mode) {
   switch (e_mode) {
   case GGAZE_KEY_MODE_PANEL:
      return ("Edit");
   case GGAZE_KEY_MODE_CROP:
      return ("Crop");
   case GGAZE_KEY_MODE_STRAIGHTEN:
      return ("Straighten");
   default:
      return (NULL);
   }
}

/* --- help window (built from the same SHORTCUTS[] table) -----------------
 *
 * The GtkShortcuts* widgets are deprecated in GTK 4.18 but still the only
 * way to render a shortcuts help window; the deprecations are silenced here
 * so the rest of the build stays -Wall -Wextra clean. */

/* One merged help row: a title plus the space-joined accelerators of every
 * SHORTCUTS[] entry that shares that title within one group. */
typedef struct {
   const char *c_title;
   GString    *p_accels; /* owned; space-joined accelerator names */
} HelpRow;

static HelpRow *
_help_row_new(const char *c_title, const char *c_first_accel) {
   HelpRow *p_row  = g_new(HelpRow, 1);
   p_row->c_title  = c_title;
   p_row->p_accels = g_string_new(c_first_accel);
   return (p_row);
}

static void
_help_row_free(HelpRow *p_row) {
   if (p_row != NULL) {
      g_string_free(p_row->p_accels, TRUE);
      g_free(p_row);
   }
}

/* Return the ordered list of distinct help groups by first appearance in
 * SHORTCUTS[] (the table is grouped, so this is the display order). */
static GPtrArray *
_help_groups(void) {
   GPtrArray *p_groups = g_ptr_array_new_with_free_func(g_free);
   for (gsize u_i = 0; u_i < G_N_ELEMENTS(SHORTCUTS); u_i++) {
      const char *c_g = SHORTCUTS[u_i].c_group;
      if (c_g == NULL) {
         continue;
      }
      gboolean b_found = FALSE;
      for (guint u_j = 0; u_j < p_groups->len; u_j++) {
         if (g_strcmp0((const char *)g_ptr_array_index(p_groups, u_j), c_g) ==
             0) {
            b_found = TRUE;
            break;
         }
      }
      if (!b_found) {
         g_ptr_array_add(p_groups, g_strdup(c_g));
      }
   }
   return (p_groups);
}

/* Collect merged help rows for one group, preserving first-appearance order
 * and merging same-title entries (h+Left, 1..8, plus+equal, ...) into one row.
 */
static GPtrArray *
_help_rows_for_group(const char *c_group) {
   GPtrArray *p_rows =
      g_ptr_array_new_with_free_func((GDestroyNotify)_help_row_free);
   for (gsize u_i = 0; u_i < G_N_ELEMENTS(SHORTCUTS); u_i++) {
      if (SHORTCUTS[u_i].c_title == NULL ||
          g_strcmp0(SHORTCUTS[u_i].c_group, c_group) != 0) {
         continue;
      }
      /* One-keystroke rows are merged by title (h Left, 1..8, ...). */
      char *c_accel =
         gtk_accelerator_name(SHORTCUTS[u_i].u_keyval, SHORTCUTS[u_i].e_mods);
      HelpRow *p_row = NULL;
      for (guint u_j = 0; u_j < p_rows->len; u_j++) {
         HelpRow *p_existing = (HelpRow *)g_ptr_array_index(p_rows, u_j);
         if (g_strcmp0(p_existing->c_title, SHORTCUTS[u_i].c_title) == 0) {
            p_row = p_existing;
            break;
         }
      }
      if (p_row == NULL) {
         p_row = _help_row_new(SHORTCUTS[u_i].c_title, c_accel);
         g_ptr_array_add(p_rows, p_row);
      } else {
         g_string_append_c(p_row->p_accels, ' ');
         g_string_append(p_row->p_accels, c_accel);
      }
      g_free(c_accel);
   }
   return (p_rows);
}

GtkShortcutsWindow *
shortcuts_build_help(GtkWindow *p_parent) {
   G_GNUC_BEGIN_IGNORE_DEPRECATIONS
   GtkShortcutsWindow *p_win =
      g_object_new(GTK_TYPE_SHORTCUTS_WINDOW, "modal", TRUE, "section-name",
                   "shortcuts", NULL);
   GtkShortcutsSection *p_sec =
      g_object_new(GTK_TYPE_SHORTCUTS_SECTION, "section-name", "shortcuts",
                   "title", "ggaze", NULL);

   GPtrArray *p_groups = _help_groups();
   for (guint u_g = 0; u_g < p_groups->len; u_g++) {
      const char *c_group = (const char *)g_ptr_array_index(p_groups, u_g);
      GtkShortcutsGroup *p_grp =
         g_object_new(GTK_TYPE_SHORTCUTS_GROUP, "title", c_group, NULL);
      GPtrArray *p_rows = _help_rows_for_group(c_group);
      for (guint u_r = 0; u_r < p_rows->len; u_r++) {
         HelpRow *p_row = (HelpRow *)g_ptr_array_index(p_rows, u_r);
         GtkShortcutsShortcut *p_s =
            g_object_new(GTK_TYPE_SHORTCUTS_SHORTCUT, "accelerator",
                         p_row->p_accels->str, "title", p_row->c_title, NULL);
         gtk_shortcuts_group_add_shortcut(p_grp, p_s);
      }
      g_ptr_array_unref(p_rows);
      gtk_shortcuts_section_add_group(p_sec, p_grp);
   }
   g_ptr_array_unref(p_groups);

   gtk_shortcuts_window_add_section(p_win, p_sec);

   gtk_window_set_title(GTK_WINDOW(p_win),
                        "ggaze \xe2\x80\x94 keyboard shortcuts");
   if (p_parent != NULL) {
      gtk_window_set_transient_for(GTK_WINDOW(p_win), p_parent);
   }
   G_GNUC_END_IGNORE_DEPRECATIONS
   return (p_win);
}
