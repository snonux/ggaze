/*:*
 * ggaze — format detection unit test
 *
 * Feeds magic-byte buffers to detect_format() and asserts the result. No I/O,
 * no display. Covers every format plus edge cases (empty, too-short, garbage).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "loader/detect.h"

#include <gio/gio.h>
#include <glib.h>
#include <unistd.h>

/* Locate the baseline SOF0 marker (0xFF 0xC0) in a JPEG byte buffer and
 * overwrite its declared height/width with 65500 (0xFFDC), mirroring
 * tests/test_loader_jpeg.c's _patch_sof_dims_huge (kept as a local copy
 * since this test binary does not link that one). */
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

static void
test_jpeg(void) {
   const guint8 h[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x10, 0x00};
   g_assert_cmpint(detect_format(h, G_N_ELEMENTS(h)), ==, GGAZE_FMT_JPEG);
}

static void
test_png(void) {
   const guint8 h[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0};
   g_assert_cmpint(detect_format(h, G_N_ELEMENTS(h)), ==, GGAZE_FMT_PNG);
}

static void
test_gif(void) {
   const guint8 h[] = {'G', 'I', 'F', '8', '9', 'a'};
   g_assert_cmpint(detect_format(h, G_N_ELEMENTS(h)), ==, GGAZE_FMT_GIF);
}

static void
test_webp(void) {
   const guint8 h[] = "RIFF\x00\x00\x00\x00WEBP";
   g_assert_cmpint(detect_format(h, 12), ==, GGAZE_FMT_WEBP);
}

static void
test_tiff_le(void) {
   const guint8 h[] = {'I', 'I', 0x2A, 0x00};
   g_assert_cmpint(detect_format(h, 4), ==, GGAZE_FMT_TIFF);
}

static void
test_tiff_be(void) {
   const guint8 h[] = {'M', 'M', 0x00, 0x2A};
   g_assert_cmpint(detect_format(h, 4), ==, GGAZE_FMT_TIFF);
}

static void
test_ico(void) {
   const guint8 h[] = {0x00, 0x00, 0x01, 0x00};
   g_assert_cmpint(detect_format(h, 4), ==, GGAZE_FMT_ICO);
}

static void
test_jxl_codestream(void) {
   const guint8 h[] = {0xFF, 0x0A, 0, 0};
   g_assert_cmpint(detect_format(h, 2), ==, GGAZE_FMT_JXL);
}

static void
test_jxl_container(void) {
   const guint8 h[] = {0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ', 0, 0, 0, 0};
   g_assert_cmpint(detect_format(h, 12), ==, GGAZE_FMT_JXL);
}

static void
test_avif(void) {
   const guint8 h[] = {0, 0, 0, 0, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f'};
   g_assert_cmpint(detect_format(h, 12), ==, GGAZE_FMT_AVIF);
}

static void
test_heif(void) {
   const guint8 h[] = {0, 0, 0, 0, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c'};
   g_assert_cmpint(detect_format(h, 12), ==, GGAZE_FMT_HEIF);
}

static void
test_unknown_garbage(void) {
   const guint8 h[] = {'h', 'e', 'l', 'l', 'o'};
   g_assert_cmpint(detect_format(h, G_N_ELEMENTS(h)), ==, GGAZE_FMT_UNKNOWN);
}

static void
test_empty_and_short(void) {
   g_assert_cmpint(detect_format(NULL, 0), ==, GGAZE_FMT_UNKNOWN);
   const guint8 h[] = {0xFF};
   g_assert_cmpint(detect_format(h, 1), ==, GGAZE_FMT_UNKNOWN);
}

/* detect_jpeg_peek_dims(): real-file coverage using the committed fixtures,
 * plus the crafted-oversized-header case that motivated adding it (mu0
 * review: GdkPixbuf stalls ~28s on this exact input instead of failing
 * fast, so backends must reject it before decoding). */
static void
test_jpeg_peek_dims_plain(void) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   guint8 *p_buf  = NULL;
   gsize   u_len  = 0;
   g_assert_true(g_file_get_contents(c_path, (gchar **)&p_buf, &u_len, NULL));
   g_free(c_path);

   guint32 u_w = 0, u_h = 0;
   g_assert_true(detect_jpeg_peek_dims(p_buf, u_len, &u_w, &u_h));
   g_assert_cmpuint(u_w, ==, 6);
   g_assert_cmpuint(u_h, ==, 3);
   g_free(p_buf);
}

static void
test_jpeg_peek_dims_rotated_uses_raw_dims(void) {
   /* rot6.jpg is 8x4 as declared by its SOF0 (EXIF Orientation=6 rotates it
    * to 4x8 only once a decoder applies the tag); the header-only peek must
    * report the raw, undecoded 8x4. */
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar  *c_path = g_build_filename(c_dir, "rot6.jpg", NULL);
   guint8 *p_buf  = NULL;
   gsize   u_len  = 0;
   g_assert_true(g_file_get_contents(c_path, (gchar **)&p_buf, &u_len, NULL));
   g_free(c_path);

   guint32 u_w = 0, u_h = 0;
   g_assert_true(detect_jpeg_peek_dims(p_buf, u_len, &u_w, &u_h));
   g_assert_cmpuint(u_w, ==, 8);
   g_assert_cmpuint(u_h, ==, 4);
   g_free(p_buf);
}

static void
test_jpeg_peek_dims_oversized(void) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   guint8 *p_buf  = NULL;
   gsize   u_len  = 0;
   g_assert_true(g_file_get_contents(c_path, (gchar **)&p_buf, &u_len, NULL));
   g_free(c_path);

   _patch_sof_dims_huge(p_buf, u_len);
   guint32 u_w = 0, u_h = 0;
   g_assert_true(detect_jpeg_peek_dims(p_buf, u_len, &u_w, &u_h));
   g_assert_cmpuint(u_w, ==, 65500);
   g_assert_cmpuint(u_h, ==, 65500);
   g_assert_cmpuint((guint64)u_w * u_h, >, GGAZE_JPEG_MAX_PIXELS);
   g_free(p_buf);
}

static void
test_jpeg_peek_dims_not_jpeg(void) {
   const guint8 h[] = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};
   guint32      u_w = 0xdeadbeef, u_h = 0xdeadbeef;
   g_assert_false(detect_jpeg_peek_dims(h, G_N_ELEMENTS(h), &u_w, &u_h));
}

static void
test_jpeg_peek_dims_truncated(void) {
   /* SOI + start of an APP0 marker, no SOF, no scan data. */
   const guint8 h[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F'};
   guint32      u_w = 0, u_h = 0;
   g_assert_false(detect_jpeg_peek_dims(h, G_N_ELEMENTS(h), &u_w, &u_h));
}

static void
test_jpeg_peek_dims_null_and_empty(void) {
   guint32 u_w = 0, u_h = 0;
   g_assert_false(detect_jpeg_peek_dims(NULL, 0, &u_w, &u_h));
   const guint8 h[] = {0xFF, 0xD8};
   g_assert_false(detect_jpeg_peek_dims(h, G_N_ELEMENTS(h), &u_w, &u_h));
}

/* Write p_data (u_len bytes) to a unique temp file; caller frees the
 * returned path with g_free and removes the file with unlink. Mirrors the
 * _write_tmp helper in tests/test_loader_jpeg.c. */
static gchar *
_write_tmp(const guint8 *p_data, gsize u_len) {
   gchar  *c_tmp = NULL;
   GError *p_sub = NULL;
   gint    i_fd  = g_file_open_tmp("ggaze-detect-XXXXXX", &c_tmp, &p_sub);
   g_assert_no_error(p_sub);
   g_assert_cmpint(i_fd, >=, 0);
   g_assert_cmpint((glong)write(i_fd, p_data, u_len), ==, (glong)u_len);
   close(i_fd);
   return (c_tmp);
}

/* detect_jpeg_peek_dims_from_path() (mu0 review round 2): the path-based
 * twin used by call sites (thumbnail.c, info.c) that hand a path straight to
 * a decoder rather than loading bytes themselves first. */
static void
test_jpeg_peek_dims_from_path_plain(void) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar *c_path = g_build_filename(c_dir, "plain.jpg", NULL);

   guint32 u_w = 0, u_h = 0;
   g_assert_cmpint(detect_jpeg_peek_dims_from_path(c_path, &u_w, &u_h), ==,
                   GGAZE_JPEG_PEEK_OK);
   g_assert_cmpuint(u_w, ==, 6);
   g_assert_cmpuint(u_h, ==, 3);
   g_free(c_path);
}

static void
test_jpeg_peek_dims_from_path_oversized(void) {
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

   guint32 u_w = 0, u_h = 0;
   g_assert_cmpint(detect_jpeg_peek_dims_from_path(c_tmp, &u_w, &u_h), ==,
                   GGAZE_JPEG_PEEK_OK);
   g_assert_cmpuint(u_w, ==, 65500);
   g_assert_cmpuint(u_h, ==, 65500);
   g_assert_false(detect_jpeg_dims_within_bounds(u_w, u_h, NULL));

   unlink(c_tmp);
   g_free(c_tmp);
}

static void
test_jpeg_peek_dims_from_path_missing(void) {
   guint32 u_w = 0xdeadbeef, u_h = 0xdeadbeef;
   g_assert_cmpint(
      detect_jpeg_peek_dims_from_path("/nonexistent/ggaze-mu0.jpg", &u_w, &u_h),
      ==, GGAZE_JPEG_PEEK_NOT_JPEG);
}

/* mu0 review round 3: the CRITICAL bypass. A JPEG marker segment can legally
 * declare a length up to 65533 bytes; prefixing the real (huge-patched) SOF0
 * with one maximal-length filler APP0 segment pushes the SOF past
 * GGAZE_JPEG_PEEK_LEN (64KB). The bounded scan must recognize it ran out of
 * prefix mid-header and report GGAZE_JPEG_PEEK_INCONCLUSIVE, not silently
 * come back as "no JPEG here" -- the latter is what let thumbnail.c/info.c
 * proceed straight to the unguarded ~28-30s GdkPixbuf stall. */
static void
test_jpeg_peek_dims_from_path_padded_past_prefix_inconclusive(void) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   guint8 *p_buf  = NULL;
   gsize   u_len  = 0;
   g_assert_true(g_file_get_contents(c_path, (gchar **)&p_buf, &u_len, NULL));
   g_free(c_path);
   _patch_sof_dims_huge(p_buf, u_len);

   /* Build SOI + one maximal-length APP0 filler segment (0xFFFD = 65533,
    * including the 2-byte length field itself) + the rest of the original
    * (SOI-prefixed, now huge-SOF0) file appended after it, minus its own
    * leading SOI so the stream stays well-formed. Total filler pushes the
    * original SOF far past GGAZE_JPEG_PEEK_LEN (64KB). */
   const guint16 u_fill_seglen = 0xFFFD;
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
   /* Skip the original file's own 2-byte SOI before appending. */
   g_byte_array_append(p_out, p_buf + 2, (guint)(u_len - 2));
   g_free(p_buf);

   g_assert_cmpuint(p_out->len, >, GGAZE_JPEG_PEEK_LEN);
   gchar *c_tmp = _write_tmp(p_out->data, p_out->len);
   g_byte_array_unref(p_out);

   guint32 u_w = 0xdeadbeef, u_h = 0xdeadbeef;
   g_assert_cmpint(detect_jpeg_peek_dims_from_path(c_tmp, &u_w, &u_h), ==,
                   GGAZE_JPEG_PEEK_INCONCLUSIVE);

   unlink(c_tmp);
   g_free(c_tmp);
}

/* detect_jpeg_dims_within_bounds() (mu0 review round 2): the bound
 * comparison + GError shared by every call site. */
static void
test_jpeg_dims_within_bounds(void) {
   GError *p_err = NULL;
   g_assert_true(detect_jpeg_dims_within_bounds(6, 3, &p_err));
   g_assert_no_error(p_err);
}

static void
test_jpeg_dims_within_bounds_oversized_sets_error(void) {
   GError *p_err = NULL;
   g_assert_false(detect_jpeg_dims_within_bounds(65500, 65500, &p_err));
   g_assert_nonnull(p_err);
   g_assert_cmpuint(p_err->domain, ==, (guint)G_IO_ERROR);
   g_error_free(p_err);
}

static void
test_jpeg_dims_within_bounds_null_err_ok(void) {
   /* Callers that don't propagate a GError (info.c) may pass NULL. */
   g_assert_false(detect_jpeg_dims_within_bounds(65500, 65500, NULL));
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/detect/jpeg", test_jpeg);
   g_test_add_func("/detect/png", test_png);
   g_test_add_func("/detect/gif", test_gif);
   g_test_add_func("/detect/webp", test_webp);
   g_test_add_func("/detect/tiff_le", test_tiff_le);
   g_test_add_func("/detect/tiff_be", test_tiff_be);
   g_test_add_func("/detect/ico", test_ico);
   g_test_add_func("/detect/jxl_codestream", test_jxl_codestream);
   g_test_add_func("/detect/jxl_container", test_jxl_container);
   g_test_add_func("/detect/avif", test_avif);
   g_test_add_func("/detect/heif", test_heif);
   g_test_add_func("/detect/unknown_garbage", test_unknown_garbage);
   g_test_add_func("/detect/empty_and_short", test_empty_and_short);
   g_test_add_func("/detect/jpeg_peek_dims/plain", test_jpeg_peek_dims_plain);
   g_test_add_func("/detect/jpeg_peek_dims/rotated_uses_raw_dims",
                   test_jpeg_peek_dims_rotated_uses_raw_dims);
   g_test_add_func("/detect/jpeg_peek_dims/oversized",
                   test_jpeg_peek_dims_oversized);
   g_test_add_func("/detect/jpeg_peek_dims/not_jpeg",
                   test_jpeg_peek_dims_not_jpeg);
   g_test_add_func("/detect/jpeg_peek_dims/truncated",
                   test_jpeg_peek_dims_truncated);
   g_test_add_func("/detect/jpeg_peek_dims/null_and_empty",
                   test_jpeg_peek_dims_null_and_empty);
   g_test_add_func("/detect/jpeg_peek_dims_from_path/plain",
                   test_jpeg_peek_dims_from_path_plain);
   g_test_add_func("/detect/jpeg_peek_dims_from_path/oversized",
                   test_jpeg_peek_dims_from_path_oversized);
   g_test_add_func("/detect/jpeg_peek_dims_from_path/missing",
                   test_jpeg_peek_dims_from_path_missing);
   g_test_add_func(
      "/detect/jpeg_peek_dims_from_path/padded_past_prefix_inconclusive",
      test_jpeg_peek_dims_from_path_padded_past_prefix_inconclusive);
   g_test_add_func("/detect/jpeg_dims_within_bounds/ok",
                   test_jpeg_dims_within_bounds);
   g_test_add_func("/detect/jpeg_dims_within_bounds/oversized_sets_error",
                   test_jpeg_dims_within_bounds_oversized_sets_error);
   g_test_add_func("/detect/jpeg_dims_within_bounds/null_err_ok",
                   test_jpeg_dims_within_bounds_null_err_ok);
   return (g_test_run());
}