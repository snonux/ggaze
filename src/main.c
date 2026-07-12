/*:*
 * ggaze — entry point
 *
 * Builds the GgazeApp, registers the `--version` option, and hands control to
 * g_application_run. `--help` is provided by GApplication itself; `--version`
 * is handled in handle-local-options and exits before any window is created
 * (so it works headless, which the unit test relies on). See docs/roadmap.md
 * M0.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "app.h"
#include "ggaze-config.h"

#include <glib.h>
#include <gtk/gtk.h>

static const GOptionEntry GGAZE_OPTIONS[] = {
   {"version", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL,
    "Show version information and exit", NULL},
   {NULL, '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, NULL, NULL},
};

static gint
on_local_options(GApplication *p_app, GVariantDict *p_opts, gpointer p_data) {
   (void)p_app;
   (void)p_data;
   gboolean b_version = FALSE;
   if (g_variant_dict_lookup(p_opts, "version", "b", &b_version) && b_version) {
      g_print("ggaze %s\n", GGAZE_VERSION);
      return (0); /* exit success, before activate */
   }
   return (-1); /* continue normal startup */
}

int
main(int i_argc, char **c_argv) {
   g_set_application_name("ggaze");

   GgazeApp *p_app = ggaze_app_new();
   g_application_add_main_option_entries(G_APPLICATION(p_app), GGAZE_OPTIONS);
   g_signal_connect(p_app, "handle-local-options", G_CALLBACK(on_local_options),
                    NULL);

   int i_status = g_application_run(G_APPLICATION(p_app), i_argc, c_argv);

   g_object_unref(p_app);
   return (i_status);
}