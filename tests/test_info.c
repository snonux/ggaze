/*:*
 * ggaze — info / EXIF unit test
 *
 * Gathers EXIF from the fixtures: plain.jpg (orientation 1) and rot6.jpg
 * (orientation 6, 8x4 stored). Asserts dimensions, orientation, and that
 * the format string is non-empty. Uses $GGAZE_FIXTURES_DIR.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "info.h"

#include <glib.h>

static GgazeInfo *
info_from_fixture(const char *c_name) {
   const char *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   char  *c_path = g_build_filename(c_dir, c_name, NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);
   GgazeInfo *p_info = info_new(p_file);
   g_assert_nonnull(p_info);
   g_object_unref(p_file);
   return (p_info);
}

static void
test_plain_jpeg(void) {
   GgazeInfo *p_info = info_from_fixture("plain.jpg");
   g_assert_cmpint(p_info->i_width, ==, 6);
   g_assert_cmpint(p_info->i_height, ==, 3);
   g_assert_cmpint(p_info->i_orientation, ==, 1); /* Horizontal (normal) */
   g_assert_nonnull(p_info->c_format);
   g_assert(p_info->i_size > 0);
   char *c_fmt = info_format(p_info);
   g_assert_nonnull(c_fmt);
   g_free(c_fmt);
   info_delete(p_info);
}

static void
test_rotated_jpeg(void) {
   GgazeInfo *p_info = info_from_fixture("rot6.jpg");
   /* Stored dimensions are 8x4 (before orientation is applied). */
   g_assert_cmpint(p_info->i_width, ==, 8);
   g_assert_cmpint(p_info->i_height, ==, 4);
   g_assert_cmpint(p_info->i_orientation, ==, 6); /* Rotate 90 CW */
   info_delete(p_info);
}

static void
test_png_no_exif(void) {
   GgazeInfo *p_info = info_from_fixture("small.png");
   g_assert_cmpint(p_info->i_width, ==, 5);
   g_assert_cmpint(p_info->i_height, ==, 2);
   g_assert_cmpint(p_info->i_orientation, ==, 0); /* no EXIF */
   g_assert_null(p_info->c_camera);               /* no EXIF → no camera */
   info_delete(p_info);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   if (g_getenv("GGAZE_FIXTURES_DIR") == NULL) {
      g_test_skip("GGAZE_FIXTURES_DIR unset");
      return (g_test_run());
   }
   g_test_add_func("/info/plain_jpeg", test_plain_jpeg);
   g_test_add_func("/info/rotated_jpeg", test_rotated_jpeg);
   g_test_add_func("/info/png_no_exif", test_png_no_exif);
   return (g_test_run());
}