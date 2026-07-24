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
#include <unistd.h>

#include "loader/detect.h"

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

/* mu0 review round 2: info_new() (via _fill_dims()) called
 * gdk_pixbuf_new_from_file() directly on the source file, bypassing
 * loader.c's guard entirely -- and info_new() runs synchronously on the GTK
 * main thread (window.c's _show_info, the info keybinding), so an unguarded
 * stall there freezes the whole UI. Exercises the REAL public entry point
 * (info_new()) with the crafted 65500x65500-SOF0 header: asserts a non-NULL
 * GgazeInfo with dimensions left at their zero default (the same fallback
 * info_new() already used for any other GdkPixbuf failure) plus a tight
 * wall-clock budget -- pre-fix, this call stalled ~28s (GdkPixbuf/glycin
 * pre-allocating a huge sparse memfd off the declared size before its own
 * internal cap rejected it). */
static void
test_oversized_jpeg(void) {
   const char *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   char  *c_src = g_build_filename(c_dir, "plain.jpg", NULL);
   gchar *p_buf = NULL;
   gsize  u_len = 0;
   g_assert_true(g_file_get_contents(c_src, &p_buf, &u_len, NULL));
   g_free(c_src);
   _patch_sof_dims_huge((guint8 *)p_buf, u_len);

   gchar  *c_tmp = NULL;
   GError *p_sub = NULL;
   gint i_fd = g_file_open_tmp("ggaze-info-oversized-XXXXXX", &c_tmp, &p_sub);
   g_assert_no_error(p_sub);
   g_assert_cmpint(i_fd, >=, 0);
   g_assert_cmpint((glong)write(i_fd, p_buf, u_len), ==, (glong)u_len);
   close(i_fd);
   g_free(p_buf);

   GFile *p_file = g_file_new_for_path(c_tmp);

   gint64     i_start = g_get_monotonic_time();
   GgazeInfo *p_info  = info_new(p_file);
   gdouble    d_secs  = (g_get_monotonic_time() - i_start) / 1e6;

   g_assert_nonnull(p_info);
   g_assert_cmpint(p_info->i_width, ==, 0);
   g_assert_cmpint(p_info->i_height, ==, 0);
   /* 5s leaves generous headroom above the microsecond-scale header peek
    * while catching a regression back to the ~28s stall long before any CI
    * per-test timeout would. */
   g_assert_cmpfloat(d_secs, <, 5.0);

   info_delete(p_info);
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
 * scan used to come back as plain "no SOF found" and _fill_dims() treated
 * that the same as "not a JPEG, safe to proceed" -- reintroducing the exact
 * ~28-30s GdkPixbuf stall this task exists to close, this time freezing the
 * whole UI (info_new() runs on the GTK main thread). Exercises the REAL
 * public entry point (info_new()) and asserts the fixed fail-closed
 * behavior: dimensions stay at their zero default and the call returns fast,
 * not after a multi-second stall. */
static void
test_padded_past_prefix_oversized_jpeg(void) {
   const char *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   char  *c_src = g_build_filename(c_dir, "plain.jpg", NULL);
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
      g_file_open_tmp("ggaze-info-padded-oversized-XXXXXX", &c_tmp, &p_sub);
   g_assert_no_error(p_sub);
   g_assert_cmpint(i_fd, >=, 0);
   g_assert_cmpint((glong)write(i_fd, p_padded, u_padded_len), ==,
                   (glong)u_padded_len);
   close(i_fd);
   g_free(p_padded);

   GFile *p_file = g_file_new_for_path(c_tmp);

   gint64     i_start = g_get_monotonic_time();
   GgazeInfo *p_info  = info_new(p_file);
   gdouble    d_secs  = (g_get_monotonic_time() - i_start) / 1e6;

   g_assert_nonnull(p_info);
   g_assert_cmpint(p_info->i_width, ==, 0);
   g_assert_cmpint(p_info->i_height, ==, 0);
   /* Same 5s budget as test_oversized_jpeg(): must fail fast, not stall. */
   g_assert_cmpfloat(d_secs, <, 5.0);

   info_delete(p_info);
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
   g_test_add_func("/info/plain_jpeg", test_plain_jpeg);
   g_test_add_func("/info/rotated_jpeg", test_rotated_jpeg);
   g_test_add_func("/info/png_no_exif", test_png_no_exif);
   g_test_add_func("/info/oversized_jpeg", test_oversized_jpeg);
   g_test_add_func("/info/padded_past_prefix_oversized_jpeg",
                   test_padded_past_prefix_oversized_jpeg);
   return (g_test_run());
}