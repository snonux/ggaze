#ifndef GGAZE_RUNNER_H
#define GGAZE_RUNNER_H

/*:*
 * ggaze — async shell script runner (`!`) with %f/%d expansion
 *
 * Holds an ordered list of scripts (SettingsPair: name + command, the command
 * may contain %f and %d) read from GSettings. Plain-C.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "settings-pair.h"

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct Runner Runner;

Runner          *runner_new(void);
void             runner_delete(Runner *p_r);
void             runner_set_scripts(Runner *p_r, const GPtrArray *p_scripts);
const GPtrArray *runner_get_scripts(Runner *p_r);

/* Run the script (p_script->c_value) via /bin/sh -c with %f and %d expanded
 * (single-quoted). Async: calls p_cb (on the main thread) when the process
 * exits. p_data is passed to p_cb as-is. */
gboolean runner_run(Runner *p_r, GFile *p_file, GFile *p_dir,
                    const SettingsPair *p_script, GAsyncReadyCallback p_cb,
                    gpointer p_data, GError **p_err);

/* Finish: returns the exit code, or -1 on error. */
int runner_run_finish(Runner *p_r, GAsyncResult *p_res, GError **p_err);

G_END_DECLS

#endif /* GGAZE_RUNNER_H */