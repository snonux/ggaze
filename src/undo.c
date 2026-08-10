/* undo.c — unified-undo arbitration (see undo.h). */
#include "undo.h"

#include <glib.h>

struct Undo {
   UndoKind e_last;
};

Undo *
undo_new(void) {
   return (g_new0(Undo, 1));
}

void
undo_delete(Undo *p) {
   if (p == NULL) {
      return;
   }
   g_free(p);
}

void
undo_record_trash(Undo *p) {
   g_return_if_fail(p != NULL);
   p->e_last = GGAZE_UNDO_TRASH;
}

void
undo_record_move(Undo *p) {
   g_return_if_fail(p != NULL);
   p->e_last = GGAZE_UNDO_MOVE;
}

void
undo_reset(Undo *p) {
   g_return_if_fail(p != NULL);
   p->e_last = GGAZE_UNDO_NONE;
}

/* Decision P: honour the most-recent kind when its engine can still undo;
 * otherwise fall back to whichever engine can, so a within-session sequence
 * like "move then trash, undo (undoes trash), undo (undoes the move)" works
 * rather than the second `u` silently doing nothing. */
UndoKind
undo_choose(const Undo *p, gboolean b_trash_can, gboolean b_move_can) {
   g_return_val_if_fail(p != NULL, GGAZE_UNDO_NONE);
   if (p->e_last == GGAZE_UNDO_MOVE && b_move_can) {
      return (GGAZE_UNDO_MOVE);
   }
   if (p->e_last == GGAZE_UNDO_TRASH && b_trash_can) {
      return (GGAZE_UNDO_TRASH);
   }
   if (b_move_can) {
      return (GGAZE_UNDO_MOVE);
   }
   if (b_trash_can) {
      return (GGAZE_UNDO_TRASH);
   }
   return (GGAZE_UNDO_NONE);
}