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

/* The GdkContentProvider win.copy (Ctrl+c) would set on the clipboard, built
 * from the current window state WITHOUT touching the (display-backend-
 * dependent) system clipboard, so the copy decision is testable offscreen.
 *   - Marks present: text/uri-list + text/plain over the marked files.
 *   - No marks: the DISPLAYED GdkTexture as image/png (the enhanced preview
 *     when an enhance preset is active, else the original).
 * Returns a new ref the caller must unref, or NULL when nothing is open / no
 * marks and no texture is displayed. */
GdkContentProvider *ggaze_window_get_copy_provider(GgazeWindow *p_win);

/* Launch editor u_idx (0-based, in the configured editors list order) on the
 * ORIGINAL current file (not an enhanced preview). The opener is detached
 * (GSubprocess), so the UI stays responsive. Returns TRUE iff the program
 * was started; FALSE (with a g_warning) on a parse/launch error, an out-of-
 * range index, or when nothing is open / no editors are configured. This is
 * the testable launch path the `e` open-external popup (and its hotkeys)
 * invoke. */
gboolean ggaze_window_open_external_index(GgazeWindow *p_win, guint u_idx);

/* Run script u_idx (0-based, in the configured scripts list order) on the
 * ORIGINAL current file (%f) and the current folder (%d) via /bin/sh -c,
 * asynchronously (GSubprocess), so the UI stays responsive. On completion the
 * navigator is rescanned (scripts may add/remove files) and a status line
 * reports success or the exit status / error. The rescan is folder-identity
 * safe: the folder the script ran against is captured at launch time, and the
 * completion callback only rescans it if the window still navigates that same
 * folder (a single-instance open / drop that replaced the folder while the
 * script ran does NOT trigger a rescan of the new folder). Returns TRUE iff the
 * script was started; FALSE (with a g_warning + status) on a launch error, an
 * out-of-range index, or when nothing is open / no scripts are configured. This
 * is the testable run path the `!` run-script popup (and its hotkeys) invoke.
 */
gboolean ggaze_window_run_script_index(GgazeWindow *p_win, guint u_idx);

/* Move to destination u_idx (0-based, in the configured destinations list
 * order). Acts on every marked file if any are marked, else just the current
 * file (docs/ui-and-interactions.md "Selection & moving"). Uses mover_move
 * (rename or copy+delete, collision-suffixed) and, for every target that
 * actually left its original path, dims it in the navigator/grid via
 * navigator_mark_removed (mirroring trash) and advances past the moved set.
 * Records the move so `u` (win.undo) can move the set back — win.undo
 * prefers whichever of trash/move happened most recently. Reports a status
 * line on success or failure. Returns TRUE iff mover_move reported overall
 * success; FALSE on an out-of-range index, no destinations configured, no
 * targets to move, or a mover_move failure (still leaves anything that DID
 * move flagged as removed — see window.c's collision-with-partial-failure
 * handling). This is the testable move path the `m` popup (and its hotkeys)
 * invoke. */
gboolean ggaze_window_move_index(GgazeWindow *p_win, guint u_idx);

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