#ifndef GGAZE_SHORTCUTS_H
#define GGAZE_SHORTCUTS_H

/*:*
 * ggaze — keybinding -> GAction map
 *
 * Installs a GtkShortcutController on a window, binding keys to named actions
 * ("win.*"). One table -- the SINGLE source of truth for every key: it drives
 * the bindings, the `?` help window, and the header-bar tooltips and menu
 * labels. The viewer and grid widgets bind no navigation/zoom keys of their
 * own. See docs/ui-and-interactions.md for the full keybinding set.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Build a shortcut controller for the default keybinding set and attach it to
 * p_widget (the window). The actions ("win.prev", "win.next", ...) must be
 * installed on p_widget's GActionMap by the caller. */
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

G_END_DECLS

#endif /* GGAZE_SHORTCUTS_H */