#ifndef GGAZE_SHORTCUTS_H
#define GGAZE_SHORTCUTS_H

/*:*
 * ggaze — keybinding -> GAction map
 *
 * Installs a GtkShortcutController on a window, binding keys to named actions
 * ("win.*"). One table -- the SINGLE source of truth for every key: it drives
 * the bindings, the `?` help window, the header-bar tooltips and menu
 * labels, the edit panel's button keys and the key-hint bar. The viewer and
 * grid widgets bind no navigation/zoom keys of their own. See
 * docs/ui-and-interactions.md for the full keybinding set.
 *
 * KEY MODES. Most rows are GLOBAL: bound on the window's shortcut
 * controller, live whatever is on screen. A row can instead be SCOPED to an
 * edit mode -- the edit panel open (GGAZE_KEY_MODE_PANEL), the crop tool,
 * the straighten tool. A scoped row is never bound globally; the window's
 * capture-phase edit-key router (edit-mode.c) asks this table for it while
 * that mode is the one on screen (shortcuts_mode_action for a panel key
 * that fires a win.* action, shortcuts_mode_op for a tool key the tool
 * answers itself). So the digits toggle presets only while the panel is
 * open, and `h` moves the crop rectangle only while the crop tool is up --
 * and the moment the mode ends the same key falls through to its global
 * meaning (win.prev). Rows also say which modes' key-hint bar lists them
 * (a global `s` is listed in the panel's bar), so the bar, `?`, the menu
 * and the tooltips are all read off the same rows and cannot drift.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The editing context that owns the keyboard beyond the global table.
 * Exactly one is in effect at a time: a tool wins over the panel (the
 * panel is open whenever a tool is started from the keyboard, but the
 * tool's keys come first). */
typedef enum {
   GGAZE_KEY_MODE_NONE = 0,   /* browsing: the global table alone */
   GGAZE_KEY_MODE_PANEL,      /* the edit panel is open, no tool active */
   GGAZE_KEY_MODE_CROP,       /* the crop tool (c) */
   GGAZE_KEY_MODE_STRAIGHTEN, /* the straighten tool (r) */
} GgazeKeyMode;

/* A mode as a bit, for the table's scope and hint masks. */
#define GGAZE_KEY_MODE_BIT(e_mode) (1u << (guint)(e_mode))

/* What a tool-scoped key does. The tool (tool-ctrl.c) switches on these
 * instead of on raw keyvals, so which key does what lives in the table
 * alone. */
typedef enum {
   GGAZE_KEY_OP_NONE = 0,
   GGAZE_KEY_OP_TOOL_APPLY,  /* Enter: commit and leave the tool */
   GGAZE_KEY_OP_TOOL_CANCEL, /* Esc: restore and leave */
   GGAZE_KEY_OP_CROP_MOVE_LEFT,
   GGAZE_KEY_OP_CROP_MOVE_RIGHT,
   GGAZE_KEY_OP_CROP_MOVE_UP,
   GGAZE_KEY_OP_CROP_MOVE_DOWN,
   GGAZE_KEY_OP_CROP_GROW_LEFT, /* that side moves outward */
   GGAZE_KEY_OP_CROP_GROW_RIGHT,
   GGAZE_KEY_OP_CROP_GROW_TOP,
   GGAZE_KEY_OP_CROP_GROW_BOTTOM,
   GGAZE_KEY_OP_CROP_SHRINK_LEFT, /* that side moves inward */
   GGAZE_KEY_OP_CROP_SHRINK_RIGHT,
   GGAZE_KEY_OP_CROP_SHRINK_TOP,
   GGAZE_KEY_OP_CROP_SHRINK_BOTTOM,
   GGAZE_KEY_OP_CROP_ASPECT,        /* cycle the aspect lock */
   GGAZE_KEY_OP_STRAIGHTEN_CCW,     /* nudge counter-clockwise */
   GGAZE_KEY_OP_STRAIGHTEN_CW,      /* nudge clockwise */
   GGAZE_KEY_OP_STRAIGHTEN_AUTOCROP /* toggle the auto-crop */
} GgazeKeyOp;

/* The short hint labels shared by the table rows and the edit panel's
 * buttons that look their keys up (shortcuts_hint_keys). */
#define SHORTCUTS_HINT_CLOSE "close"

/* Build a shortcut controller for the default (GLOBAL) keybinding set and
 * attach it to p_widget (the window). The actions ("win.prev", "win.next",
 * ...) must be installed on p_widget's GActionMap by the caller.
 * Mode-scoped rows are not bound here (see the header comment). */
void shortcuts_install(GtkWidget *p_widget);

/* Build the "?" keyboard-shortcuts help window from the same SHORTCUTS[] table
 * that shortcuts_install() binds, so the help never drifts from the live
 * keybindings. Returns a new GtkShortcutsWindow (modal, transient for
 * p_parent which may be NULL); the caller presents it. */
GtkShortcutsWindow *shortcuts_build_help(GtkWindow *p_parent);

/* Lookups into the same table, so the header-bar buttons and the main menu
 * can carry the live key in their labels/tooltips instead of a second copy
 * of the binding. c_action is the full name ("win.next"). */
/* The help title of c_action (borrowed), or NULL if the table has no row. */
const char *shortcuts_title_for_action(const char *c_action);
/* Human-readable keys bound to c_action, " / "-joined ("l / Right"), or
 * NULL. Caller frees. */
char *shortcuts_keys_for_action(const char *c_action);
/* "Title (keys)" for a button tooltip, or NULL. Caller frees. */
char *shortcuts_tooltip_for_action(const char *c_action);
/* The short menu label of c_action ("Crop", "Rotate left"; borrowed): the
 * row's own short label, else its help title, else NULL. The help keeps
 * the long description. */
const char *shortcuts_label_for_action(const char *c_action);

/* --- key modes (edit panel, crop tool, straighten tool) ------------------ */

/* The win.* action a PANEL-scoped row binds u_keyval + e_state to in
 * e_mode (borrowed), or NULL. Only scoped rows are consulted: a global key
 * is the shortcut controller's. Shift is compared only for letters (it is
 * part of the keyval for `+`, `?`, ...); NumLock is ignored, and so is
 * Caps Lock: a letter it upper-cased without Shift matches as the plain
 * letter (`H` + Lock is `h`), with Shift as the Shift chord. */
const char *shortcuts_mode_action(GgazeKeyMode e_mode, guint u_keyval,
                                  GdkModifierType e_state);

/* The tool op a row scoped to e_mode gives u_keyval + e_state, or
 * GGAZE_KEY_OP_NONE (same matching rules). */
GgazeKeyOp shortcuts_mode_op(GgazeKeyMode e_mode, guint u_keyval,
                             GdkModifierType e_state);

/* The GLOBAL action u_keyval + e_state is bound to (borrowed), or NULL: how
 * a tool recognises the tool keys themselves (c, r, [, ]) without a second
 * copy of them. Matched by the same rules as the modes (Caps Lock: `C` +
 * Lock is `c`), so a tool and the shortcut controller agree on a key. */
const char *shortcuts_global_action(guint u_keyval, GdkModifierType e_state);

/* The key as the edit panel and the hint bar print it: lower-case letters
 * with their modifiers spelled out ("c", "Shift+h", "Ctrl+l"), "Enter",
 * "Esc", "Space", and the character itself for punctuation ("[", "+").
 * Caller frees. */
char *shortcuts_key_label(guint u_keyval, GdkModifierType e_mods);

/* The keys of every row of c_action that some hint bar lists, merged the
 * way the bar merges them ("c", "1–8", "[/]"), or NULL. For a button that
 * fires c_action. Caller frees. */
char *shortcuts_hint_keys_for_action(const char *c_action);

/* The keys the e_mode hint bar shows under c_hint ("a/Esc" for "close"),
 * or NULL. Caller frees. */
char *shortcuts_hint_keys(GgazeKeyMode e_mode, const char *c_hint);

/* The mode's key-hint line: its rows in hint order, one "keys label"
 * segment per hint label, " · "-joined -- plain text, or Pango markup with
 * the keys in bold when b_markup. The preset digits are listed for the
 * u_n_presets presets that exist ("1–8 presets", "1/2 presets", "1
 * preset", nothing for 0), never for a digit that toggles nothing. NULL
 * for GGAZE_KEY_MODE_NONE. Caller frees. */
char *shortcuts_hint_for_mode(GgazeKeyMode e_mode, guint u_n_presets,
                              gboolean b_markup);

/* The mode's name for the hint bar ("Edit", "Crop", "Straighten"), or NULL
 * for NONE. Borrowed. */
const char *shortcuts_mode_title(GgazeKeyMode e_mode);

G_END_DECLS

#endif /* GGAZE_SHORTCUTS_H */
