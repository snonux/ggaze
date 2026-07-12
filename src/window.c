/*:*
 * ggaze — main window
 *
 * Implements GgazeWindow. Builds an AdwHeaderBar + a GtkStack with two named
 * children: "grid" (placeholder until M7) and "large" (the GgazeViewer from
 * M1). ggaze_window_open() loads the file via the loader and shows it in the
 * viewer. Real file-vs-folder resolution lands in M2 (navigator); M1 just loads
 * the one file. See docs/architecture.md "Responsibilities / window".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "window.h"

#include <adwaita.h>
#include <glib.h>

#include "viewer.h"
#include "loader/loader.h"

struct _GgazeWindow {
   GtkApplicationWindow parent_instance;
   GFile     *p_file;   /* current file/folder, remembered for later use */
   GtkWidget *p_stack;  /* GtkStack: grid (placeholder) / large (viewer) */
   GtkWidget *p_viewer; /* GgazeViewer — the large view */
};

G_DEFINE_TYPE(GgazeWindow, ggaze_window, GTK_TYPE_APPLICATION_WINDOW)

static void
ggaze_window_dispose(GObject *p_obj) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_obj);
   g_clear_object(&p_win->p_file);
   /* p_stack/p_viewer are GtkWidgets parented to the window; GTK releases them.
    */
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

   /* Two-view stack: "grid" is a placeholder until M7; "large" is the viewer.
    */
   p_win->p_stack = gtk_stack_new();
   gtk_stack_set_transition_type(GTK_STACK(p_win->p_stack),
                                 GTK_STACK_TRANSITION_TYPE_CROSSFADE);
   gtk_window_set_child(GTK_WINDOW(p_win), p_win->p_stack);

   GtkWidget *p_grid = gtk_label_new("grid");
   gtk_widget_add_css_class(p_grid, "dim-label");
   gtk_stack_add_named(GTK_STACK(p_win->p_stack), p_grid, "grid");

   p_win->p_viewer = ggaze_viewer_new();
   gtk_widget_set_hexpand(p_win->p_viewer, TRUE);
   gtk_widget_set_vexpand(p_win->p_viewer, TRUE);
   gtk_stack_add_named(GTK_STACK(p_win->p_stack), p_win->p_viewer, "large");

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

   /* Load and display (M1: synchronous; M3 makes this async with cancel). */
   GError     *p_err = NULL;
   GdkTexture *p_tex = loader_load(p_file, NULL, &p_err);
   if (p_tex != NULL) {
      ggaze_viewer_set_texture(GGAZE_VIEWER(p_win->p_viewer), p_tex);
      gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack), "large");
      g_object_unref(p_tex); /* viewer holds its own ref */
   } else {
      const gchar *c_msg = (p_err != NULL) ? p_err->message : "unknown error";
      g_warning("ggaze: failed to load %s: %s", c_name, c_msg);
      g_clear_error(&p_err);
   }
   g_free(c_name);
}