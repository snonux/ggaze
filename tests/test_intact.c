/*:*
 * ggaze — PNG / JPEG completeness check unit test (every lane, no GEGL)
 *
 * Pins intact.h: the committed fixtures are complete, and the cut copies
 * that make GEGL's loaders spin (see intact.h) are refused as truncated --
 * a PNG missing its IEND, a PNG whose chunk length points past the end, a
 * JPEG cut inside its scan, a JPEG whose only EOI is an EXIF thumbnail's
 * ahead of SOS, a JPEG with no SOS at all -- while a JPEG with bytes
 * trailing its EOI (camera trailers) is complete. A missing file is its
 * GIO error, and a marker stream that is not one is INVALID_DATA.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "loader/intact.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static char *c_dir; /* temp folder for the hand-built files */

static GFile *
fixture(const char *c_name) {
   const char *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char  *c_path = g_build_filename(c_fx, c_name, NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);
   return (p_file);
}

static char *
fixture_bytes(const char *c_name, gsize *pu_len) {
   GFile *p_file = fixture(c_name);
   char  *c_data = NULL;
   g_assert_true(
      g_file_load_contents(p_file, NULL, &c_data, pu_len, NULL, NULL));
   g_object_unref(p_file);
   return (c_data);
}

/* Write u_len bytes as c_dir/c_name and return the GFile. */
static GFile *
temp_file(const char *c_name, const void *p_data, gsize u_len) {
   char *c_path = g_build_filename(c_dir, c_name, NULL);
   g_assert_true(g_file_set_contents(c_path, p_data, (gssize)u_len, NULL));
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);
   return (p_file);
}

static void
assert_truncated(gboolean b_png, GFile *p_file) {
   GError *p_err = NULL;
   g_assert_false(b_png ? intact_png(p_file, &p_err)
                        : intact_jpeg(p_file, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
   g_clear_error(&p_err);
   g_file_delete(p_file, NULL, NULL);
   g_object_unref(p_file);
}

static void
assert_intact(gboolean b_png, GFile *p_file) {
   GError *p_err = NULL;
   g_assert_true(b_png ? intact_png(p_file, &p_err)
                       : intact_jpeg(p_file, &p_err));
   g_assert_no_error(p_err);
   g_file_delete(p_file, NULL, NULL);
   g_object_unref(p_file);
}

static void
test_fixtures_are_intact(void) {
   const char *C_PNG[] = {"small.png", "rgba.png", "swapped.png", "badicc.png"};
   const char *C_JPG[] = {"plain.jpg", "rot6.jpg", "swapped.jpg"};
   for (gsize u = 0; u < G_N_ELEMENTS(C_PNG); u++) {
      GFile  *p_file = fixture(C_PNG[u]);
      GError *p_err  = NULL;
      g_assert_true(intact_png(p_file, &p_err));
      g_assert_no_error(p_err);
      g_object_unref(p_file);
   }
   for (gsize u = 0; u < G_N_ELEMENTS(C_JPG); u++) {
      GFile  *p_file = fixture(C_JPG[u]);
      GError *p_err  = NULL;
      g_assert_true(intact_jpeg(p_file, &p_err));
      g_assert_no_error(p_err);
      g_object_unref(p_file);
   }
}

static void
test_cut_png_is_truncated(void) {
   gsize u_len;
   char *c_png = fixture_bytes("swapped.png", &u_len);
   assert_truncated(TRUE, temp_file("cut.png", c_png, u_len - 40));
   assert_truncated(TRUE, temp_file("noiend.png", c_png, u_len - 12));
   assert_truncated(TRUE, temp_file("sig.png", c_png, 8));
   assert_truncated(TRUE, temp_file("empty.png", c_png, 0));
   /* A chunk whose declared length points past the end of the file. */
   guint8 *p_lie = g_memdup2(c_png, u_len);
   p_lie[33]     = 0x7F; /* the iCCP chunk's length, high byte */
   assert_truncated(TRUE, temp_file("lie.png", p_lie, u_len));
   g_free(p_lie);
   g_free(c_png);
}

static void
test_cut_jpeg_is_truncated(void) {
   gsize u_len;
   char *c_jpg = fixture_bytes("swapped.jpg", &u_len);
   assert_truncated(FALSE, temp_file("cut.jpg", c_jpg, u_len - 100));
   assert_truncated(FALSE, temp_file("noeoi.jpg", c_jpg, u_len - 2));
   assert_truncated(FALSE, temp_file("soi.jpg", c_jpg, 2));
   assert_truncated(FALSE, temp_file("empty.jpg", c_jpg, 0));
   /* Cut inside a header segment (the APP2 profile). */
   assert_truncated(FALSE, temp_file("hdr.jpg", c_jpg, 40));
   /* Trailing bytes after the EOI (a camera trailer) are fine. */
   GByteArray *p_trail = g_byte_array_new();
   g_byte_array_append(p_trail, (const guint8 *)c_jpg, (guint)u_len);
   g_byte_array_append(p_trail, (const guint8 *)"SEFT trailer", 12);
   assert_intact(FALSE, temp_file("trail.jpg", p_trail->data, p_trail->len));
   g_byte_array_unref(p_trail);
   g_free(c_jpg);
}

/* A hand-built marker stream: an APP1 carrying an EOI (as an EXIF
 * thumbnail would), then SOS and scan bytes with no EOI -- truncated; the
 * same with an EOI after the scan -- intact; EOI straight after the
 * headers (no image) -- truncated; a non-marker byte -- INVALID_DATA. */
static void
test_jpeg_eoi_must_follow_sos(void) {
   static const guint8 C_HEAD[] = {
      0xFF, 0xD8,                         /* SOI */
      0xFF, 0xE1, 0x00, 0x04, 0xFF, 0xD9, /* APP1 w/ EOI */
      0xFF, 0xD0,                         /* RST0 */
      0xFF, 0xFF, 0xDA, 0x00, 0x02,       /* fill + SOS */
      0x12, 0x34, 0xFF, 0x00, 0x56};
   GByteArray *p_a = g_byte_array_new();
   g_byte_array_append(p_a, C_HEAD, sizeof(C_HEAD));
   assert_truncated(FALSE, temp_file("thumb.jpg", p_a->data, p_a->len));
   g_byte_array_append(p_a, (const guint8 *)"\xFF\xD9", 2);
   assert_intact(FALSE, temp_file("ok.jpg", p_a->data, p_a->len));
   g_byte_array_unref(p_a);

   static const guint8 C_NOIMG[] = {0xFF, 0xD8, 0xFF, 0xD9};
   assert_truncated(FALSE, temp_file("noimg.jpg", C_NOIMG, sizeof(C_NOIMG)));

   static const guint8 C_JUNK[] = {0xFF, 0xD8, 0x12, 0x34};
   assert_truncated(FALSE, temp_file("junk.jpg", C_JUNK, sizeof(C_JUNK)));
   static const guint8 C_LEN[] = {0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x01};
   assert_truncated(FALSE, temp_file("len.jpg", C_LEN, sizeof(C_LEN)));
}

static void
test_missing_file_is_an_io_error(void) {
   GFile  *p_file = g_file_new_for_path("/nonexistent/ggaze/x.png");
   GError *p_err  = NULL;
   g_assert_false(intact_png(p_file, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
   g_clear_error(&p_err);
   g_assert_false(intact_jpeg(p_file, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
   g_clear_error(&p_err);
   g_object_unref(p_file);
}

int
main(int argc, char **argv) {
   g_test_init(&argc, &argv, NULL);
   c_dir = g_dir_make_tmp("ggaze-intact-XXXXXX", NULL);
   g_assert_nonnull(c_dir);
   g_test_add_func("/intact/fixtures_are_intact", test_fixtures_are_intact);
   g_test_add_func("/intact/cut_png_is_truncated", test_cut_png_is_truncated);
   g_test_add_func("/intact/cut_jpeg_is_truncated", test_cut_jpeg_is_truncated);
   g_test_add_func("/intact/jpeg_eoi_must_follow_sos",
                   test_jpeg_eoi_must_follow_sos);
   g_test_add_func("/intact/missing_file_is_an_io_error",
                   test_missing_file_is_an_io_error);
   int i_rc = g_test_run();
   g_rmdir(c_dir);
   g_free(c_dir);
   return (i_rc);
}
