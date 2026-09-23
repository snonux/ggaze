/*:*
 * ggaze — inotify open counter for one regular file (see open_counter.h)
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "open_counter.h"

#include <errno.h>
#include <sys/inotify.h>
#include <unistd.h>

void
ggtest_assert_inotify_ok(int i_ret, const char *c_call) {
   if (i_ret < 0) {
      g_error("%s failed: %s (fs.inotify.max_user_instances or "
              "max_user_watches exhausted?)",
              c_call, g_strerror(errno));
   }
}

/* IN_CLOSE (either close) rather than IN_CLOSE_NOWRITE alone: a regression
 * that opens the watched file for WRITING still ends in a close event, so
 * the alternation check names it instead of tripping over a merged pair. */
void
ggtest_open_counter_start(GgtestOpenCounter *p_c, const char *c_path) {
   p_c->i_inotify = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
   ggtest_assert_inotify_ok(p_c->i_inotify, "inotify_init1");
   ggtest_assert_inotify_ok(
      inotify_add_watch(p_c->i_inotify, c_path, IN_OPEN | IN_CLOSE),
      "inotify_add_watch");
}

/* Non-blocking drain: EAGAIN is "empty", anything else a failure. A watch
 * on a FILE never carries a name, so every event is exactly
 * sizeof(struct inotify_event). */
guint
ggtest_open_counter_finish(GgtestOpenCounter *p_c) {
   struct inotify_event evs[64];
   guint                u_opens = 0;
   gboolean             b_open  = FALSE; /* an OPEN awaits its CLOSE */
   for (;;) {
      ssize_t i_n = read(p_c->i_inotify, evs, sizeof(evs));
      if (i_n < 0) {
         g_assert_cmpint(errno, ==, EAGAIN);
         break;
      }
      g_assert_cmpint(i_n % (ssize_t)sizeof(evs[0]), ==, 0);
      for (ssize_t i = 0; i < i_n / (ssize_t)sizeof(evs[0]); i++) {
         gboolean b_is_open = (evs[i].mask & IN_OPEN) != 0;
         g_assert_cmpint(b_is_open, !=, b_open);
         b_open = b_is_open;
         u_opens += b_is_open ? 1 : 0;
      }
   }
   g_assert_false(b_open);
   close(p_c->i_inotify);
   p_c->i_inotify = -1;
   return (u_opens);
}
