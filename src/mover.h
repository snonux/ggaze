#ifndef GGAZE_MOVER_H
#define GGAZE_MOVER_H

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
   char *c_name;
   char *c_path;
} MoverDest;

typedef struct Mover Mover;

Mover *mover_new(void);
void   mover_delete(Mover *p_m);

void             mover_set_dests(Mover *p_m, const GPtrArray *p_dests);
const GPtrArray *mover_get_dests(Mover *p_m);

gboolean mover_move(Mover *p_m, GList *p_files, const MoverDest *p_dest,
                    GError **p_err);
gboolean mover_undo_last(Mover *p_m, GError **p_err);
gboolean mover_can_undo(Mover *p_m);

/* Drop the recorded last-move undo state (so mover_can_undo() becomes FALSE)
 * without touching the configured destinations. Callers reopening a folder
 * use this to invalidate a move recorded against the folder just left, the
 * same way trash_new() gives that folder's Trash a fresh undo state. */
void mover_clear_last(Mover *p_m);

G_END_DECLS

#endif