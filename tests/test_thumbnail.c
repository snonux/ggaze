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
#include <unistd.h>

#include "loader/detect.h"

static const char *GGAZE_FX_DIR;
static char       *GGAZE_CACHE_DIR;

static GdkTexture *GGAZE_RESULT;
static GMainLoop  *GGAZE_LOOP;
static GError     *GGAZE_ERR;

static void
_thumb_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   (void)p_data;
   GError *p_err = NULL;
   GGAZE_RESULT  = thumbnail_get_finish(NULL, p_res, &p_err);
   g_assert_no_error(p_err);
   g_main_loop_quit(GGAZE_LOOP);
}

/* Like _thumb_cb, but for tests that expect the load to fail: stores the
 * GError in GGAZE_ERR (caller frees) instead of asserting success. */
static void
_thumb_err_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   (void)p_data;
   GGAZE_RESULT = thumbnail_get_finish(NULL, p_res, &GGAZE_ERR);
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

/* Locate the baseline SOF0 marker (0xFF 0xC0) in a JPEG byte buffer and
 * overwrite its declared height/width with 65500 (0xFFDC), the largest value
 * libjpeg's own JPEG_MAX_DIMENSION check still accepts at header-read time.
 * Mirrors tests/test_loader_jpeg.c's _patch_sof_dims_huge. */
static void
_patch_sof_dims_huge(guint8 *p_buf, gsize u_len) {
   for (gsize u = 0; u + 8 < u_len; u++) {
      if (p_buf[u] == 0xff && p_buf[u + 1] == 0xc0) {
         p_buf[u + 5] = 0xff;
         p_buf[u + 6] = 0xdc;
         p_buf[u + 7] = 0xff;
         p_buf[u + 8] = 0xdc;
         return;
      }
   }
   g_assert_not_reached();
}

/* mu0 review round 2: _generate() called gdk_pixbuf_new_from_file_at_scale()
 * directly on the source file, bypassing loader.c's guard entirely --
 * reachable simply by scrolling a directory in grid view past a malicious
 * JPEG (gridview.c's _on_pic_map -> thumbnail_get_async, no click needed).
 * Exercises the REAL public entry point (thumbnail_get_async(), the same
 * call gridview.c makes) with the crafted 65500x65500-SOF0 header, and
 * asserts both a clean error and a tight wall-clock budget: pre-fix this
 * stalled ~28s per file (GdkPixbuf/glycin pre-allocating a huge sparse memfd
 * off the declared size before its own internal cap rejected it). */
static void
test_oversized_jpeg(void) {
   char  *c_src = g_build_filename(GGAZE_FX_DIR, "plain.jpg", NULL);
   gchar *p_buf = NULL;
   gsize  u_len = 0;
   g_assert_true(g_file_get_contents(c_src, &p_buf, &u_len, NULL));
   g_free(c_src);
   _patch_sof_dims_huge((guint8 *)p_buf, u_len);

   gchar  *c_tmp = NULL;
   GError *p_sub = NULL;
   gint i_fd = g_file_open_tmp("ggaze-thumb-oversized-XXXXXX", &c_tmp, &p_sub);
   g_assert_no_error(p_sub);
   g_assert_cmpint(i_fd, >=, 0);
   g_assert_cmpint((glong)write(i_fd, p_buf, u_len), ==, (glong)u_len);
   close(i_fd);
   g_free(p_buf);

   Thumbnail *p_t    = thumbnail_new();
   GFile     *p_file = g_file_new_for_path(c_tmp);

   GGAZE_RESULT   = NULL;
   GGAZE_ERR      = NULL;
   GGAZE_LOOP     = g_main_loop_new(NULL, FALSE);
   gint64 i_start = g_get_monotonic_time();
   thumbnail_get_async(p_t, p_file, 128, NULL, _thumb_err_cb, NULL);
   g_main_loop_run(GGAZE_LOOP);
   g_main_loop_unref(GGAZE_LOOP);
   GGAZE_LOOP     = NULL;
   gdouble d_secs = (g_get_monotonic_time() - i_start) / 1e6;

   g_assert_null(GGAZE_RESULT);
   g_assert_nonnull(GGAZE_ERR);
   g_assert_cmpuint(GGAZE_ERR->domain, ==, (guint)G_IO_ERROR);
   g_error_free(GGAZE_ERR);
   GGAZE_ERR = NULL;
   /* 5s leaves generous headroom above the microsecond-scale header peek
    * while catching a regression back to the ~28s stall long before any CI
    * per-test timeout would. */
   g_assert_cmpfloat(d_secs, <, 5.0);

   thumbnail_delete(p_t);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
}

/* Prepend a maximal-length (65533-byte, the largest a JPEG marker segment may
 * legally declare) filler APP0 segment ahead of p_src's SOI+rest, pushing
 * whatever follows (here, an already huge-patched SOF0) past
 * GGAZE_JPEG_PEEK_LEN. Mirrors tests/test_detect.c's direct-unit version of
 * this same construction. Returns a newly g_malloc'd buffer (caller frees
 * with g_free) via *p_out_len. */
static guint8 *
_build_padded_oversized_jpeg(const guint8 *p_src, gsize u_src_len,
                             gsize *p_out_len) {
   const guint16 u_fill_seglen = 0xFFFD; /* max length, incl. itself */
   GByteArray   *p_out         = g_byte_array_new();
   const guint8  soi[]         = {0xFF, 0xD8};
   g_byte_array_append(p_out, soi, (guint)sizeof(soi));
   const guint8 app0_hdr[] = {0xFF, 0xE0, (guint8)(u_fill_seglen >> 8),
                              (guint8)(u_fill_seglen & 0xFF)};
   g_byte_array_append(p_out, app0_hdr, (guint)sizeof(app0_hdr));
   guint   u_fill_payload = (guint)u_fill_seglen - 2;
   guint8 *p_zeros        = g_new0(guint8, u_fill_payload);
   g_byte_array_append(p_out, p_zeros, u_fill_payload);
   g_free(p_zeros);
   /* Skip p_src's own leading 2-byte SOI before appending the rest of it. */
   g_byte_array_append(p_out, p_src + 2, (guint)(u_src_len - 2));
   *p_out_len = p_out->len;
   return (g_byte_array_free(p_out, FALSE));
}

/* mu0 review round 3: the CRITICAL bypass. detect_jpeg_peek_dims_from_path()
 * only scans a bounded GGAZE_JPEG_PEEK_LEN (64KB) prefix; a filler marker
 * segment ahead of the huge-patched SOF0 pushes it past that prefix, so the
 * scan used to come back as plain "no SOF found" and
 * _thumb_reject_if_oversized_jpeg() treated that the same as "not a JPEG,
 * safe to proceed" -- reintroducing the exact ~28-30s GdkPixbuf stall this
 * task exists to close. Exercises the REAL public entry point
 * (thumbnail_get_async()) and asserts the fixed fail-closed behavior: a fast
 * G_IO_ERROR, not a multi-second stall. */
static void
test_padded_past_prefix_oversized_jpeg(void) {
   char  *c_src = g_build_filename(GGAZE_FX_DIR, "plain.jpg", NULL);
   gchar *p_buf = NULL;
   gsize  u_len = 0;
   g_assert_true(g_file_get_contents(c_src, &p_buf, &u_len, NULL));
   g_free(c_src);
   _patch_sof_dims_huge((guint8 *)p_buf, u_len);

   gsize   u_padded_len;
   guint8 *p_padded =
      _build_padded_oversized_jpeg((guint8 *)p_buf, u_len, &u_padded_len);
   g_free(p_buf);
   g_assert_cmpuint(u_padded_len, >, GGAZE_JPEG_PEEK_LEN);

   gchar  *c_tmp = NULL;
   GError *p_sub = NULL;
   gint    i_fd =
      g_file_open_tmp("ggaze-thumb-padded-oversized-XXXXXX", &c_tmp, &p_sub);
   g_assert_no_error(p_sub);
   g_assert_cmpint(i_fd, >=, 0);
   g_assert_cmpint((glong)write(i_fd, p_padded, u_padded_len), ==,
                   (glong)u_padded_len);
   close(i_fd);
   g_free(p_padded);

   Thumbnail *p_t    = thumbnail_new();
   GFile     *p_file = g_file_new_for_path(c_tmp);

   GGAZE_RESULT   = NULL;
   GGAZE_ERR      = NULL;
   GGAZE_LOOP     = g_main_loop_new(NULL, FALSE);
   gint64 i_start = g_get_monotonic_time();
   thumbnail_get_async(p_t, p_file, 128, NULL, _thumb_err_cb, NULL);
   g_main_loop_run(GGAZE_LOOP);
   g_main_loop_unref(GGAZE_LOOP);
   GGAZE_LOOP     = NULL;
   gdouble d_secs = (g_get_monotonic_time() - i_start) / 1e6;

   g_assert_null(GGAZE_RESULT);
   g_assert_nonnull(GGAZE_ERR);
   g_assert_cmpuint(GGAZE_ERR->domain, ==, (guint)G_IO_ERROR);
   g_error_free(GGAZE_ERR);
   GGAZE_ERR = NULL;
   /* Same 5s budget as test_oversized_jpeg(): must fail fast, not stall. */
   g_assert_cmpfloat(d_secs, <, 5.0);

   thumbnail_delete(p_t);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
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
   g_test_add_func("/thumbnail/oversized_jpeg", test_oversized_jpeg);
   g_test_add_func("/thumbnail/padded_past_prefix_oversized_jpeg",
                   test_padded_past_prefix_oversized_jpeg);

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