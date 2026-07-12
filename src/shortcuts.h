#ifndef GGAZE_SHORTCUTS_H
#define GGAZE_SHORTCUTS_H

/*:*
 * ggaze — keybinding -> GAction map
 *
 * Installs a GtkShortcutController on a window, binding keys to named actions
 * ("win.*"). One table all milestones add to; see docs/ui-and-interactions.md
 * for the full keybinding set.
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

G_END_DECLS

#endif /* GGAZE_SHORTCUTS_H */