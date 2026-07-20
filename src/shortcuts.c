/*:*
 * ggaze — keybinding -> GAction map
 *
 * Default keybinding table bound to "win.*" actions. Milestones add rows here
 * as new actions land. See docs/ui-and-interactions.md keybindings table.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "shortcuts.h"

#include <gtk/gtk.h>

typedef struct {
   guint           u_keyval;
   GdkModifierType e_mods;
   const char     *c_action; /* e.g. "win.next" */
} ShortcutEntry;

static const ShortcutEntry SHORTCUTS[] = {
   /* navigation (decision #7: vi-style + cursor keys) */
   {GDK_KEY_h, 0, "win.prev"},
   {GDK_KEY_Left, 0, "win.prev"},
   {GDK_KEY_l, 0, "win.next"},
   {GDK_KEY_Right, 0, "win.next"},
   {GDK_KEY_g, 0, "win.first"},
   {GDK_KEY_G, 0, "win.last"},
   /* open / quit / back */
   {GDK_KEY_o, 0, "win.open"},
   {GDK_KEY_q, 0, "win.quit"},
   {GDK_KEY_d, 0, "win.trash"},
   {GDK_KEY_D, GDK_SHIFT_MASK, "win.delete"},
   {GDK_KEY_u, 0, "win.undo"},
   {GDK_KEY_t, 0, "win.toggle-view"},
   {GDK_KEY_plus, 0, "win.zoom-in"},
   {GDK_KEY_equal, 0, "win.zoom-in"},
   {GDK_KEY_minus, 0, "win.zoom-out"},
   {GDK_KEY_underscore, 0, "win.zoom-out"},
   {GDK_KEY_f, 0, "win.fullscreen"},
   {GDK_KEY_S, GDK_SHIFT_MASK, "win.slideshow"},
   {GDK_KEY_i, 0, "win.info"},
   {GDK_KEY_Escape, 0, "win.back"},
};

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
      GtkShortcut *p_s =
         gtk_shortcut_new(GTK_SHORTCUT_TRIGGER(gtk_keyval_trigger_new(
                             SHORTCUTS[u_i].u_keyval, SHORTCUTS[u_i].e_mods)),
                          gtk_named_action_new(SHORTCUTS[u_i].c_action));
      gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(p_ctrl),
                                           p_s);
   }
   gtk_widget_add_controller(p_widget, GTK_EVENT_CONTROLLER(p_ctrl));
}