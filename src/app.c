/*:*
 * ggaze — application object
 *
 * Implements GgazeApp (GgazeApp : AdwApplication : GtkApplication). Wires the
 * GApplication `open` signal (file or folder) and the default `activate`
 * (no-arg) path to a GgazeWindow, reusing the active window for the
 * single-instance "replace on new open" behaviour (decision #32). File-vs-
 * folder resolution is the window's/navigator's job. See
 * docs/architecture.md.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "app.h"

#include <glib.h>

#include "ggaze-config.h"
#include "window.h"

#if GGAZE_HAVE_GEGL
#include <gegl.h>
#endif

struct _GgazeApp {
   AdwApplication parent_instance;
};

G_DEFINE_TYPE(GgazeApp, ggaze_app, ADW_TYPE_APPLICATION)

static GgazeWindow *
_ensure_window(GgazeApp *p_app) {
   GtkWindow *p_active =
      gtk_application_get_active_window(GTK_APPLICATION(p_app));
   if (p_active != NULL && GGAZE_IS_WINDOW(p_active)) {
      return (GgazeWindow *)p_active;
   }
   return (ggaze_window_new(p_app));
}

static void
ggaze_app_activate(GApplication *p_app) {
   GgazeWindow *p_win = _ensure_window(GGAZE_APP(p_app));
   gtk_window_present(GTK_WINDOW(p_win));
}

static void
ggaze_app_startup(GApplication *p_app) {
   G_APPLICATION_CLASS(ggaze_app_parent_class)->startup(p_app);
#if GGAZE_HAVE_GEGL
   /* Init GEGL once before any window is created (before the first enhance
    * call). Idempotent if already initialised. */
   gegl_init(NULL, NULL);
#endif
}

static void
ggaze_app_open(GApplication *p_app, GFile **p_files, gint n_files,
               const gchar *c_hint) {
   (void)c_hint;
   GgazeApp    *p_app_self = GGAZE_APP(p_app);
   GgazeWindow *p_win      = _ensure_window(p_app_self);
   /* Decision #27 / open-question Z: one file opens it in the large view;
    * several open the first file's folder in the grid with the first one
    * current (the window decides, see ggaze_window_open_files). */
   if (n_files > 0 && p_files != NULL) {
      ggaze_window_open_files(p_win, p_files, n_files);
   }
   gtk_window_present(GTK_WINDOW(p_win));
}

static void
ggaze_app_init(GgazeApp *p_app) {
   (void)p_app;
}

static void
ggaze_app_class_init(GgazeAppClass *p_klass) {
   GApplicationClass *p_app_class = G_APPLICATION_CLASS(p_klass);
   p_app_class->startup           = ggaze_app_startup;
   p_app_class->activate          = ggaze_app_activate;
   p_app_class->open              = ggaze_app_open;
}

GgazeApp *
ggaze_app_new(void) {
   return (
      GGAZE_APP(g_object_new(GGAZE_TYPE_APP, "application-id", GGAZE_APP_ID,
                             "flags", G_APPLICATION_HANDLES_OPEN, NULL)));
}