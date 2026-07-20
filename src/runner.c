/* runner.c — async shell script runner with %f/%d expansion + injection guard.
 */
#include "runner.h"

struct Runner {
   GPtrArray *p_scripts;
};

static void
_script_free(gpointer p) {
   RunnerScript *d = (RunnerScript *)p;
   if (d) {
      g_free(d->c_name);
      g_free(d->c_command);
      g_free(d);
   }
}

Runner *
runner_new(void) {
   Runner *r    = g_new0(Runner, 1);
   r->p_scripts = g_ptr_array_new_with_free_func(_script_free);
   return r;
}

void
runner_delete(Runner *r) {
   if (!r)
      return;
   g_ptr_array_unref(r->p_scripts);
   g_free(r);
}

void
runner_set_scripts(Runner *r, const GPtrArray *p) {
   g_return_if_fail(r);
   g_ptr_array_set_size(r->p_scripts, 0);
   if (!p)
      return;
   for (guint i = 0; i < p->len; i++) {
      const RunnerScript *src = g_ptr_array_index((GPtrArray *)p, i);
      RunnerScript       *ns  = g_new(RunnerScript, 1);
      ns->c_name              = g_strdup(src->c_name);
      ns->c_command           = g_strdup(src->c_command);
      g_ptr_array_add(r->p_scripts, ns);
   }
}

const GPtrArray *
runner_get_scripts(Runner *r) {
   return r ? r->p_scripts : NULL;
}

/* Single-quote a path for safe shell interpolation. Caller frees. */
static char *
_shell_quote(const char *c_path) {
   /* g_shell_quote wraps the path in single quotes and escapes any
    * embedded quotes, making it safe to interpolate into a sh -c script. */
   return g_shell_quote(c_path);
}

/* Replace all occurrences of c_old with c_new in c_str. Caller frees. */
static char *
_str_replace(const char *c_str, const char *c_old, const char *c_new) {
   if (c_str == NULL || c_old == NULL || c_new == NULL)
      return NULL;
   GString    *p_out     = g_string_new(NULL);
   const char *p         = c_str;
   gsize       u_old_len = strlen(c_old);
   while (*p) {
      if (strncmp(p, c_old, u_old_len) == 0) {
         g_string_append(p_out, c_new);
         p += u_old_len;
      } else {
         g_string_append_c(p_out, *p);
         p++;
      }
   }
   return g_string_free(p_out, FALSE);
}

static char *
_expand(const char *c_cmd, GFile *p_file, GFile *p_dir) {
   char *c_fpath = p_file ? g_file_get_path(p_file) : g_strdup("");
   char *c_dpath = p_dir ? g_file_get_path(p_dir) : g_strdup("");
   char *c_fq    = _shell_quote(c_fpath);
   char *c_dq    = _shell_quote(c_dpath);
   g_free(c_fpath);
   g_free(c_dpath);
   char *r1 = _str_replace(c_cmd, "%f", c_fq);
   char *r2 = _str_replace(r1 ? r1 : c_cmd, "%d", c_dq);
   g_free(r1);
   g_free(c_fq);
   g_free(c_dq);
   return r2 ? r2 : g_strdup(c_cmd);
}

gboolean
runner_run(Runner *r, GFile *p_file, GFile *p_dir, const RunnerScript *p_script,
           GAsyncReadyCallback p_cb, gpointer p_data, GError **p_err) {
   (void)r;
   g_return_val_if_fail(p_script, FALSE);
   char *c_cmd = _expand(p_script->c_command, p_file, p_dir);
   if (c_cmd == NULL) {
      g_set_error(p_err, G_SHELL_ERROR, G_SHELL_ERROR_FAILED,
                  "runner: failed to expand script command");
      return FALSE;
   }
   /* Pass the whole expanded command as a single argv element to sh -c so
    * that pipelines, redirections, && and multi-word arguments are parsed
    * by the shell as one script (not split by g_shell_parse_argv, which
    * would feed sh -c only the first word and treat the rest as $0/$1...).
    * %f/%d are already single-quoted by _expand, so paths stay safe. */
   const char  *argv[] = {"/bin/sh", "-c", c_cmd, NULL};
   GSubprocess *p_sub = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_NONE, p_err);
   g_free(c_cmd);
   if (p_sub == NULL)
      return FALSE;
   if (p_cb) {
      g_subprocess_wait_check_async(p_sub, NULL, p_cb, p_data);
   }
   g_object_unref(p_sub);
   return TRUE;
}

int
runner_run_finish(Runner *r, GAsyncResult *p_res, GError **p_err) {
   (void)r;
   GSubprocess *p_sub = G_SUBPROCESS(g_async_result_get_source_object(p_res));
   if (p_sub == NULL)
      return -1;
   gboolean ok = g_subprocess_wait_check_finish(p_sub, p_res, p_err);
   g_object_unref(p_sub);
   return ok ? 0 : -1;
}