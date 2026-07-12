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
   return (g_test_run());
}