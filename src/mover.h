#ifndef GGAZE_MOVER_H
#define GGAZE_MOVER_H

/*:*
 * ggaze — configurable move destinations with undo
 *
 * Holds an ordered list of destinations (SettingsPair: name + absolute path)
 * read from GSettings, and a one-level undo of the last move. Plain-C.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "settings-pair.h"

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct Mover Mover;

Mover *mover_new(void);
void   mover_delete(Mover *p_m);

void             mover_set_dests(Mover *p_m, const GPtrArray *p_dests);
const GPtrArray *mover_get_dests(Mover *p_m);

/* Move p_files into p_dest->c_value (creating it if needed). Records the move
 * for mover_undo_last(). */
gboolean mover_move(Mover *p_m, GList *p_files, const SettingsPair *p_dest,
                    GError **p_err);
gboolean mover_undo_last(Mover *p_m, GError **p_err);
gboolean mover_can_undo(Mover *p_m);

/* Drop the recorded last-move undo state (so mover_can_undo() becomes FALSE)
 * without touching the configured destinations. Callers reopening a folder
 * use this to invalidate a move recorded against the folder just left, the
 * same way trash_new() gives that folder's Trash a fresh undo state. */
void mover_clear_last(Mover *p_m);

G_END_DECLS

#endif /* GGAZE_MOVER_H */