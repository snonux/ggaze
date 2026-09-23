/*:*
 * ggaze — wait for a condition, bounded by a monotonic deadline (see
 * wait_until.h)
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "wait_until.h"

#include <glib.h>

gdouble
ggtest_wait_scale(void) {
   static gdouble d_scale = -1.0;
   if (d_scale < 0.0) {
      const char *c_env = g_getenv("GGAZE_TEST_TIMEOUT_SCALE");
      d_scale           = (c_env != NULL) ? g_ascii_strtod(c_env, NULL) : 1.0;
      if (!(d_scale > 0.0)) {
         d_scale = 1.0; /* unset, unparseable or nonsense: ignore it */
      }
#ifdef __SANITIZE_ADDRESS__
      d_scale *= 3.0;
#endif
   }
   return (d_scale);
}

/* The condition is polled BEFORE the deadline is checked, so a condition
 * that holds at the very moment the budget runs out still counts as met:
 * the deadline decides only when to stop asking. */
gboolean
ggtest_wait_until(GgtestCondFn p_fn, gpointer p_data, gint64 i_budget_us) {
   gint64 i_deadline = g_get_monotonic_time() +
                       (gint64)((gdouble)i_budget_us * ggtest_wait_scale());
   for (;;) {
      if (p_fn(p_data)) {
         return (TRUE);
      }
      if (g_get_monotonic_time() >= i_deadline) {
         return (FALSE);
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}
