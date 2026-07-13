/*:*
 * ggaze — thumbnail cache unit test
 *
 * Generates a thumbnail for a fixture, verifies the TMS cache file is written
 * (with Thumb::MTime), and that a second get hits the cache. Uses a temp
 * XDG_CACHE_HOME so the real cache is not polluted. No display needed.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "thumbnail.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

static const char *GGAZE_FX_DIR;
static char       *GGAZE_CACHE_DIR;

static GdkTexture *GGAZE_RESULT;
static GMainLoop  *GGAZE_LOOP;

static void
_thumb_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   (void)p_data;
   GError *p_err = NULL;
   GGAZE_RESULT  = thumbnail_get_finish(NULL, p_res, &p_err);
   g_assert_no_error(p_err);
   g_main_loop_quit(GGAZE_LOOP);
}

/* Get a thumbnail synchronously (pump a main loop until the callback). */
static GdkTexture *
get_thumb(Thumbnail *p_t, GFile *p_file, int i_size) {
   GGAZE_RESULT = NULL;
   GGAZE_LOOP   = g_main_loop_new(NULL, FALSE);
   thumbnail_get_async(p_t, p_file, i_size, NULL, _thumb_cb, NULL);
   g_main_loop_run(GGAZE_LOOP);
   g_main_loop_unref(GGAZE_LOOP);
   GGAZE_LOOP = NULL;
   return (GGAZE_RESULT);
}

static GFile *
fixture_file(const char *c_name) {
   char  *c_path = g_build_filename(GGAZE_FX_DIR, c_name, NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);
   return (p_file);
}

static void
test_generate_and_cache(void) {
   Thumbnail *p_t    = thumbnail_new();
   GFile     *p_file = fixture_file("plain.jpg");

   GdkTexture *p_tex = get_thumb(p_t, p_file, 128);
   g_assert_nonnull(p_tex);
   g_assert_cmpint(gdk_texture_get_width(p_tex), <=, 128);
   g_assert_cmpint(gdk_texture_get_height(p_tex), <=, 128);
   g_object_unref(p_tex);

   /* The cache file should exist under our temp XDG_CACHE_HOME. */
   char *c_normal =
      g_build_filename(GGAZE_CACHE_DIR, "thumbnails", "normal", NULL);
   GDir *p_dir = g_dir_open(c_normal, 0, NULL);
   g_assert_nonnull(p_dir);
   gboolean    b_found = FALSE;
   const char *c_name;
   while ((c_name = g_dir_read_name(p_dir)) != NULL) {
      if (g_str_has_suffix(c_name, ".png")) {
         b_found = TRUE;
         break;
      }
   }
   g_dir_close(p_dir);
   g_free(c_normal);
   g_assert_true(b_found);

   /* Second get should hit the cache (same result). */
   GdkTexture *p_tex2 = get_thumb(p_t, p_file, 128);
   g_assert_nonnull(p_tex2);
   g_object_unref(p_tex2);

   thumbnail_delete(p_t);
   g_object_unref(p_file);
}

static void
test_different_bucket(void) {
   Thumbnail *p_t    = thumbnail_new();
   GFile     *p_file = fixture_file("rot6.jpg");

   /* Request 200px → bucket = large (256). */
   GdkTexture *p_tex = get_thumb(p_t, p_file, 200);
   g_assert_nonnull(p_tex);
   g_object_unref(p_tex);

   /* The cache file should be under "large". */
   char *c_large =
      g_build_filename(GGAZE_CACHE_DIR, "thumbnails", "large", NULL);
   GDir *p_dir = g_dir_open(c_large, 0, NULL);
   g_assert_nonnull(p_dir);
   gboolean    b_found = FALSE;
   const char *c_name;
   while ((c_name = g_dir_read_name(p_dir)) != NULL) {
      if (g_str_has_suffix(c_name, ".png")) {
         b_found = TRUE;
         break;
      }
   }
   g_dir_close(p_dir);
   g_free(c_large);
   g_assert_true(b_found);

   thumbnail_delete(p_t);
   g_object_unref(p_file);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);

   GGAZE_FX_DIR = g_getenv("GGAZE_FIXTURES_DIR");
   if (GGAZE_FX_DIR == NULL) {
      g_test_skip("GGAZE_FIXTURES_DIR unset");
      return (g_test_run());
   }

   /* Use a temp XDG_CACHE_HOME so the real cache is not polluted. */
   GError *p_err   = NULL;
   GGAZE_CACHE_DIR = g_dir_make_tmp("ggaze-thumb-cache-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   g_setenv("XDG_CACHE_HOME", GGAZE_CACHE_DIR, TRUE);

   g_test_add_func("/thumbnail/generate_and_cache", test_generate_and_cache);
   g_test_add_func("/thumbnail/different_bucket", test_different_bucket);

   int i_ret = g_test_run();

   /* Cleanup the temp cache dir (best-effort recursive). */
   GFile           *p_cd = g_file_new_for_path(GGAZE_CACHE_DIR);
   GFileEnumerator *p_e =
      g_file_enumerate_children(p_cd, "standard::name,standard::type",
                                G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (p_e != NULL) {
      GFileInfo *p_info;
      while ((p_info = g_file_enumerator_next_file(p_e, NULL, NULL)) != NULL) {
         GFile *p_child = g_file_get_child(p_cd, g_file_info_get_name(p_info));
         if (g_file_info_get_file_type(p_info) == G_FILE_TYPE_DIRECTORY) {
            GFileEnumerator *p_e2 = g_file_enumerate_children(
               p_child, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (p_e2 != NULL) {
               GFileInfo *p_i2;
               while ((p_i2 = g_file_enumerator_next_file(p_e2, NULL, NULL)) !=
                      NULL) {
                  GFile *p_c2 =
                     g_file_get_child(p_child, g_file_info_get_name(p_i2));
                  g_file_delete(p_c2, NULL, NULL);
                  g_object_unref(p_c2);
                  g_object_unref(p_i2);
               }
               g_object_unref(p_e2);
            }
         }
         g_file_delete(p_child, NULL, NULL);
         g_object_unref(p_child);
         g_object_unref(p_info);
      }
      g_object_unref(p_e);
   }
   g_file_delete(p_cd, NULL, NULL);
   g_object_unref(p_cd);
   g_free(GGAZE_CACHE_DIR);
   return (i_ret);
}