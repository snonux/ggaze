#ifndef GGAZE_APP_H
#define GGAZE_APP_H

/*:*
 * ggaze — application object
 *
 * GgazeApp is the GApplication/single-instance entry point. It owns the
 * GtkApplication, registers actions, and routes the GApplication `open`
 * signal (a file OR a directory) to the window. See docs/architecture.md
 * "Responsibilities / app".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <adwaita.h>

G_BEGIN_DECLS

#define GGAZE_TYPE_APP (ggaze_app_get_type())
G_DECLARE_FINAL_TYPE(GgazeApp, ggaze_app, GGAZE, APP, AdwApplication)

/* Construct a new single-instance ggaze application. */
GgazeApp *ggaze_app_new(void);

G_END_DECLS

#endif /* GGAZE_APP_H */