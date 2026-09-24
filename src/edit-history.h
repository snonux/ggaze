#ifndef GGAZE_EDIT_HISTORY_H
#define GGAZE_EDIT_HISTORY_H

/*:*
 * ggaze — undo / redo of edit steps (plain C, no GTK)
 *
 * The edit panel's `u` / `U` (7i2). An EDIT STEP is one thing the user did
 * to the image in the panel -- a preset toggled, a quarter turn, a crop or
 * a straighten applied, every edit reverted with x -- and the history
 * remembers each one as the pair of whole edit states around it
 * (EditSnapshot: before and after). Undo puts the BEFORE state of the
 * newest step back, redo the AFTER state of the step undone last; the
 * caller renders whatever state it is handed, so nothing here knows how a
 * step is performed or reversed, and a new kind of edit needs no undo code
 * of its own. Storing whole snapshots instead of per-kind deltas is what
 * keeps x undoable (its "before" is every edit at once) and what makes
 * adding a field to the edit state (part 3's per-preset strengths) a
 * change to EditSnapshot and edit_snapshot_equal alone.
 *
 * The usual linear model: a new step after an undo drops the steps that
 * could have been redone; the history is bounded (EDIT_HISTORY_MAX_STEPS
 * by default) and forgets its OLDEST step when full; a step whose after
 * equals its before (a crop applied over the crop already there, x with
 * nothing to revert under a tool) is no step and is not recorded.
 *
 * RUNS. A burst of steps of one kind on one target -- part 3's h / l
 * strength nudges on one preset -- should undo in one go, not one nudge
 * at a time. edit_history_push_run coalesces into the newest step while a
 * run of the same kind and key is open: its before stays, its after and
 * label move on (and a run that comes back to where it started is no step
 * at all and is dropped). Any other push, an undo, a redo, a clear or
 * edit_history_end_run closes the run.
 *
 * Plain C, owns no GtkWidget: the enhance controller (enhance-ctrl.c)
 * keeps one per window, cleared whenever the image changes, and it is
 * unit-tested standalone (tests/test_edit_history.c). It is compiled in
 * every build, the minimal one included, like transform.c, whose
 * Transform it snapshots.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <glib.h>

#include "transform.h"

G_BEGIN_DECLS

/* The whole edit state of one image: what undo / redo restore. Plain data
 * (copied by assignment). Adding a field means adding it here, setting it
 * in edit_snapshot_init and comparing it in edit_snapshot_equal -- nothing
 * else in this module looks inside. */
typedef struct {
   guint8    u_mask; /* bit i: preset i enabled (layered) */
   Transform t_xf;   /* rotate 90 / straighten / crop (the committed one) */
} EditSnapshot;

/* The untouched original: no preset, the identity transform. */
void edit_snapshot_init(EditSnapshot *p_s);

/* TRUE iff both describe the same edit state (the transform compared by
 * transform_equal). */
gboolean edit_snapshot_equal(const EditSnapshot *p_a, const EditSnapshot *p_b);

/* What a step was. Only runs look at it (a run coalesces steps of ONE
 * kind); the status line uses the step's label. */
typedef enum {
   EDIT_STEP_PRESET = 0, /* a preset toggled on or off (key: its index) */
   EDIT_STEP_ROTATE,     /* one quarter turn, [ or ] */
   EDIT_STEP_CROP,       /* the crop tool applied */
   EDIT_STEP_STRAIGHTEN, /* the straighten tool applied */
   EDIT_STEP_REVERT,     /* x: every edit dropped */
   EDIT_STEP_STRENGTH,   /* a preset's strength changed (part 3; runs) */
} EditStepKind;

/* The default bound: this many steps, then the oldest is forgotten. */
#define EDIT_HISTORY_MAX_STEPS 64

typedef struct EditHistory EditHistory;

/* A new, empty history keeping at most u_max steps (0: the default,
 * EDIT_HISTORY_MAX_STEPS). */
EditHistory *edit_history_new(guint u_max);
/* Free it (NULL is fine). */
void edit_history_delete(EditHistory *p_h);

/* Record one step from *p_before to *p_after, named c_label ("Auto-fix
 * on", "crop"; copied). Drops the steps an undo left to redo, and the
 * oldest step when the bound is reached; closes an open run. FALSE (and
 * nothing changes, redo included) when the two states are equal. */
gboolean edit_history_push(EditHistory *p_h, EditStepKind e_kind,
                           const char *c_label, const EditSnapshot *p_before,
                           const EditSnapshot *p_after);

/* Like edit_history_push, but part of a RUN of (e_kind, i_key) steps: while
 * that run is open and its step is the newest, this moves that step's
 * after and label to the new ones instead of adding a step (the run then
 * undoes as one), dropping it when its after comes back to its before.
 * Otherwise it starts a new step and opens the run. TRUE iff the history
 * holds the step afterwards (added or coalesced). */
gboolean edit_history_push_run(EditHistory *p_h, EditStepKind e_kind,
                               gint i_key, const char *c_label,
                               const EditSnapshot *p_before,
                               const EditSnapshot *p_after);

/* Close an open run: the next push_run starts a new step even for the
 * same kind and key (another preset card selected, a pause). */
void edit_history_end_run(EditHistory *p_h);

/* Undo the newest step: returns the state to go back to (its before,
 * borrowed until the next call that changes the history) and, when
 * c_label_out is non-NULL, its label (borrowed alike). NULL when there is
 * nothing to undo. */
const EditSnapshot *edit_history_undo(EditHistory *p_h,
                                      const char **c_label_out);

/* Redo the step undone last: its after state and label (borrowed as
 * above), or NULL when there is nothing to redo. */
const EditSnapshot *edit_history_redo(EditHistory *p_h,
                                      const char **c_label_out);

/* TRUE iff undo / redo would do something. */
gboolean edit_history_can_undo(const EditHistory *p_h);
gboolean edit_history_can_redo(const EditHistory *p_h);

/* How many steps undo / redo can walk (test seams, and a tooltip's
 * count). */
guint edit_history_undo_count(const EditHistory *p_h);
guint edit_history_redo_count(const EditHistory *p_h);

/* Forget every step (another image, a discard). */
void edit_history_clear(EditHistory *p_h);

G_END_DECLS

#endif /* GGAZE_EDIT_HISTORY_H */
