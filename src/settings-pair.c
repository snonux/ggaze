/*:*
 * ggaze — shared (name, value) config pair
 *
 * See settings-pair.h. The one implementation of construct / copy / free for
 * the pair the a(ss) settings keys and the mover/opener/runner engines share;
 * every consumer used to hand-roll the two-strdup copy loop.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "settings-pair.h"

#include <glib.h>

SettingsPair *
settings_pair_new(const char *c_name, const char *c_value) {
   SettingsPair *p_pr = g_new(SettingsPair, 1);
   p_pr->c_name       = g_strdup(c_name);
   p_pr->c_value      = g_strdup(c_value);
   return (p_pr);
}

SettingsPair *
settings_pair_copy(const SettingsPair *p_src) {
   g_return_val_if_fail(p_src != NULL, NULL);
   return (settings_pair_new(p_src->c_name, p_src->c_value));
}

void
settings_pair_free(gpointer p) {
   SettingsPair *p_pr = (SettingsPair *)p;
   if (p_pr == NULL) {
      return;
   }
   g_free(p_pr->c_name);
   g_free(p_pr->c_value);
   g_free(p_pr);
}

GPtrArray *
settings_pair_array_new(void) {
   return (g_ptr_array_new_with_free_func(settings_pair_free));
}

GPtrArray *
settings_pair_array_copy(const GPtrArray *p_src) {
   GPtrArray *p_out = settings_pair_array_new();
   for (guint u = 0; p_src != NULL && u < p_src->len; u++) {
      g_ptr_array_add(
         p_out, settings_pair_copy(g_ptr_array_index((GPtrArray *)p_src, u)));
   }
   return (p_out);
}
