#ifndef GGAZE_RUNNER_H
#define GGAZE_RUNNER_H

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
   char *c_name;
   char *c_command; /* may contain %f and %d */
} RunnerScript;

typedef struct Runner Runner;

Runner          *runner_new(void);
void             runner_delete(Runner *p_r);
void             runner_set_scripts(Runner *p_r, const GPtrArray *p_scripts);
const GPtrArray *runner_get_scripts(Runner *p_r);

/* Run the script via /bin/sh -c with %f and %d expanded (single-quoted).
 * Async: calls p_cb (on the main thread) when the process exits. p_data
 * is passed to p_cb as-is. */
gboolean runner_run(Runner *p_r, GFile *p_file, GFile *p_dir,
                    const RunnerScript *p_script, GAsyncReadyCallback p_cb,
                    gpointer p_data, GError **p_err);

/* Finish: returns the exit code, or -1 on error. */
int runner_run_finish(Runner *p_r, GAsyncResult *p_res, GError **p_err);

G_END_DECLS

#endif