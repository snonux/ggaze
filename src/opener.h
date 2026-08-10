#ifndef GGAZE_OPENER_H
#define GGAZE_OPENER_H

/*:*
 * ggaze — launch external programs (`e`) with %f expansion
 *
 * Holds an ordered list of editors (SettingsPair: name + command, the command
 * may contain %f) read from GSettings. Plain-C.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "settings-pair.h"

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct Opener Opener;

Opener          *opener_new(void);
void             opener_delete(Opener *p_o);
void             opener_set_progs(Opener *p_o, const GPtrArray *p_progs);
const GPtrArray *opener_get_progs(Opener *p_o);

/* Launch the program (p_prog->c_value) with %f expanded to the file's path.
 * Detached. Returns TRUE if the process was started. */
gboolean opener_launch(Opener *p_o, GFile *p_file, const SettingsPair *p_prog,
                       GError **p_err);

G_END_DECLS

#endif /* GGAZE_OPENER_H */