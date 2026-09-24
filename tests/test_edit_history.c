/*:*
 * ggaze — edit-history unit tests (plain C, no display)
 *
 * The edit panel's undo / redo stack (7i2): push / undo / redo over whole
 * edit snapshots, the redo tail dropped by a new step, the bound that
 * forgets the oldest step, runs that coalesce (and a run that comes back
 * to its start dropping itself), clear, and the negative cases -- nothing
 * to undo or redo, a step that changes nothing, NULL arguments.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "edit-history.h"

#include <glib.h>

#include "transform.h"

/* A snapshot with mask u_mask and u_turns quarter turns. */
static EditSnapshot
snap(guint8 u_mask, gint i_turns) {
   EditSnapshot t_s;
   edit_snapshot_init(&t_s);
   t_s.u_mask         = u_mask;
   t_s.t_xf.i_quarter = i_turns;
   return (t_s);
}

static void
test_snapshot_init_and_equal(void) {
   EditSnapshot t_a, t_b;
   edit_snapshot_init(&t_a);
   g_assert_cmpuint(t_a.u_mask, ==, 0);
   g_assert_true(transform_is_identity(&t_a.t_xf));
   t_b = t_a;
   g_assert_true(edit_snapshot_equal(&t_a, &t_b));
   t_b.u_mask = 1;
   g_assert_false(edit_snapshot_equal(&t_a, &t_b));
   t_b                = t_a;
   t_b.t_xf.d_degrees = 2.5;
   g_assert_false(edit_snapshot_equal(&t_a, &t_b));
}

/* Undo walks back through the befores, redo forward through the afters,
 * each with its label; both stop at the ends. */
static void
test_push_undo_redo(void) {
   EditHistory *p_h  = edit_history_new(0);
   EditSnapshot t_s0 = snap(0, 0);
   EditSnapshot t_s1 = snap(1, 0);
   EditSnapshot t_s2 = snap(1, 1);
   g_assert_false(edit_history_can_undo(p_h));
   g_assert_false(edit_history_can_redo(p_h));
   g_assert_true(
      edit_history_push(p_h, EDIT_STEP_PRESET, "Auto-fix on", &t_s0, &t_s1));
   g_assert_true(
      edit_history_push(p_h, EDIT_STEP_ROTATE, "rotate right", &t_s1, &t_s2));
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 2);
   g_assert_cmpuint(edit_history_redo_count(p_h), ==, 0);

   const char         *c_label = NULL;
   const EditSnapshot *p_s     = edit_history_undo(p_h, &c_label);
   g_assert_true(edit_snapshot_equal(p_s, &t_s1));
   g_assert_cmpstr(c_label, ==, "rotate right");
   p_s = edit_history_undo(p_h, &c_label);
   g_assert_true(edit_snapshot_equal(p_s, &t_s0));
   g_assert_cmpstr(c_label, ==, "Auto-fix on");
   g_assert_null(edit_history_undo(p_h, &c_label)); /* nothing left */
   g_assert_false(edit_history_can_undo(p_h));
   g_assert_true(edit_history_can_redo(p_h));
   g_assert_cmpuint(edit_history_redo_count(p_h), ==, 2);

   p_s = edit_history_redo(p_h, &c_label);
   g_assert_true(edit_snapshot_equal(p_s, &t_s1));
   g_assert_cmpstr(c_label, ==, "Auto-fix on");
   p_s = edit_history_redo(p_h, NULL); /* the label is optional */
   g_assert_true(edit_snapshot_equal(p_s, &t_s2));
   g_assert_null(edit_history_redo(p_h, &c_label));
   g_assert_false(edit_history_can_redo(p_h));
   g_assert_nonnull(edit_history_undo(p_h, NULL));
   edit_history_delete(p_h);
}

/* A new step after an undo drops what could have been redone. */
static void
test_push_drops_redo(void) {
   EditHistory *p_h  = edit_history_new(0);
   EditSnapshot t_s0 = snap(0, 0);
   EditSnapshot t_s1 = snap(1, 0);
   EditSnapshot t_s2 = snap(2, 0);
   edit_history_push(p_h, EDIT_STEP_PRESET, "a", &t_s0, &t_s1);
   edit_history_push(p_h, EDIT_STEP_PRESET, "b", &t_s1, &t_s2);
   edit_history_undo(p_h, NULL);
   edit_history_undo(p_h, NULL);
   g_assert_cmpuint(edit_history_redo_count(p_h), ==, 2);
   EditSnapshot t_s3 = snap(4, 0);
   g_assert_true(edit_history_push(p_h, EDIT_STEP_PRESET, "c", &t_s0, &t_s3));
   g_assert_false(edit_history_can_redo(p_h));
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 1);
   const char *c_label = NULL;
   g_assert_true(edit_snapshot_equal(edit_history_undo(p_h, &c_label), &t_s0));
   g_assert_cmpstr(c_label, ==, "c");
   edit_history_delete(p_h);
}

/* A step that changes nothing is no step -- and does not cost the redo
 * tail either. A NULL label records a generic one. */
static void
test_noop_step_is_ignored(void) {
   EditHistory *p_h  = edit_history_new(0);
   EditSnapshot t_s0 = snap(0, 0);
   EditSnapshot t_s1 = snap(1, 0);
   g_assert_false(edit_history_push(p_h, EDIT_STEP_CROP, "crop", &t_s0, &t_s0));
   g_assert_false(edit_history_can_undo(p_h));
   edit_history_push(p_h, EDIT_STEP_PRESET, NULL, &t_s0, &t_s1);
   edit_history_undo(p_h, NULL);
   g_assert_false(
      edit_history_push(p_h, EDIT_STEP_REVERT, "revert", &t_s0, &t_s0));
   g_assert_true(edit_history_can_redo(p_h)); /* kept */
   const char *c_label = NULL;
   edit_history_redo(p_h, &c_label);
   g_assert_cmpstr(c_label, ==, "edit");
   edit_history_delete(p_h);
}

/* The bound: once full, each new step forgets the oldest one. */
static void
test_bound_drops_oldest(void) {
   EditHistory *p_h = edit_history_new(3);
   for (gint i = 0; i < 5; i++) {
      EditSnapshot t_a = snap(0, i);
      EditSnapshot t_b = snap(0, i + 1);
      char        *c_l = g_strdup_printf("turn %d", i);
      g_assert_true(edit_history_push(p_h, EDIT_STEP_ROTATE, c_l, &t_a, &t_b));
      g_free(c_l);
   }
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 3);
   const char         *c_label = NULL;
   const EditSnapshot *p_s     = NULL;
   for (gint i = 4; i >= 2; i--) {
      p_s = edit_history_undo(p_h, &c_label);
      g_assert_cmpint(p_s->t_xf.i_quarter, ==, i);
   }
   g_assert_cmpstr(c_label, ==, "turn 2"); /* turns 0 and 1 are gone */
   g_assert_null(edit_history_undo(p_h, NULL));
   edit_history_delete(p_h);

   /* The default bound is EDIT_HISTORY_MAX_STEPS. */
   p_h = edit_history_new(0);
   for (guint u = 0; u < EDIT_HISTORY_MAX_STEPS + 10; u++) {
      EditSnapshot t_a = snap((guint8)(u & 1), 0);
      EditSnapshot t_b = snap((guint8)((u + 1) & 1), 0);
      edit_history_push(p_h, EDIT_STEP_PRESET, "x", &t_a, &t_b);
   }
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, EDIT_HISTORY_MAX_STEPS);
   edit_history_delete(p_h);
}

/* A run coalesces into one step: its before stays, its after and label
 * move on, and one undo takes the whole run back. */
static void
test_run_coalesces(void) {
   EditHistory *p_h  = edit_history_new(0);
   EditSnapshot t_s0 = snap(0, 0);
   EditSnapshot t_s1 = snap(1, 0);
   EditSnapshot t_s2 = snap(1, 1);
   EditSnapshot t_s3 = snap(1, 2);
   edit_history_push(p_h, EDIT_STEP_PRESET, "on", &t_s0, &t_s1);
   g_assert_true(
      edit_history_push_run(p_h, EDIT_STEP_STRENGTH, 0, "s1", &t_s1, &t_s2));
   g_assert_true(
      edit_history_push_run(p_h, EDIT_STEP_STRENGTH, 0, "s2", &t_s2, &t_s3));
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 2);
   const char         *c_label = NULL;
   const EditSnapshot *p_s     = edit_history_undo(p_h, &c_label);
   g_assert_true(edit_snapshot_equal(p_s, &t_s1));
   g_assert_cmpstr(c_label, ==, "s2");
   p_s = edit_history_redo(p_h, NULL);
   g_assert_true(edit_snapshot_equal(p_s, &t_s3));
   edit_history_delete(p_h);
}

/* What closes a run: another key, another kind, an ordinary push, an
 * undo, an explicit end -- each makes the next run push a step of its
 * own. */
static void
test_run_breaks(void) {
   EditHistory *p_h  = edit_history_new(0);
   EditSnapshot t_s0 = snap(0, 0);
   EditSnapshot t_s1 = snap(0, 1);
   EditSnapshot t_s2 = snap(0, 2);
   EditSnapshot t_s3 = snap(0, 3);
   edit_history_push_run(p_h, EDIT_STEP_STRENGTH, 0, "a", &t_s0, &t_s1);
   edit_history_push_run(p_h, EDIT_STEP_STRENGTH, 1, "b", &t_s1, &t_s2);
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 2); /* another key */
   edit_history_push_run(p_h, EDIT_STEP_ROTATE, 1, "c", &t_s2, &t_s3);
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 3); /* another kind */
   edit_history_end_run(p_h);
   edit_history_push_run(p_h, EDIT_STEP_ROTATE, 1, "d", &t_s3, &t_s0);
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 4); /* ended */
   edit_history_push(p_h, EDIT_STEP_PRESET, "e", &t_s0, &t_s1);
   edit_history_push_run(p_h, EDIT_STEP_PRESET, 0, "f", &t_s1, &t_s2);
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 6); /* plain push */
   edit_history_undo(p_h, NULL);
   edit_history_push_run(p_h, EDIT_STEP_PRESET, 0, "g", &t_s1, &t_s3);
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 6); /* after undo */
   g_assert_false(edit_history_can_redo(p_h));
   /* A redo with nothing to redo changes nothing: the run goes on (a run
    * is only ever open with no redo tail, since its push dropped it). */
   g_assert_null(edit_history_redo(p_h, NULL));
   edit_history_push_run(p_h, EDIT_STEP_PRESET, 0, "h", &t_s3, &t_s0);
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 6);
   edit_history_delete(p_h);
}

/* A run that comes back to where it started is no step: dropped, and the
 * next nudge starts afresh. A run's first push that changes nothing
 * records nothing. */
static void
test_run_back_to_start_is_dropped(void) {
   EditHistory *p_h  = edit_history_new(0);
   EditSnapshot t_s0 = snap(0, 0);
   EditSnapshot t_s1 = snap(0, 1);
   EditSnapshot t_s2 = snap(0, 2);
   g_assert_false(
      edit_history_push_run(p_h, EDIT_STEP_STRENGTH, 3, "n", &t_s0, &t_s0));
   g_assert_false(edit_history_can_undo(p_h));
   edit_history_push(p_h, EDIT_STEP_ROTATE, "r", &t_s2, &t_s0);
   edit_history_push_run(p_h, EDIT_STEP_STRENGTH, 3, "+", &t_s0, &t_s1);
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 2);
   g_assert_false(
      edit_history_push_run(p_h, EDIT_STEP_STRENGTH, 3, "-", &t_s1, &t_s0));
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 1);
   g_assert_true(
      edit_history_push_run(p_h, EDIT_STEP_STRENGTH, 3, "+", &t_s0, &t_s1));
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 2);
   const char *c_label = NULL;
   g_assert_true(edit_snapshot_equal(edit_history_undo(p_h, &c_label), &t_s0));
   g_assert_cmpstr(c_label, ==, "+");
   g_assert_true(edit_snapshot_equal(edit_history_undo(p_h, &c_label), &t_s2));
   g_assert_cmpstr(c_label, ==, "r");
   edit_history_delete(p_h);
}

static void
test_clear(void) {
   EditHistory *p_h  = edit_history_new(0);
   EditSnapshot t_s0 = snap(0, 0);
   EditSnapshot t_s1 = snap(1, 0);
   EditSnapshot t_s2 = snap(3, 0);
   edit_history_push(p_h, EDIT_STEP_PRESET, "a", &t_s0, &t_s1);
   edit_history_push_run(p_h, EDIT_STEP_STRENGTH, 0, "b", &t_s1, &t_s2);
   edit_history_undo(p_h, NULL);
   edit_history_clear(p_h);
   g_assert_false(edit_history_can_undo(p_h));
   g_assert_false(edit_history_can_redo(p_h));
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 0);
   g_assert_cmpuint(edit_history_redo_count(p_h), ==, 0);
   g_assert_null(edit_history_undo(p_h, NULL));
   g_assert_null(edit_history_redo(p_h, NULL));
   /* A cleared history takes steps again, and no run survived it. */
   edit_history_push_run(p_h, EDIT_STEP_STRENGTH, 0, "c", &t_s0, &t_s1);
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 1);
   edit_history_clear(p_h);
   edit_history_push_run(p_h, EDIT_STEP_STRENGTH, 0, "d", &t_s0, &t_s2);
   g_assert_cmpuint(edit_history_undo_count(p_h), ==, 1);
   edit_history_delete(p_h);
   edit_history_delete(NULL); /* fine */
}

/* One NULL-argument call per entry point (see test_null_arguments). */
static void
null_call(guint u_case) {
   EditSnapshot t_s = snap(0, 0);
   switch (u_case) {
   case 0:
      g_assert_false(edit_history_can_undo(NULL));
      break;
   case 1:
      g_assert_false(edit_history_can_redo(NULL));
      break;
   case 2:
      g_assert_cmpuint(edit_history_undo_count(NULL), ==, 0);
      break;
   case 3:
      g_assert_cmpuint(edit_history_redo_count(NULL), ==, 0);
      break;
   case 4:
      g_assert_null(edit_history_undo(NULL, NULL));
      break;
   case 5:
      g_assert_null(edit_history_redo(NULL, NULL));
      break;
   case 6:
      edit_history_clear(NULL);
      break;
   case 7:
      edit_history_end_run(NULL);
      break;
   case 8:
      g_assert_false(
         edit_history_push(NULL, EDIT_STEP_PRESET, "x", &t_s, &t_s));
      break;
   case 9:
      g_assert_false(
         edit_history_push_run(NULL, EDIT_STEP_STRENGTH, 0, "x", &t_s, &t_s));
      break;
   case 10:
      g_assert_false(edit_snapshot_equal(NULL, &t_s));
      break;
   default:
      edit_snapshot_init(NULL);
      break;
   }
}

/* NULL arguments are programming errors: each entry point reports one
 * (g_return_*) and answers with the neutral value, never a crash. */
static void
test_null_arguments(void) {
   for (guint u = 0; u < 12; u++) {
      g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*");
      null_call(u);
      g_test_assert_expected_messages();
   }
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/edit_history/snapshot_init_and_equal",
                   test_snapshot_init_and_equal);
   g_test_add_func("/edit_history/push_undo_redo", test_push_undo_redo);
   g_test_add_func("/edit_history/push_drops_redo", test_push_drops_redo);
   g_test_add_func("/edit_history/noop_step_is_ignored",
                   test_noop_step_is_ignored);
   g_test_add_func("/edit_history/bound_drops_oldest", test_bound_drops_oldest);
   g_test_add_func("/edit_history/run_coalesces", test_run_coalesces);
   g_test_add_func("/edit_history/run_breaks", test_run_breaks);
   g_test_add_func("/edit_history/run_back_to_start_is_dropped",
                   test_run_back_to_start_is_dropped);
   g_test_add_func("/edit_history/clear", test_clear);
   g_test_add_func("/edit_history/null_arguments", test_null_arguments);
   return (g_test_run());
}
