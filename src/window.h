#ifndef GGAZE_WINDOW_H
#define GGAZE_WINDOW_H

/*:*
 * ggaze — main window
 *
 * GgazeWindow : GtkApplicationWindow owns the layout: an AdwHeaderBar and a
 * GtkStack with two children (`grid`, `large`). The grid child is a placeholder
 * until M7; the large child is the GgazeViewer (M1). The window remembers the
 * current GFile so later milestones can build on it. See
 * docs/architecture.md "Responsibilities / window".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "app.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GGAZE_TYPE_WINDOW (ggaze_window_get_type())
G_DECLARE_FINAL_TYPE(GgazeWindow, ggaze_window, GGAZE, WINDOW,
                     GtkApplicationWindow)

/* Construct a new window attached to p_app. */
GgazeWindow *ggaze_window_new(GgazeApp *p_app);

/* Remember p_file as the current file (or folder) and title the window with
 * its basename. The GFile reference is held until replaced or the window is
 * destroyed. Real file-vs-folder handling lands in M2. */
void ggaze_window_open(GgazeWindow *p_win, GFile *p_file);

G_END_DECLS

#endif /* GGAZE_WINDOW_H */