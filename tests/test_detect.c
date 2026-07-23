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

#include <glib.h>

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
   return (g_test_run());
}