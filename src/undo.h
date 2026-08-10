#ifndef GGAZE_UNDO_H
#define GGAZE_UNDO_H

/*:*
 * ggaze — unified-undo arbitration
 *
 * Tracks which destructive engine (Trash or Mover) acted most recently and
 * decides which one a press of `u` (win.undo) should undo, so a single key
 * undoes the last trash OR the last move without the caller knowing either
 * engine. The engines themselves (trash.c, mover.c) keep their own one-level
 * undo state; this coordinator only owns the "which one ran last" ordering
 * (decision P: one unified undo, no shared engine module).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
   GGAZE_UNDO_NONE = 0, /* nothing destructive has run this session/folder */
   GGAZE_UNDO_TRASH,    /* the last destructive action was a trash (`d`) */
   GGAZE_UNDO_MOVE      /* the last destructive action was a move (`m`) */
} UndoKind;

typedef struct Undo Undo;

Undo *undo_new(void);
void  undo_delete(Undo *p);

/* Record that a trash / move just succeeded, or clear the record (on folder
 * (re)open, which resets both engines' undo state together). */
void undo_record_trash(Undo *p);
void undo_record_move(Undo *p);
void undo_reset(Undo *p);

/* Decide which engine a `u` should undo: honour the most-recent destructive
 * kind when its engine can still undo, else fall back to whichever engine
 * can. Returns GGAZE_UNDO_NONE when neither can. b_trash_can / b_move_can are
 * the engines' own can_undo() results. */
UndoKind undo_choose(const Undo *p, gboolean b_trash_can, gboolean b_move_can);

G_END_DECLS

#endif /* GGAZE_UNDO_H */