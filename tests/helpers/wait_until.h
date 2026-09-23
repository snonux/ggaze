/*:*
 * ggaze — wait for a condition, bounded by a monotonic deadline (AGENTS.md:
 * "Shared helpers go in tests/helpers/")
 *
 * A test that waits for work on another thread -- a decode in the thumbnail
 * pool, a window's finalize, a dialog being raised -- is waiting for a
 * CONDITION, and the wait has to be written as one: poll the condition,
 * give up only once a real clock says the budget is spent. The pattern it
 * replaces is "N iterations of a 1 ms sleep, then assert": that loop stays
 * ~N ms however starved the thread it waits on is, because a sleeping poll
 * loop is barely slowed by an oversubscribed box while the worker it waits
 * for is starved of CPU by exactly that oversubscription. So the wait
 * always passed with the suite run alone and failed on a busy parallel
 * lane (1w0 for the dialog waits in gtk_helpers.c, hd2 for
 * test_thumbnail's queue drain: 23 == 24 once at load average ~10).
 *
 * Widening a condition wait cannot hide a defect: the wait returns the
 * instant the condition holds, so a condition that is genuinely never met
 * still fails the caller's assertion, just later. The budget only sets how
 * long the test is willing to wait before calling it a bug, and only one
 * wait per run can ever burn it -- the assertion aborts the suite.
 *
 * Plain GLib, no GTK, so the unit suites can link it without a display.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#ifndef GGAZE_TEST_WAIT_UNTIL_H
#define GGAZE_TEST_WAIT_UNTIL_H

#include <glib.h>

G_BEGIN_DECLS

/* TRUE once the awaited condition holds. Called from the waiting (main)
 * thread between main-context iterations, so it may read state that the
 * awaited callbacks write on the main thread without any locking. */
typedef gboolean (*GgtestCondFn)(gpointer p_data);

/* Scale factor every wait budget in tests/helpers/ is multiplied by.
 *
 * ASan/UBSan builds run several times slower than the plain lanes, and
 * they are the same lanes meson packs nproc-wide in parallel, so they get
 * 3x. GGAZE_TEST_TIMEOUT_SCALE lets a slow or heavily loaded machine widen
 * every wait without a rebuild; unset, unparseable or nonsense is 1.
 * Computed once; the test main loop is single-threaded, so the cached
 * value needs no locking. */
gdouble ggtest_wait_scale(void);

/* Poll p_fn(p_data), iterating the default main context (non-blocking, so
 * the callbacks the condition depends on get delivered) with a 1 ms sleep
 * between polls, until it returns TRUE or i_budget_us (scaled by
 * ggtest_wait_scale()) of monotonic time has passed. Returns whether the
 * condition held; the caller turns FALSE into a NAMED assertion that says
 * what it was waiting for, since a bare "should be 24" cannot tell "never
 * happened" from "not yet". */
gboolean ggtest_wait_until(GgtestCondFn p_fn, gpointer p_data,
                           gint64 i_budget_us);

G_END_DECLS

#endif /* GGAZE_TEST_WAIT_UNTIL_H */
