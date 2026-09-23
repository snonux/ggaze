/*:*
 * ggaze — how many times one regular file is opened during a call under
 * test (AGENTS.md: "Shared helpers go in tests/helpers/")
 *
 * An inotify watch on the file. The kernel queues IN_OPEN inside openat()
 * itself, synchronously, whoever the opener is (this process or a glycin
 * sandbox), so once the call under test has returned the queue holds
 * exactly its opens: no helper thread, no deadline, no assumption about
 * which end closes first. One kernel rule shapes the watch: inotify
 * COALESCES an event identical (wd, mask, cookie, name) to the one at the
 * tail of its unread queue, so two back-to-back IN_OPENs would count as
 * one (observed: 1 for the two opens of the loader's oversized-JPEG case).
 * Watching IN_CLOSE as well keeps every IN_OPEN distinct, because each
 * open the code under test makes is closed before the next -- the
 * loader's sniff stream is unreffed before detect's peek opens, that
 * closes before gdk-pixbuf's fopen -- so the queue alternates OPEN, CLOSE,
 * OPEN, CLOSE and nothing merges; ggtest_open_counter_finish() asserts
 * that alternation. It is the only proof from OUTSIDE the code under test
 * that a file was NOT touched: test_loader_pixbuf counts the loader's
 * opens exactly (was gdk-pixbuf consulted?), test_thumbnail asserts zero
 * for a pre-cancelled request (neither the cache entry nor the source).
 * Linux-only, like the app.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#ifndef GGAZE_TEST_OPEN_COUNTER_H
#define GGAZE_TEST_OPEN_COUNTER_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
   int i_inotify; /* the watch's instance; -1 once finished */
} GgtestOpenCounter;

/* inotify_init1()/inotify_add_watch() fail for reasons that have nothing
 * to do with the code under test -- fs.inotify.max_user_instances
 * exhausted by a desktop full of file watchers is the usual one -- so a
 * harness reports such a failure with its errno text (g_error), where a
 * bare "assertion failed: (fd >= 0)" reads like a wrong open count. */
void ggtest_assert_inotify_ok(int i_ret, const char *c_call);

/* Start counting opens of the existing file at c_path. Asserts (with the
 * errno text) that the watch could be armed. */
void ggtest_open_counter_start(GgtestOpenCounter *p_c, const char *c_path);

/* Drain the queue, assert every OPEN was followed by its CLOSE before the
 * next (the alternation the header relies on), tear the watch down and
 * return the number of opens. The file itself is the caller's to remove. */
guint ggtest_open_counter_finish(GgtestOpenCounter *p_c);

G_END_DECLS

#endif /* GGAZE_TEST_OPEN_COUNTER_H */
