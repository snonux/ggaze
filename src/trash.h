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
 * move so trash_restore_last() can undo it. Returns TRUE on success. */
gboolean trash_bin(Trash *p_t, GFile *p_file, GError **p_err);

/* Permanently delete p_file (unlink). Not undoable. */
gboolean trash_permanently_delete(Trash *p_t, GFile *p_file, GError **p_err);

/* Move the last binned file back to its original path. Returns FALSE if there
 * is nothing to undo. */
gboolean trash_restore_last(Trash *p_t, GError **p_err);

gboolean trash_can_undo(Trash *p_t);

G_END_DECLS

#endif /* GGAZE_TRASH_H */