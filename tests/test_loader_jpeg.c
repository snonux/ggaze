/*:*
 * ggaze — libjpeg-turbo progressive loader unit test (feature-gated)
 *
 * Tests the progressive JPEG path: loader_load_async with a progress callback
 * fires the callback (low-res partial) and returns the full texture. Only
 * compiled when libjpeg-turbo is found (HAVE_JPEG).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "loader/loader.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

static volatile int g_progress_fired = 0;
static GMainLoop   *g_loop           = NULL;
static GdkTexture  *g_result         = NULL;

static void
progress_cb(GdkTexture *p_partial, gpointer p_data) {
   (void)p_partial;
   (void)p_data;
   g_atomic_int_set(&g_progress_fired, 1);
}

static void
finish_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   (void)p_data;
   GError *p_err = NULL;
   g_result      = loader_load_finish(p_res, &p_err);
   g_assert_no_error(p_err);
   g_main_loop_quit(g_loop);
}

static void
test_progressive_jpeg(void) {
   const char *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   char  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);

   g_progress_fired = 0;
   g_result         = NULL;
   g_loop           = g_main_loop_new(NULL, FALSE);
   loader_load_async(p_file, NULL, progress_cb, NULL, finish_cb, NULL);
   g_main_loop_run(g_loop);
   g_main_loop_unref(g_loop);
   g_loop = NULL;

   g_assert_nonnull(g_result);
   g_assert_cmpint(gdk_texture_get_width(g_result), ==, 6);
   g_assert_cmpint(gdk_texture_get_height(g_result), ==, 3);
   g_assert_cmpint(g_atomic_int_get(&g_progress_fired), ==, 1);
   g_object_unref(g_result);
   g_object_unref(p_file);
}

static void
test_progressive_rotated(void) {
   const char *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   char  *c_path = g_build_filename(c_dir, "rot6.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);

   g_progress_fired = 0;
   g_result         = NULL;
   g_loop           = g_main_loop_new(NULL, FALSE);
   loader_load_async(p_file, NULL, progress_cb, NULL, finish_cb, NULL);
   g_main_loop_run(g_loop);
   g_main_loop_unref(g_loop);

   /* Full result via GdkPixbuf applies EXIF orientation → 4x8. */
   g_assert_nonnull(g_result);
   g_assert_cmpint(gdk_texture_get_width(g_result), ==, 4);
   g_assert_cmpint(gdk_texture_get_height(g_result), ==, 8);
   g_object_unref(g_result);
   g_object_unref(p_file);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   if (g_getenv("GGAZE_FIXTURES_DIR") == NULL) {
      g_test_skip("GGAZE_FIXTURES_DIR unset");
      return (g_test_run());
   }
   g_test_add_func("/loader/jpeg/progressive", test_progressive_jpeg);
   g_test_add_func("/loader/jpeg/progressive_rotated",
                   test_progressive_rotated);
   return (g_test_run());
}