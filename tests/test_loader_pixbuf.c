/*:*
 * ggaze — GdkPixbuf loader backend unit test
 *
 * Loads committed fixtures via loader_load() and asserts the resulting
 * GdkTexture dimensions, including the rotated-EXIF case (decision #26): an
 * 8x4 JPEG with Orientation=6 must load as 4x8 after
 * gdk_pixbuf_apply_embedded_orientation. No display needed (texture creation
 * from a pixbuf is headless). Fixture dir comes from $GGAZE_FIXTURES_DIR
 * (set by meson). See ./sample-images for the optional realistic corpus.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "loader/loader.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <unistd.h>

static GdkTexture *
load_fixture(const gchar *c_name) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar      *c_path = g_build_filename(c_dir, c_name, NULL);
   GFile      *p_file = g_file_new_for_path(c_path);
   GError     *p_err  = NULL;
   GdkTexture *p_tex  = loader_load(p_file, NULL, &p_err);
   g_assert_no_error(p_err);
   g_object_unref(p_file);
   g_free(c_path);
   return (p_tex);
}

static void
test_plain_jpeg(void) {
   /* 6x3, Orientation = 1 -> 6x3 (no rotation applied). */
   GdkTexture *p_tex = load_fixture("plain.jpg");
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 6);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, 3);
   g_object_unref(p_tex);
}

static void
test_rotated_exif_jpeg(void) {
   /* 8x4, Orientation = 6 (rotate 90 CW) -> upright 4x8 (decision #26). */
   GdkTexture *p_tex = load_fixture("rot6.jpg");
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 4);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, 8);
   g_object_unref(p_tex);
}

static void
test_png(void) {
   /* 5x2 PNG, no orientation. */
   GdkTexture *p_tex = load_fixture("small.png");
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 5);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, 2);
   g_object_unref(p_tex);
}

static void
test_missing_file_errors(void) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar      *c_path = g_build_filename(c_dir, "does-not-exist.jpg", NULL);
   GFile      *p_file = g_file_new_for_path(c_path);
   GError     *p_err  = NULL;
   GdkTexture *p_tex  = loader_load(p_file, NULL, &p_err);
   g_assert_null(p_tex);
   g_assert_nonnull(p_err);
   g_error_free(p_err);
   g_object_unref(p_file);
   g_free(c_path);
}

/* Write raw bytes to a temp file and load them (magic-byte / corrupt cases). */
static GdkTexture *
load_bytes(const guint8 *p_buf, gsize u_len, GError **p_err) {
   gchar  *c_path = NULL;
   GError *p_sub  = NULL;
   gint    i_fd   = g_file_open_tmp("ggaze-XXXXXX", &c_path, &p_sub);
   g_assert_no_error(p_sub);
   g_assert_cmpint(i_fd, >=, 0);
   gsize u_off = 0;
   while (u_off < u_len) {
      gssize n = write(i_fd, p_buf + u_off, u_len - u_off);
      g_assert_cmpint(n, >, 0);
      u_off += (gsize)n;
   }
   close(i_fd);
   GFile      *p_file = g_file_new_for_path(c_path);
   GdkTexture *p_tex  = loader_load(p_file, NULL, p_err);
   g_object_unref(p_file);
   unlink(c_path);
   g_free(c_path);
   return (p_tex);
}

static void
assert_unsupported(const guint8 *p_buf, gsize u_len) {
   GError     *p_err = NULL;
   GdkTexture *p_tex = load_bytes(p_buf, u_len, &p_err);
   g_assert_null(p_tex);
   g_assert_nonnull(p_err);
   g_assert_cmpint(p_err->code, ==, G_IO_ERROR_NOT_SUPPORTED);
   g_error_free(p_err);
}

static void
test_unsupported_jxl(void) {
   const guint8 h[] = {0xFF, 0x0A, 0x10, 0x00}; /* JXL codestream magic */
   assert_unsupported(h, G_N_ELEMENTS(h));
}

static void
test_unsupported_avif(void) {
   const guint8 h[] = {0, 0, 0, 0, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f'};
   assert_unsupported(h, G_N_ELEMENTS(h));
}

static void
test_unsupported_heif(void) {
   const guint8 h[] = {0, 0, 0, 0, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c'};
   assert_unsupported(h, G_N_ELEMENTS(h));
}

static void
test_corrupt_jpeg(void) {
   /* Truncated JPEG: SOI + APP0 marker, no image data. GdkPixbuf fails. */
   const guint8 h[]   = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F',
                         'I',  'F',  0,    1,    1,    0,    0,   0};
   GError      *p_err = NULL;
   GdkTexture  *p_tex = load_bytes(h, G_N_ELEMENTS(h), &p_err);
   g_assert_null(p_tex);
   g_assert_nonnull(p_err);
   g_error_free(p_err);
}

static void
test_rgba_png(void) {
   /* 5x2 RGBA PNG -> exercises the has-alpha branch of texture_from_pixbuf. */
   GdkTexture *p_tex = load_fixture("rgba.png");
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 5);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, 2);
   g_object_unref(p_tex);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/loader/pixbuf/plain_jpeg", test_plain_jpeg);
   g_test_add_func("/loader/pixbuf/rotated_exif", test_rotated_exif_jpeg);
   g_test_add_func("/loader/pixbuf/png", test_png);
   g_test_add_func("/loader/pixbuf/missing_file", test_missing_file_errors);
   g_test_add_func("/loader/pixbuf/unsupported_jxl", test_unsupported_jxl);
   g_test_add_func("/loader/pixbuf/unsupported_avif", test_unsupported_avif);
   g_test_add_func("/loader/pixbuf/unsupported_heif", test_unsupported_heif);
   g_test_add_func("/loader/pixbuf/corrupt_jpeg", test_corrupt_jpeg);
   g_test_add_func("/loader/pixbuf/rgba_png", test_rgba_png);
   return (g_test_run());
}