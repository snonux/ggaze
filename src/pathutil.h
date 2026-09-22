#ifndef GGAZE_PATHUTIL_H
#define GGAZE_PATHUTIL_H

/*:*
 * ggaze — shared path/string helpers (DRY)
 *
 * Helpers that were duplicated across trash.c, mover.c, enhance-ctrl.c and
 * window.c: a stem/extension splitter, a symlink-safe "is this a real
 * directory" check + mkdir -p, and a non-colliding child-name finder.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

/* Split a basename into its stem and extension: "a.jpg" -> ("a", ".jpg"),
 * "a" -> ("a", ""), ".hidden" -> (".hidden", ""). *pc_stem is a new string
 * the caller frees; *pc_ext points into c_base (borrowed) or to "". */
void pathutil_split_ext(const char *c_base, char **pc_stem,
                        const char **pc_ext);

/* TRUE iff p_dir is a real directory (queried NOFOLLOW_SYMLINKS, so a symlink
 * to a directory is rejected). On failure sets G_IO_ERROR_NOT_DIRECTORY. */
gboolean pathutil_dir_is_safe(GFile *p_dir, GError **p_err);

/* Ensure p_dir exists (mkdir -p). G_IO_ERROR_EXISTS from
 * g_file_make_directory_with_parents() is treated as success once
 * pathutil_dir_is_safe() confirms the existing path is a real directory. */
gboolean pathutil_ensure_dir(GFile *p_dir, GError **p_err);

/* Return a non-colliding child of p_dir named "<c_prefix><c_suffix>" if that
 * is free, else "<c_prefix>-<n><c_suffix>" for n = u_start, u_start+1, ...
 * until a free name is found. The prefix and suffix are inserted verbatim --
 * they are never interpreted as a printf format, so a '%' in a file name is
 * safe. Returns a new GFile (caller unrefs) or NULL after 100000 tries.
 * Never overwrites an existing file. This is the ONE collision-naming rule in
 * ggaze: trash, move and enhance-save all produce "<stem>-<n><ext>". */
GFile *pathutil_unique_child(GFile *p_dir, const char *c_prefix,
                             const char *c_suffix, guint u_start);

G_END_DECLS

#endif /* GGAZE_PATHUTIL_H */