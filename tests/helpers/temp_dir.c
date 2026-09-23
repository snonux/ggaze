/*:*
 * ggaze — shared temp-folder teardown (see temp_dir.h)
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "temp_dir.h"

#include <gio/gio.h>
#include <glib.h>

void
ggtest_remove_tree(GFile *p_dir) {
   GError *p_err = NULL;
   /* NOFOLLOW: the type of a symlink must be G_FILE_TYPE_SYMBOLIC_LINK, not
    * its target's, so a link to a directory is unlinked below instead of
    * recursed into -- following it deleted the TARGET's contents, which
    * may live outside the temp folder entirely. g_file_delete() on a link
    * removes the link itself. */
   GFileEnumerator *p_e = g_file_enumerate_children(
      p_dir, "standard::name,standard::type",
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &p_err);
   g_assert_no_error(p_err);
   GFileInfo *p_info;
   while ((p_info = g_file_enumerator_next_file(p_e, NULL, &p_err)) != NULL) {
      GFile *p_child = g_file_get_child(p_dir, g_file_info_get_name(p_info));
      if (g_file_info_get_file_type(p_info) == G_FILE_TYPE_DIRECTORY) {
         ggtest_remove_tree(p_child);
      }
      g_assert_true(g_file_delete(p_child, NULL, &p_err));
      g_assert_no_error(p_err);
      g_object_unref(p_child);
      g_object_unref(p_info);
   }
   g_assert_no_error(p_err); /* next_file returns NULL on an error too */
   g_object_unref(p_e);
}

void
ggtest_cleanup_temp_dir(char *c_dir) {
   GError *p_err = NULL;
   GFile  *p_dir = g_file_new_for_path(c_dir);
   ggtest_remove_tree(p_dir);
   g_assert_true(g_file_delete(p_dir, NULL, &p_err));
   g_assert_no_error(p_err);
   g_object_unref(p_dir);
   g_free(c_dir);
}
