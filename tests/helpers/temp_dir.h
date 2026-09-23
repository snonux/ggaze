/*:*
 * ggaze — shared temp-folder teardown for the test suites (AGENTS.md:
 * "Shared helpers go in tests/helpers/")
 *
 * Every suite that works on a copy of the fixtures makes a g_dir_make_tmp()
 * folder and has to take it down again -- recursively, because trash.c's
 * lazy "./.Trash" bin and mover/export subtests leave subfolders behind, and
 * a non-empty directory cannot be g_file_delete()d. The old per-suite
 * copies ignored every failure, so a folder left behind (a file still being
 * written by an export that had not landed, a permission a subtest forgot
 * to restore) went unnoticed and piled up in $TMPDIR. This one ASSERTS each
 * delete and the final rmdir: a leftover is a test bug and fails the suite
 * that caused it.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#ifndef GGAZE_TEST_TEMP_DIR_H
#define GGAZE_TEST_TEMP_DIR_H

#include <gio/gio.h>

/* Delete everything under p_dir (files and subfolders, depth first), leaving
 * p_dir itself empty. Asserts the enumeration and every delete. A symlink
 * is deleted as a link and never followed: a link to a folder outside the
 * tree must not have its TARGET emptied. */
void ggtest_remove_tree(GFile *p_dir);

/* ggtest_remove_tree(c_dir) and then the folder itself; asserts that final
 * rmdir succeeded. Takes ownership of c_dir (freed on return), so the usual
 * call is the last line of a subtest. */
void ggtest_cleanup_temp_dir(char *c_dir);

#endif /* GGAZE_TEST_TEMP_DIR_H */
