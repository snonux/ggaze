#ifndef GGAZE_SETTINGS_PAIR_H
#define GGAZE_SETTINGS_PAIR_H

/*:*
 * ggaze — shared (name, value) config pair
 *
 * The {name, value} shape used by the a(ss) GSettings keys (destinations,
 * editors, scripts) and, in turn, by the mover/opener/runner engines -- which
 * now hold SettingsPair directly instead of their own parallel MoverDest /
 * OpenerProg / RunnerScript types. The value is a path for destinations, a
 * command for editors/scripts. Kept in its own header + .c so the engines
 * depend on neither the GSettings wrapper's header nor its object file.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
   char *c_name;
   char *c_value;
} SettingsPair;

/* Construct / deep-copy a pair. Caller frees with settings_pair_free. */
SettingsPair *settings_pair_new(const char *c_name, const char *c_value);
SettingsPair *settings_pair_copy(const SettingsPair *p_src);

/* Free a SettingsPair (g_free-safe: NULL is a no-op). Suitable as a
 * GPtrArray free func. */
void settings_pair_free(gpointer p);

/* An empty GPtrArray whose free func is settings_pair_free. */
GPtrArray *settings_pair_array_new(void);

/* Deep-copy an array of SettingsPair* (NULL -> empty array). Caller unrefs. */
GPtrArray *settings_pair_array_copy(const GPtrArray *p_src);

G_END_DECLS

#endif /* GGAZE_SETTINGS_PAIR_H */