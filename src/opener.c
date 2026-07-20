/* opener.c — launch external programs with %f expansion. Detached GSubprocess.
 */
#include "opener.h"

struct Opener {
   GPtrArray *p_progs; /* OpenerProg* */
};

static void
_prog_free(gpointer p) {
   OpenerProg *d = (OpenerProg *)p;
   if (d) {
      g_free(d->c_name);
      g_free(d->c_command);
      g_free(d);
   }
}

Opener *
opener_new(void) {
   Opener *o  = g_new0(Opener, 1);
   o->p_progs = g_ptr_array_new_with_free_func(_prog_free);
   return o;
}

void
opener_delete(Opener *o) {
   if (!o)
      return;
   g_ptr_array_unref(o->p_progs);
   g_free(o);
}

void
opener_set_progs(Opener *o, const GPtrArray *p) {
   g_return_if_fail(o);
   g_ptr_array_set_size(o->p_progs, 0);
   if (!p)
      return;
   for (guint i = 0; i < p->len; i++) {
      const OpenerProg *src = g_ptr_array_index((GPtrArray *)p, i);
      OpenerProg       *np  = g_new(OpenerProg, 1);
      np->c_name            = g_strdup(src->c_name);
      np->c_command         = g_strdup(src->c_command);
      g_ptr_array_add(o->p_progs, np);
   }
}

const GPtrArray *
opener_get_progs(Opener *o) {
   return o ? o->p_progs : NULL;
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

/* Expand %f → the file path inside an already-parsed argv.
 *
 * The command template is parsed with g_shell_parse_argv FIRST, so quotes,
 * escapes, and option flags are resolved correctly; %f is then substituted
 * into the parsed argv elements. This guarantees %f is always exactly one
 * argv value no matter what spaces or shell metacharacters the path contains
 * (substitution happens after shell parsing, never before).
 *
 * Returns a newly-allocated, NULL-terminated argv vector; caller frees with
 * g_strfreev. On a parse error returns NULL and sets p_err — never launches
 * malformed argv. Non-static (with the `_` prefix retained to mark it as
 * non-public) so tests/test_opener.c can verify the argv shape directly via
 * an extern prototype, without polluting the public opener.h. */
char **
_expand_command(const char *c_cmd, GFile *p_file, GError **p_err) {
   g_return_val_if_fail(c_cmd != NULL, NULL);
   g_return_val_if_fail(G_IS_FILE(p_file), NULL);
   char *c_path = g_file_get_path(p_file);
   if (c_path == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "opener: file has no local path");
      return NULL;
   }
   /* Parse the raw template first: resolves quotes/escapes/options. */
   char **argv = NULL;
   if (!g_shell_parse_argv(c_cmd, NULL, &argv, p_err)) {
      g_free(c_path);
      return NULL;
   }
   /* Substitute %f into each parsed element so the path is always one argv
    * value, regardless of spaces or shell metacharacters it contains. */
   for (guint i = 0; argv[i]; i++) {
      if (g_str_equal(argv[i], "%f")) {
         g_free(argv[i]);
         argv[i] = g_strdup(c_path);
      } else if (strstr(argv[i], "%f")) {
         char *r = _str_replace(argv[i], "%f", c_path);
         if (r) {
            g_free(argv[i]);
            argv[i] = r;
         }
      }
   }
   g_free(c_path);
   return argv;
}

gboolean
opener_launch(Opener *o, GFile *p_file, const OpenerProg *p_prog,
              GError **p_err) {
   (void)o;
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   g_return_val_if_fail(p_prog, FALSE);
   char **argv = _expand_command(p_prog->c_command, p_file, p_err);
   if (argv == NULL)
      return FALSE;
   GSubprocess *p_sub = g_subprocess_newv((const char *const *)argv,
                                          G_SUBPROCESS_FLAGS_NONE, p_err);
   g_strfreev(argv);
   if (p_sub == NULL)
      return FALSE;
   /* Detached: don't wait. Unref immediately (process keeps running). */
   g_object_unref(p_sub);
   return TRUE;
}