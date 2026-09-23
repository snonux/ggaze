/*:*
 * ggaze — read and set a file's stamp in the test suites
 *
 * See file_stamp.h.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "file_stamp.h"

#include <gio/gio.h>

void
ggtest_read_stamp(const char *c_path, GgtestFileStamp *p_st) {
   GFile     *p_f    = g_file_new_for_path(c_path);
   GError    *p_err  = NULL;
   GFileInfo *p_info = g_file_query_info(p_f,
                                         G_FILE_ATTRIBUTE_STANDARD_SIZE
                                         "," G_FILE_ATTRIBUTE_TIME_MODIFIED
                                         "," G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC
                                         "," G_FILE_ATTRIBUTE_UNIX_INODE,
                                         G_FILE_QUERY_INFO_NONE, NULL, &p_err);
   g_assert_no_error(p_err);
   p_st->i_size = g_file_info_get_size(p_info);
   p_st->u_sec =
      g_file_info_get_attribute_uint64(p_info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
   p_st->u_nsec = g_file_info_get_attribute_uint32(
      p_info, G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC);
   p_st->u_inode =
      g_file_info_get_attribute_uint64(p_info, G_FILE_ATTRIBUTE_UNIX_INODE);
   g_object_unref(p_info);
   g_object_unref(p_f);
}

void
ggtest_set_mtime(const char *c_path, guint64 u_sec, guint32 u_nsec) {
   GFile     *p_f    = g_file_new_for_path(c_path);
   GFileInfo *p_info = g_file_info_new();
   GError    *p_err  = NULL;
   g_file_info_set_attribute_uint64(p_info, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                    u_sec);
   g_file_info_set_attribute_uint32(p_info, G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC,
                                    u_nsec);
   g_file_set_attributes_from_info(p_f, p_info, G_FILE_QUERY_INFO_NONE, NULL,
                                   &p_err);
   g_assert_no_error(p_err);
   g_object_unref(p_info);
   g_object_unref(p_f);
   GgtestFileStamp t_now;
   ggtest_read_stamp(c_path, &t_now);
   g_assert_cmpuint(t_now.u_sec, ==, u_sec);
   if (t_now.u_nsec != u_nsec) {
      g_error("%s: filesystem lacks sub-second mtimes (set %u ns, read back "
              "%u ns); the stamp subtests need a filesystem that keeps them "
              "(ext4, xfs, btrfs, tmpfs)",
              c_path, (guint)u_nsec, (guint)t_now.u_nsec);
   }
}

void
ggtest_require_subsecond_mtime(const char            *c_path,
                               const GgtestFileStamp *p_st) {
   if (p_st->u_nsec == 0) {
      g_error("%s: filesystem lacks sub-second mtimes (the mtime reads "
              ".000000000); the stamp subtests need a filesystem that keeps "
              "them (ext4, xfs, btrfs, tmpfs)",
              c_path);
   }
}
