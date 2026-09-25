/*:*
 * ggaze — undo / redo of edit steps (plain C, no GTK)
 *
 * See edit-history.h. The steps live in one array, oldest first; u_done
 * of them are applied (undo walks down from there, redo up), and the ones
 * above u_done are what redo can bring back until a new step drops them.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "edit-history.h"

#include <glib.h>

#include "transform.h"

/* One recorded step: the whole state before and after it, what it was,
 * and its label for the status line (owned). */
typedef struct {
   EditStepKind e_kind;
   EditSnapshot t_before;
   EditSnapshot t_after;
   char        *c_label;
} EditStep;

struct EditHistory {
   GPtrArray   *p_steps;    /* EditStep*, oldest first; owns them */
   guint        u_done;     /* steps applied: p_steps[0 .. u_done-1] */
   guint        u_max;      /* the bound */
   gboolean     b_run;      /* a run is open on the newest step ... */
   EditStepKind e_run_kind; /* ... of this kind ... */
   gint         i_run_key;  /* ... and key */
};

void
edit_snapshot_init(EditSnapshot *p_s) {
   g_return_if_fail(p_s != NULL);
   p_s->u_mask = 0;
   for (guint u = 0; u < EDIT_SNAPSHOT_PRESETS; u++) {
      p_s->d_strength[u] = 0.0;
   }
   transform_init(&p_s->t_xf);
}

gboolean
edit_snapshot_equal(const EditSnapshot *p_a, const EditSnapshot *p_b) {
   g_return_val_if_fail(p_a != NULL && p_b != NULL, FALSE);
   for (guint u = 0; u < EDIT_SNAPSHOT_PRESETS; u++) {
      if (p_a->d_strength[u] != p_b->d_strength[u]) {
         return (FALSE);
      }
   }
   return (p_a->u_mask == p_b->u_mask &&
           transform_equal(&p_a->t_xf, &p_b->t_xf));
}

static void
_step_free(gpointer p_data) {
   EditStep *p_step = p_data;
   g_free(p_step->c_label);
   g_free(p_step);
}

EditHistory *
edit_history_new(guint u_max) {
   EditHistory *p_h = g_new0(EditHistory, 1);
   p_h->p_steps     = g_ptr_array_new_with_free_func(_step_free);
   p_h->u_max       = u_max > 0 ? u_max : EDIT_HISTORY_MAX_STEPS;
   return (p_h);
}

void
edit_history_delete(EditHistory *p_h) {
   if (p_h == NULL) {
      return;
   }
   g_ptr_array_unref(p_h->p_steps);
   g_free(p_h);
}

/* The newest applied step, or NULL when none is. */
static EditStep *
_top(const EditHistory *p_h) {
   return (p_h->u_done > 0 ? g_ptr_array_index(p_h->p_steps, p_h->u_done - 1)
                           : NULL);
}

/* Forget the steps an undo left to redo: a new step starts a new future. */
static void
_drop_redo(EditHistory *p_h) {
   if (p_h->p_steps->len > p_h->u_done) {
      g_ptr_array_set_size(p_h->p_steps, p_h->u_done);
   }
}

/* Append a new step (after _drop_redo), forgetting the oldest one when
 * the bound is exceeded. */
static void
_append(EditHistory *p_h, EditStepKind e_kind, const char *c_label,
        const EditSnapshot *p_before, const EditSnapshot *p_after) {
   EditStep *p_step = g_new0(EditStep, 1);
   p_step->e_kind   = e_kind;
   p_step->t_before = *p_before;
   p_step->t_after  = *p_after;
   p_step->c_label  = g_strdup(c_label != NULL ? c_label : "edit");
   g_ptr_array_add(p_h->p_steps, p_step);
   if (p_h->p_steps->len > p_h->u_max) {
      g_ptr_array_remove_index(p_h->p_steps, 0);
   }
   p_h->u_done = p_h->p_steps->len;
}

gboolean
edit_history_push(EditHistory *p_h, EditStepKind e_kind, const char *c_label,
                  const EditSnapshot *p_before, const EditSnapshot *p_after) {
   g_return_val_if_fail(p_h != NULL && p_before != NULL && p_after != NULL,
                        FALSE);
   if (edit_snapshot_equal(p_before, p_after)) {
      return (FALSE); /* nothing changed: no step, and redo stays */
   }
   p_h->b_run = FALSE;
   _drop_redo(p_h);
   _append(p_h, e_kind, c_label, p_before, p_after);
   return (TRUE);
}

/* TRUE iff a push of (e_kind, i_key) continues the open run: the run's
 * step is still the newest one and nothing was undone since. */
static gboolean
_continues_run(const EditHistory *p_h, EditStepKind e_kind, gint i_key) {
   return (p_h->b_run && p_h->e_run_kind == e_kind && p_h->i_run_key == i_key &&
           p_h->u_done > 0 && p_h->u_done == p_h->p_steps->len);
}

/* Move the run's step on to *p_after (and c_label); a run back where it
 * started is no step: it is dropped and the run closed. */
static gboolean
_coalesce(EditHistory *p_h, const char *c_label, const EditSnapshot *p_after) {
   EditStep *p_step = _top(p_h);
   if (edit_snapshot_equal(&p_step->t_before, p_after)) {
      g_ptr_array_set_size(p_h->p_steps, p_h->u_done - 1);
      p_h->u_done = p_h->p_steps->len;
      p_h->b_run  = FALSE;
      return (FALSE);
   }
   p_step->t_after = *p_after;
   g_free(p_step->c_label);
   p_step->c_label = g_strdup(c_label != NULL ? c_label : "edit");
   return (TRUE);
}

gboolean
edit_history_push_run(EditHistory *p_h, EditStepKind e_kind, gint i_key,
                      const char *c_label, const EditSnapshot *p_before,
                      const EditSnapshot *p_after) {
   g_return_val_if_fail(p_h != NULL && p_before != NULL && p_after != NULL,
                        FALSE);
   if (_continues_run(p_h, e_kind, i_key)) {
      return (_coalesce(p_h, c_label, p_after));
   }
   if (!edit_history_push(p_h, e_kind, c_label, p_before, p_after)) {
      return (FALSE);
   }
   p_h->b_run      = TRUE;
   p_h->e_run_kind = e_kind;
   p_h->i_run_key  = i_key;
   return (TRUE);
}

void
edit_history_end_run(EditHistory *p_h) {
   g_return_if_fail(p_h != NULL);
   p_h->b_run = FALSE;
}

const EditSnapshot *
edit_history_undo(EditHistory *p_h, const char **c_label_out) {
   g_return_val_if_fail(p_h != NULL, NULL);
   EditStep *p_step = _top(p_h);
   if (p_step == NULL) {
      return (NULL);
   }
   p_h->b_run = FALSE;
   p_h->u_done--;
   if (c_label_out != NULL) {
      *c_label_out = p_step->c_label;
   }
   return (&p_step->t_before);
}

const EditSnapshot *
edit_history_redo(EditHistory *p_h, const char **c_label_out) {
   g_return_val_if_fail(p_h != NULL, NULL);
   if (p_h->u_done >= p_h->p_steps->len) {
      return (NULL);
   }
   EditStep *p_step = g_ptr_array_index(p_h->p_steps, p_h->u_done);
   p_h->b_run       = FALSE;
   p_h->u_done++;
   if (c_label_out != NULL) {
      *c_label_out = p_step->c_label;
   }
   return (&p_step->t_after);
}

gboolean
edit_history_can_undo(const EditHistory *p_h) {
   g_return_val_if_fail(p_h != NULL, FALSE);
   return (p_h->u_done > 0);
}

gboolean
edit_history_can_redo(const EditHistory *p_h) {
   g_return_val_if_fail(p_h != NULL, FALSE);
   return (p_h->u_done < p_h->p_steps->len);
}

guint
edit_history_undo_count(const EditHistory *p_h) {
   g_return_val_if_fail(p_h != NULL, 0);
   return (p_h->u_done);
}

guint
edit_history_redo_count(const EditHistory *p_h) {
   g_return_val_if_fail(p_h != NULL, 0);
   return (p_h->p_steps->len - p_h->u_done);
}

void
edit_history_clear(EditHistory *p_h) {
   g_return_if_fail(p_h != NULL);
   g_ptr_array_set_size(p_h->p_steps, 0);
   p_h->u_done = 0;
   p_h->b_run  = FALSE;
}
