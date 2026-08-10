/* test_undo.c — unified-undo arbitration unit test (plain-C, no display). */
#include "undo.h"

#include <glib.h>

static void
test_undo_choose_prefers_recent(void) {
   Undo *u = undo_new();
   /* Nothing recorded yet: falls back to whichever engine can. */
   g_assert_cmpint(undo_choose(u, FALSE, FALSE), ==, GGAZE_UNDO_NONE);
   g_assert_cmpint(undo_choose(u, TRUE, FALSE), ==, GGAZE_UNDO_TRASH);
   g_assert_cmpint(undo_choose(u, FALSE, TRUE), ==, GGAZE_UNDO_MOVE);

   /* Most-recent wins when its engine can still undo. */
   undo_record_trash(u);
   g_assert_cmpint(undo_choose(u, TRUE, TRUE), ==, GGAZE_UNDO_TRASH);
   undo_record_move(u);
   g_assert_cmpint(undo_choose(u, TRUE, TRUE), ==, GGAZE_UNDO_MOVE);

   /* Most-recent's engine exhausted -> fall back to the other. */
   undo_record_move(u);
   g_assert_cmpint(undo_choose(u, TRUE, FALSE), ==, GGAZE_UNDO_TRASH);
   undo_record_trash(u);
   g_assert_cmpint(undo_choose(u, FALSE, TRUE), ==, GGAZE_UNDO_MOVE);
   undo_delete(u);
}

static void
test_undo_reset(void) {
   Undo *u = undo_new();
   undo_record_move(u);
   undo_reset(u);
   /* After reset, neither is preferred: falls back to capability. */
   g_assert_cmpint(undo_choose(u, TRUE, TRUE), ==, GGAZE_UNDO_MOVE);
   g_assert_cmpint(undo_choose(u, TRUE, FALSE), ==, GGAZE_UNDO_TRASH);
   g_assert_cmpint(undo_choose(u, FALSE, FALSE), ==, GGAZE_UNDO_NONE);
   undo_delete(u);
}

/* decision P within-session sequence: move then trash, undo (trash), undo
 * (move) -- the second undo must not be silently skipped. */
static void
test_undo_move_then_trash_sequence(void) {
   Undo *u = undo_new();
   undo_record_move(u);
   undo_record_trash(u); /* trash now most recent */
   g_assert_cmpint(undo_choose(u, TRUE, TRUE), ==, GGAZE_UNDO_TRASH);
   undo_reset(u); /* first undo (trash) done; move still undoable */
   g_assert_cmpint(undo_choose(u, TRUE, TRUE), ==, GGAZE_UNDO_MOVE);
   undo_delete(u);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/undo/choose_prefers_recent",
                   test_undo_choose_prefers_recent);
   g_test_add_func("/undo/reset", test_undo_reset);
   g_test_add_func("/undo/move_then_trash_sequence",
                   test_undo_move_then_trash_sequence);
   return (g_test_run());
}