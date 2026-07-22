#ifndef GGAZE_WINDOW_H
#define GGAZE_WINDOW_H

/*:*
 * ggaze — main window
 *
 * GgazeWindow : GtkApplicationWindow owns the layout: an AdwHeaderBar and a
 * GtkStack with two children (`grid`, `large`). The grid child is a placeholder
 * until M7; the large child is the GgazeViewer (M1). M2 adds a Navigator over
 * the current folder, a single GCancellable (last-write-wins), keybinding
 * shortcuts, and a file/folder drop target. See docs/architecture.md.
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

/* Open p_arg (a file or a folder): for a file, list its parent folder with
 * p_arg current; for a folder, list it with the first image current. Loads the
 * current image into the viewer and switches the stack to "large". */
void ggaze_window_open(GgazeWindow *p_win, GFile *p_arg);

/* The GtkStack (grid/large) — the tests use this instead of
 * gtk_window_get_child (which now returns the wrapping GtkOverlay). */
GtkStack *ggaze_window_get_stack(GgazeWindow *p_win);

/* Navigation over the current folder (bound to h/l/Left/Right/g/G via
 * shortcuts.c). No-ops if nothing is open. */
void ggaze_window_prev(GgazeWindow *p_win);
void ggaze_window_next(GgazeWindow *p_win);
void ggaze_window_first(GgazeWindow *p_win);
void ggaze_window_last(GgazeWindow *p_win);

/* --- INTERNAL: bulk-delete safety (used by the confirm-dialog flow and the
 * delete-safety regression test) -------------------------------------------
 *
 * The >1-mark delete opens an async GtkAlertDialog. While it is pending, a
 * single-instance open / drop can replace the folder (ggaze_window_open swaps
 * p_nav). Re-reading the navigator's marks on confirm would then delete files
 * from the NEW folder. To prevent that, the targets are captured at prompt
 * time and the dialog callback validates the folder is still the same one
 * before deleting the captured (not re-read) files.
 *
 * ggaze_window_delete_targets_still_current returns TRUE iff p_win's current
 * navigator is over p_dir (the folder the captured targets came from is still
 * open), so a pending confirm may safely delete them. FALSE means the folder
 * was replaced while the dialog was pending and the delete must be refused.
 *
 * ggaze_window_delete_captured deletes EXACTLY p_files (the captured targets;
 * the list is borrowed and not freed here) iff that folder is still open, and
 * returns TRUE iff the delete proceeded. p_files are GFile* (borrowed refs).
 */
gboolean ggaze_window_delete_targets_still_current(GgazeWindow *p_win,
                                                   GFile       *p_dir);
gboolean ggaze_window_delete_captured(GgazeWindow *p_win, GFile *p_dir,
                                      GList *p_files);

G_END_DECLS

#endif /* GGAZE_WINDOW_H */