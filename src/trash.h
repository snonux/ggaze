#ifndef GGAZE_TRASH_H
#define GGAZE_TRASH_H

/*:*
 * ggaze — local ./Trash bin
 *
 * Moves files into <shoot_dir>/.Trash/ (created lazily, collision-suffixed
 * -1/-2/...) and tracks the last binned item for `u` restore. `D` permanently
 * unlinks. Never touches the system trash. Plain-C, no GtkWidget,
 * unit-testable. See docs/architecture.md "trash" + docs/PLAN.md decisions
 * #5/#6.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct Trash Trash;

/* .Trash lives at <p_shoot_dir>/.Trash. Refs p_shoot_dir. */
Trash *trash_new(GFile *p_shoot_dir);
void   trash_delete(Trash *p_t); /* g_object_unref-style destructor */

/* Move p_file into .Trash (lazy create, collision suffix -1/-2). Records the
 * move -- as a new undo batch of one -- so trash_restore_last() can undo
 * it. Returns TRUE on success. */
gboolean trash_bin(Trash *p_t, GFile *p_file, GError **p_err);

/* The same, as one more file of the batch the last trash_bin started: the
 * undo then restores all of them (a multi-selection binned with one `d`). */
gboolean trash_bin_more(Trash *p_t, GFile *p_file, GError **p_err);

/* Permanently delete p_file (unlink). Not undoable; stateless (no Trash). */
gboolean trash_permanently_delete(GFile *p_file, GError **p_err);

/* Move the last binned batch (one file, or everything trash_bin_more added
 * to it) back to its original paths. Returns FALSE if there is nothing to
 * undo, or when a file would not go back (p_err set; it and the ones not
 * tried yet stay restorable). */
gboolean trash_restore_last(Trash *p_t, GError **p_err);

gboolean trash_can_undo(Trash *p_t);

/* Permanently delete every file in the folder's .Trash (the "Empty Trash"
 * menu action). Stops at the first failure with p_err set; *p_deleted counts
 * the files removed either way. Clears the one-level undo. A missing .Trash
 * is an empty one (TRUE, 0 deleted). */
gboolean trash_empty(Trash *p_t, guint *p_deleted, GError **p_err);

/* Number of files currently in the folder's .Trash (0 when absent). */
guint trash_count(Trash *p_t);

G_END_DECLS

#endif /* GGAZE_TRASH_H */