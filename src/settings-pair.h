#ifndef GGAZE_SETTINGS_PAIR_H
#define GGAZE_SETTINGS_PAIR_H

/*:*
 * ggaze — shared (name, value) config pair
 *
 * The {name, value} shape used by the a(ss) GSettings keys (destinations,
 * editors, scripts) and, in turn, by the mover/opener/runner engines -- which
 * now hold SettingsPair directly instead of their own parallel MoverDest /
 * OpenerProg / RunnerScript types. The value is a path for destinations, a
 * command for editors/scripts. Kept in its own header so the engines do not
 * depend on the GSettings wrapper (settings.h).
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

/* Free a SettingsPair (g_free-safe: NULL is a no-op). Suitable as a
 * GPtrArray free func. */
void settings_pair_free(gpointer p);

G_END_DECLS

#endif /* GGAZE_SETTINGS_PAIR_H */