#ifndef GGAZE_PATHUTIL_H
#define GGAZE_PATHUTIL_H

/*:*
 * ggaze — shared path/string helpers (DRY)
 *
 * Helpers that were duplicated across trash.c, mover.c, opener.c, runner.c
 * and window.c: a %-placeholder string replace, a symlink-safe "is this a
 * real directory" check + mkdir -p, and a non-colliding child-name finder.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

/* Replace all occurrences of c_old with c_new in c_str. Caller frees. Returns
 * NULL on NULL input. */
char *pathutil_str_replace(const char *c_str, const char *c_old,
                           const char *c_new);

/* TRUE iff p_dir is a real directory (queried NOFOLLOW_SYMLINKS, so a symlink
 * to a directory is rejected). On failure sets G_IO_ERROR_NOT_DIRECTORY. */
gboolean pathutil_dir_is_safe(GFile *p_dir, GError **p_err);

/* Ensure p_dir exists (mkdir -p). G_IO_ERROR_EXISTS from
 * g_file_make_directory_with_parents() is treated as success once
 * pathutil_dir_is_safe() confirms the existing path is a real directory. */
gboolean pathutil_ensure_dir(GFile *p_dir, GError **p_err);

/* Return a non-colliding child of p_dir whose basename is c_first if that is
 * free, else g_strdup_printf(c_fmt, n) for n = u_start, u_start+1, ...
 * (c_fmt MUST contain a single %u) until a free name is found. Returns a new
 * GFile (caller unrefs) or NULL after 100000 tries. Never overwrites an
 * existing file. */
GFile *pathutil_unique_child(GFile *p_dir, const char *c_first,
                             const char *c_fmt, guint u_start);

G_END_DECLS

#endif /* GGAZE_PATHUTIL_H */