/*:*
 * ggaze — keybinding -> GAction map (single source of truth)
 *
 * The SHORTCUTS[] table binds keys to "win.*" actions (shortcuts_install) AND
 * drives the "?" help window (shortcuts_build_help), so the help can never
 * drift from the live keybindings. Each row carries a human-readable c_title
 * and a help c_group; entries sharing a title within a group merge into one
 * help row with a space-joined accelerator (h Left, 1 2 3 4 5 6 7 8, ...).
 * See docs/ui-and-interactions.md keybindings table.
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
   const char     *c_title;  /* help label; NULL hides the row from help */
   const char     *c_group;  /* help group; NULL hides the row from help */
} ShortcutEntry;

/* Ordered by help group so shortcuts_build_help can render groups in this
 * order; within a group, rows that share a title (and so merge into one help
 * row) are kept adjacent. The binding controller does not depend on this
 * order — triggers are distinct — so reordering is safe. */
static const ShortcutEntry SHORTCUTS[] = {
   /* Navigation (decision #7: vi-style + cursor keys) */
   {GDK_KEY_h, 0, "win.prev", "Previous image", "Navigation"},
   {GDK_KEY_Left, 0, "win.prev", "Previous image", "Navigation"},
   {GDK_KEY_l, 0, "win.next", "Next image", "Navigation"},
   {GDK_KEY_Right, 0, "win.next", "Next image", "Navigation"},
   {GDK_KEY_j, 0, "win.cursor-down", "Cursor down one row (grid)",
    "Navigation"},
   {GDK_KEY_Down, 0, "win.cursor-down", "Cursor down one row (grid)",
    "Navigation"},
   {GDK_KEY_k, 0, "win.cursor-up", "Cursor up one row (grid)", "Navigation"},
   {GDK_KEY_Up, 0, "win.cursor-up", "Cursor up one row (grid)", "Navigation"},
   {GDK_KEY_g, 0, "win.first", "First image", "Navigation"},
   {GDK_KEY_G, GDK_SHIFT_MASK, "win.last", "Last image", "Navigation"},
   /* View */
   {GDK_KEY_t, 0, "win.toggle-view", "Toggle large / grid", "View"},
   {GDK_KEY_f, 0, "win.fullscreen", "Fullscreen", "View"},
   {GDK_KEY_S, GDK_SHIFT_MASK, "win.slideshow", "Slideshow", "View"},
   {GDK_KEY_i, 0, "win.info", "Info overlay", "View"},
   {GDK_KEY_comma, 0, "win.preferences", "Preferences", "View"},
   /* Selection (marks) */
   {GDK_KEY_v, 0, "win.mark", "Toggle mark on highlighted",
    "Selection (marks)"},
   {GDK_KEY_V, GDK_SHIFT_MASK, "win.mark-range",
    "Range-mark from last mark to current", "Selection (marks)"},
   {GDK_KEY_a, GDK_CONTROL_MASK, "win.mark-all", "Mark all",
    "Selection (marks)"},
   {GDK_KEY_Escape, 0, "win.back", "Clear marks / back", "Selection (marks)"},
   /* Files */
   {GDK_KEY_o, 0, "win.open", "Open", "Files"},
   {GDK_KEY_e, 0, "win.open-external", "Open in external program", "Files"},
   {GDK_KEY_exclam, 0, "win.run-script", "Run a configured shell script",
    "Files"},
   {GDK_KEY_m, 0, "win.move", "Move marked/current to a destination", "Files"},
   {GDK_KEY_d, 0, "win.trash", "Trash", "Files"},
   {GDK_KEY_D, GDK_SHIFT_MASK, "win.delete", "Delete permanently", "Files"},
   {GDK_KEY_u, 0, "win.undo", "Undo last trash or move", "Files"},
   {GDK_KEY_c, GDK_CONTROL_MASK, "win.copy", "Copy image / marked files",
    "Files"},
   {GDK_KEY_q, 0, "win.quit", "Quit", "Files"},
   /* Enhance */
   {GDK_KEY_a, 0, "win.enhance", "Toggle the enhance side panel", "Enhance"},
   {GDK_KEY_1, 0, "win.enhance-1",
    "Toggle enhance preset 1-8 (layered); 0 = Original", "Enhance"},
   {GDK_KEY_2, 0, "win.enhance-2",
    "Toggle enhance preset 1-8 (layered); 0 = Original", "Enhance"},
   {GDK_KEY_3, 0, "win.enhance-3",
    "Toggle enhance preset 1-8 (layered); 0 = Original", "Enhance"},
   {GDK_KEY_4, 0, "win.enhance-4",
    "Toggle enhance preset 1-8 (layered); 0 = Original", "Enhance"},
   {GDK_KEY_5, 0, "win.enhance-5",
    "Toggle enhance preset 1-8 (layered); 0 = Original", "Enhance"},
   {GDK_KEY_6, 0, "win.enhance-6",
    "Toggle enhance preset 1-8 (layered); 0 = Original", "Enhance"},
   {GDK_KEY_7, 0, "win.enhance-7",
    "Toggle enhance preset 1-8 (layered); 0 = Original", "Enhance"},
   {GDK_KEY_8, 0, "win.enhance-8",
    "Toggle enhance preset 1-8 (layered); 0 = Original", "Enhance"},
   {GDK_KEY_s, 0, "win.enhance-save", "Save enhanced copy", "Enhance"},
   /* Zoom */
   {GDK_KEY_plus, 0, "win.zoom-in", "Zoom in", "Zoom"},
   {GDK_KEY_equal, 0, "win.zoom-in", "Zoom in", "Zoom"},
   {GDK_KEY_minus, 0, "win.zoom-out", "Zoom out", "Zoom"},
   {GDK_KEY_underscore, 0, "win.zoom-out", "Zoom out", "Zoom"},
   {GDK_KEY_question, 0, "win.shortcuts", "Show this help", "Zoom"},
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