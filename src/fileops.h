#ifndef GGAZE_FILEOPS_H
#define GGAZE_FILEOPS_H

/*:*
 * ggaze — destructive file-operation policy (trash / delete / move / undo)
 *
 * The rules the d / D / m / u keys share, over the plain-C engines: which
 * files an action targets (the marked set, else the current file -- decided
 * ONCE, at key-press time, because the folder monitor can prune marks behind
 * a modal prompt); the "is the target still in this folder" guard that turns
 * a vanished file into a status line instead of a silent failure; advancing
 * the cursor only when the acted-on file really was current; the unified
 * undo; and the wording of every outcome. Extracted from window.c so it is
 * unit-testable with temp folders (tests/test_fileops.c) and so the window
 * is layout + routing. The only widget-side need is "say this", which comes
 * through one host op.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>

#include "mover.h"
#include "navigator.h"
#include "trash.h"
#include "undo.h"

G_BEGIN_DECLS

typedef struct FileOps FileOps;

/* Transient status line ("Trashed a.jpg — u to undo", "Undo failed: ..."). */
typedef void (*FileOpsReportFn)(gpointer p_host, const char *c_msg);

/* p_mover and p_undo are borrowed for the FileOps' lifetime. */
FileOps *fileops_new(Mover *p_mover, Undo *p_undo, FileOpsReportFn fn_report,
                     gpointer p_host);
void     fileops_delete(FileOps *p_fo);

/* The open folder: its navigator and trash (both borrowed; NULL when no
 * folder is open). Call on every open and before the navigator is freed. */
void fileops_set_folder(FileOps *p_fo, Navigator *p_nav, Trash *p_trash);

/* The files a marks-or-current action acts on: every marked file if any are
 * marked, else the current one. (transfer full: g_list_free_full with
 * g_object_unref) */
GList *fileops_capture_targets(FileOps *p_fo);

/* TRUE iff p_file still exists inside the open folder. */
gboolean fileops_target_still_in_folder(FileOps *p_fo, GFile *p_file);

/* TRUE iff navigator.current is one of p_files. */
gboolean fileops_files_include_current(FileOps *p_fo, GList *p_files);

/* `d`: bin one captured file, advance past it if it was current, report. */
void fileops_trash(FileOps *p_fo, GFile *p_target);

/* `D` (after any confirm): permanently delete the captured set, advance if
 * one was current, report successes and failures. */
void fileops_delete_files(FileOps *p_fo, GList *p_files);

/* `m`: move the captured set to destination u_dest_idx, dim what moved,
 * advance if one was current, record for undo, report. TRUE iff the mover
 * reported overall success. */
gboolean fileops_move(FileOps *p_fo, guint u_dest_idx, GList *p_files);

/* `u`: undo the most recent of trash / move (unified), reporting the
 * outcome, including "Nothing to undo". */
void fileops_undo(FileOps *p_fo);

G_END_DECLS

#endif /* GGAZE_FILEOPS_H */
