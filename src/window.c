/*:*
 * ggaze — main window
 *
 * Implements GgazeWindow. Builds an AdwHeaderBar + a GtkStack with two named
 * placeholder children ("grid", "large"); the real views arrive in M1 (large)
 * and M7 (grid). Tracks the current GFile for later milestones.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "window.h"

#include <adwaita.h>
#include <glib.h>

struct _GgazeWindow {
   GtkApplicationWindow parent_instance;
   GFile     *p_file;  /* current file/folder, remembered for later use */
   GtkWidget *p_stack; /* GtkStack: grid/large placeholder children */
};

G_DEFINE_TYPE(GgazeWindow, ggaze_window, GTK_TYPE_APPLICATION_WINDOW)

static void
ggaze_window_dispose(GObject *p_obj) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_obj);
   g_clear_object(&p_win->p_file);
   G_OBJECT_CLASS(ggaze_window_parent_class)->dispose(p_obj);
}

static void
ggaze_window_class_init(GgazeWindowClass *p_klass) {
   GObjectClass *p_obj_class = G_OBJECT_CLASS(p_klass);
   p_obj_class->dispose      = ggaze_window_dispose;
}

static void
ggaze_window_init(GgazeWindow *p_win) {
   /* Header bar (libadwaita, decision #29). */
   GtkWidget *p_header = adw_header_bar_new();
   gtk_window_set_titlebar(GTK_WINDOW(p_win), p_header);

   /* Two-view stack. Children are placeholders for M1 (large) and M7 (grid). */
   p_win->p_stack = gtk_stack_new();
   gtk_stack_set_transition_type(GTK_STACK(p_win->p_stack),
                                 GTK_STACK_TRANSITION_TYPE_CROSSFADE);
   gtk_window_set_child(GTK_WINDOW(p_win), p_win->p_stack);

   GtkWidget *p_grid = gtk_label_new("grid");
   gtk_widget_add_css_class(p_grid, "dim-label");
   GtkWidget *p_large = gtk_label_new("large");
   gtk_widget_add_css_class(p_large, "dim-label");
   gtk_stack_add_named(GTK_STACK(p_win->p_stack), p_grid, "grid");
   gtk_stack_add_named(GTK_STACK(p_win->p_stack), p_large, "large");
   gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack), "grid");
}

GgazeWindow *
ggaze_window_new(GgazeApp *p_app) {
   return (GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, "application", p_app,
                                     "default-width", 800, "default-height",
                                     600, NULL)));
}

void
ggaze_window_open(GgazeWindow *p_win, GFile *p_file) {
   g_return_if_fail(GGAZE_IS_WINDOW(p_win));
   g_return_if_fail(G_IS_FILE(p_file));
   g_set_object(&p_win->p_file, p_file);
   char *c_name = g_file_get_basename(p_file);
   gtk_window_set_title(GTK_WINDOW(p_win), c_name);
   g_free(c_name);
}