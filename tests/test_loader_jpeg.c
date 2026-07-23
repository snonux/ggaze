/*:*
 * ggaze — libjpeg-turbo progressive loader unit test (feature-gated)
 *
 * Tests the progressive JPEG path: loader_load_async with a progress callback
 * fires the callback (low-res partial) and returns the full texture. Only
 * compiled when libjpeg-turbo is found (HAVE_JPEG).
 *
 * mu0 review coverage: an initial fix bounded jpeg.c's own libjpeg-driven
 * _decode_at_scale() allocations (test_jpeg_oversized below), but a fresh-
 * context review found that guard unreachable via any real dispatch path --
 * the actual full decode users see when browsing (window.c's
 * _load_current(), which always supplies a progress callback) resolves via
 * _jpeg_load_progressive()'s phase 2, a direct gdk_pixbuf_new_from_file()
 * call that never went through _decode_at_scale() at all. Empirically,
 * GdkPixbuf's JPEG loader (glycin, gdk-pixbuf 2.44) does not crash on a
 * maximum-dimension declared header, but it does stall ~28s (pre-allocating
 * a huge sparse memfd off the declared size before its own internal cap
 * rejects it) -- a real, reachable unbounded-latency DoS. jpeg.c now peeks
 * the declared dimensions (detect_jpeg_peek_dims(), no decoder invoked)
 * before either phase runs; test_progressive_oversized below exercises that
 * fix through loader_load_async(), the actual public API window.c calls.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "loader/loader.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <unistd.h>

static volatile int g_progress_fired = 0;
static GMainLoop   *g_loop           = NULL;
static GdkTexture  *g_result         = NULL;
static GError      *g_last_err       = NULL;

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

/* Like finish_cb, but for tests that expect the load to fail: stores the
 * GError in g_last_err (caller frees) instead of asserting success. */
static void
finish_err_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   (void)p_data;
   g_result = loader_load_finish(p_res, &g_last_err);
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

/* Locate the baseline SOF0 marker (0xFF 0xC0) in a JPEG byte buffer and
 * overwrite its declared height/width with 65500 (0xFFDC), the largest value
 * libjpeg's own JPEG_MAX_DIMENSION check still accepts at header-read time.
 * The quantization tables, component data, and scan (entropy-coded) data are
 * left untouched, so the result is a structurally valid JPEG that merely
 * lies about its size — exactly what a malicious "maximum-dimension header"
 * looks like: two rewritten bytes, no huge pixel payload required. Aborts
 * the test via g_assert if no SOF0 marker is found (so a future fixture
 * regenerated as progressive/SOF2 fails loudly instead of silently
 * no-op'ing this test). */
static void
_patch_sof_dims_huge(guint8 *p_buf, gsize u_len) {
   for (gsize u = 0; u + 8 < u_len; u++) {
      if (p_buf[u] == 0xff && p_buf[u + 1] == 0xc0) {
         /* marker(2) length(2) precision(1) height(2) width(2) ... */
         p_buf[u + 5] = 0xff;
         p_buf[u + 6] = 0xdc;
         p_buf[u + 7] = 0xff;
         p_buf[u + 8] = 0xdc;
         return;
      }
   }
   g_assert_not_reached();
}

/* Write p_len bytes to a unique temp file and return its path (caller frees
 * with g_free and removes with unlink). Mirrors the helper used by
 * test_loader_jxl.c for its crafted-header test. */
static gchar *
_write_tmp(const guint8 *p_data, gsize u_len) {
   gchar  *c_tmp = NULL;
   GError *p_sub = NULL;
   gint    i_fd  = g_file_open_tmp("ggaze-jpeg-XXXXXX", &c_tmp, &p_sub);
   g_assert_no_error(p_sub);
   g_assert_cmpint(i_fd, >=, 0);
   g_assert_cmpint((glong)write(i_fd, p_data, u_len), ==, (glong)u_len);
   close(i_fd);
   return (c_tmp);
}

/* jpeg_backend is registered with the loader dispatcher only via the special-
 * cased progressive path in loader.c (_load_task_thread's HAVE_JPEG branch);
 * the synchronous BACKENDS[] list omits it, so plain loader_load() routes
 * JPEGs to the GdkPixbuf fallback instead, and loader_load_async()'s overall
 * result is decided by _jpeg_load_progressive()'s phase-2 GdkPixbuf call, not
 * by the phase-1 _decode_at_scale() call this test targets. Call the backend
 * directly (as loader.c itself does) to exercise _decode_at_scale()'s "full"
 * (scale_denom=1) path deterministically, matching the extern-declare pattern
 * loader.c already uses for the same struct. */
extern const GgazeLoaderBackend jpeg_backend;

static void
test_jpeg_oversized(void) {
   /* Start from the real plain.jpg fixture (valid quant/huffman tables and
    * scan data) and only lie about the declared SOF0 dimensions, so libjpeg
    * itself accepts the header at jpeg_read_header()/jpeg_start_decompress()
    * time — the new GGAZE_JPEG_MAX_SIDE/GGAZE_JPEG_MAX_PIXELS bound in
    * jpeg.c is what must reject it, not libjpeg's own 65500-per-side cap.
    * Pre-fix, this same input drove _decode_at_scale() straight into
    * g_malloc((gsize)65500 * (gsize)196500) (~12.9 GB) with no bound check;
    * on any machine without that much free (or overcommittable) memory,
    * GLib's g_malloc() aborts the process. */
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   guint8 *p_buf  = NULL;
   gsize   u_len  = 0;
   g_assert_true(g_file_get_contents(c_path, (gchar **)&p_buf, &u_len, NULL));
   g_free(c_path);

   _patch_sof_dims_huge(p_buf, u_len);
   gchar *c_tmp = _write_tmp(p_buf, u_len);
   g_free(p_buf);

   GFile      *p_file = g_file_new_for_path(c_tmp);
   GError     *p_err  = NULL;
   GdkTexture *p_tex  = jpeg_backend.load(p_file, NULL, &p_err);
   g_assert_null(p_tex);
   g_assert_nonnull(p_err);
   g_assert_cmpuint(p_err->domain, ==, (guint)G_IO_ERROR);
   g_error_free(p_err);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
}

/* Real-dispatch coverage (mu0 review): the same crafted 65500x65500 header,
 * but driven through loader_load_async() -- the public API window.c's
 * _load_current() calls with a progress callback, which resolves via
 * jpeg_backend.load_progressive()'s phase 2 (gdk_pixbuf_new_from_file()),
 * not the jpeg_backend.load() call test_jpeg_oversized exercises directly.
 * Asserts both a clean G_IO_ERROR and a tight wall-clock budget: pre-fix,
 * this call observed a ~28s stall (GdkPixbuf/glycin pre-allocating off the
 * declared size before its own internal cap rejected it) rather than a
 * crash, so only a timing assertion actually catches a regression here. */
static void
test_progressive_oversized(void) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   guint8 *p_buf  = NULL;
   gsize   u_len  = 0;
   g_assert_true(g_file_get_contents(c_path, (gchar **)&p_buf, &u_len, NULL));
   g_free(c_path);

   _patch_sof_dims_huge(p_buf, u_len);
   gchar *c_tmp = _write_tmp(p_buf, u_len);
   g_free(p_buf);

   GFile *p_file    = g_file_new_for_path(c_tmp);
   g_progress_fired = 0;
   g_result         = NULL;
   g_last_err       = NULL;
   g_loop           = g_main_loop_new(NULL, FALSE);
   gint64 i_start   = g_get_monotonic_time();
   loader_load_async(p_file, NULL, progress_cb, NULL, finish_err_cb, NULL);
   g_main_loop_run(g_loop);
   g_main_loop_unref(g_loop);
   g_loop         = NULL;
   gdouble d_secs = (g_get_monotonic_time() - i_start) / 1e6;

   g_assert_null(g_result);
   g_assert_nonnull(g_last_err);
   g_assert_cmpuint(g_last_err->domain, ==, (guint)G_IO_ERROR);
   g_error_free(g_last_err);
   g_last_err = NULL;
   /* No valid SOF-bounded dimensions means _decode_at_scale() never gets far
    * enough to report a low-res partial either, so progress_cb should not
    * have fired -- both phases are rejected by the same up-front peek. */
   g_assert_cmpint(g_atomic_int_get(&g_progress_fired), ==, 0);
   /* 5s leaves generous headroom above the microsecond-scale header peek
    * while catching a regression back to the ~28s stall long before any CI
    * per-test timeout would. */
   g_assert_cmpfloat(d_secs, <, 5.0);

   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
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
   g_test_add_func("/loader/jpeg/oversized", test_jpeg_oversized);
   g_test_add_func("/loader/jpeg/progressive_oversized",
                   test_progressive_oversized);
   return (g_test_run());
}