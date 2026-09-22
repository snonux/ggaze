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
/* Expand %f in c_cmd into an argv vector: the template is shell-parsed first,
 * then %f is substituted into the parsed elements, so the path is always one
 * argv value. Returns a NULL-terminated vector (g_strfreev) or NULL with
 * p_err on a parse error. Public so tests can check the argv shape. */
char **opener_expand_command(const char *c_cmd, GFile *p_file, GError **p_err);

gboolean opener_launch(GFile *p_file, const SettingsPair *p_prog,
                       GError **p_err);

G_END_DECLS

#endif /* GGAZE_OPENER_H */