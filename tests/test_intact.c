/*:*
 * ggaze — PNG / JPEG completeness check unit test (every lane, no GEGL)
 *
 * Pins intact.h: the committed fixtures are complete, and the cut copies
 * that make GEGL's loaders spin (see intact.h) are refused as truncated --
 * a PNG missing its IEND, a PNG whose chunk length points past the end, a
 * JPEG cut inside its scan, a JPEG whose only EOI is an EXIF thumbnail's
 * ahead of SOS, a JPEG with no SOS at all -- while a JPEG with bytes
 * trailing its EOI (camera trailers) is complete. A missing file is its
 * GIO error, and a marker stream that is not one is INVALID_DATA. Then
 * what libpng / libjpeg would give up on (xb2 review): a PNG's critical
 * chunk CRCs, its image data inflated row by row (Adam7 included, sizes
 * cross-checked against real interlaced files), a JPEG decoded once by
 * libjpeg -- and the stored sizes both walks report, the JPEG's also with
 * its SOF past 64 KiB and with padding between segments.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "loader/intact.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "ggaze-config.h"

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
   g_assert_false(b_png ? intact_png(p_file, NULL, &p_err)
                        : intact_jpeg(p_file, NULL, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
   g_clear_error(&p_err);
   g_file_delete(p_file, NULL, NULL);
   g_object_unref(p_file);
}

static void
assert_intact(gboolean b_png, GFile *p_file) {
   GError *p_err = NULL;
   g_assert_true(b_png ? intact_png(p_file, NULL, &p_err)
                       : intact_jpeg(p_file, NULL, &p_err));
   g_assert_no_error(p_err);
   g_file_delete(p_file, NULL, NULL);
   g_object_unref(p_file);
}

static void
test_fixtures_are_intact(void) {
   const char *C_PNG[] = {"small.png",  "rgba.png",     "swapped.png",
                          "badicc.png", "srgb-icc.png", "grey-icc.png"};
   const char *C_JPG[] = {"plain.jpg",    "rot6.jpg",         "swapped.jpg",
                          "srgb-icc.jpg", "swapped-rot6.jpg", "grey-icc.jpg",
                          "cmyk-icc.jpg"};
   for (gsize u = 0; u < G_N_ELEMENTS(C_PNG); u++) {
      GFile  *p_file = fixture(C_PNG[u]);
      GError *p_err  = NULL;
      g_assert_true(intact_png(p_file, NULL, &p_err));
      g_assert_no_error(p_err);
      g_object_unref(p_file);
   }
   for (gsize u = 0; u < G_N_ELEMENTS(C_JPG); u++) {
      GFile  *p_file = fixture(C_JPG[u]);
      GError *p_err  = NULL;
      g_assert_true(intact_jpeg(p_file, NULL, &p_err));
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
   g_assert_false(intact_png(p_file, NULL, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
   g_clear_error(&p_err);
   g_assert_false(intact_jpeg(p_file, NULL, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
   g_clear_error(&p_err);
   g_object_unref(p_file);
}

/* --- sizes and padding ---------------------------------------------------- */

static void
assert_size(gboolean b_png, GFile *p_file, guint32 u_w, guint32 u_h) {
   IntactSize t_size = {0, 0};
   GError    *p_err  = NULL;
   g_assert_true(b_png ? intact_png(p_file, &t_size, &p_err)
                       : intact_jpeg(p_file, &t_size, &p_err));
   g_assert_no_error(p_err);
   g_assert_cmpuint(t_size.u_w, ==, u_w);
   g_assert_cmpuint(t_size.u_h, ==, u_h);
}

/* Insert u_len bytes of p_data into p_a at u_at. */
static void
bytes_insert(GByteArray *p_a, gsize u_at, const guint8 *p_data, gsize u_len) {
   gsize u_old = p_a->len;
   g_byte_array_set_size(p_a, (guint)(u_old + u_len));
   memmove(p_a->data + u_at + u_len, p_a->data + u_at, u_old - u_at);
   memcpy(p_a->data + u_at, p_data, u_len);
}

static GByteArray *
fixture_array(const char *c_name) {
   gsize u_len = 0;
   char *c_raw = fixture_bytes(c_name, &u_len);
   return (g_byte_array_new_take((guint8 *)c_raw, u_len));
}

/* The stored (not oriented) sizes; a JPEG's SOF read past two 64 KiB
 * APP15 segments, and past stray bytes between segments. */
static void
test_sizes_are_reported(void) {
   GFile *p_file = fixture("swapped.png");
   assert_size(TRUE, p_file, 6, 3);
   g_object_unref(p_file);
   p_file = fixture("rot6.jpg");
   assert_size(FALSE, p_file, 8, 4);
   g_object_unref(p_file);

   GByteArray *p_far = fixture_array("swapped.jpg");
   guint8     *p_pad = g_malloc0(65537);
   memcpy(p_pad, "\xFF\xEF\xFF\xFF", 4); /* APP15, the longest segment */
   bytes_insert(p_far, 2, p_pad, 65537);
   bytes_insert(p_far, 2, p_pad, 65537);
   g_free(p_pad);
   p_file = temp_file("far.jpg", p_far->data, p_far->len);
   assert_size(FALSE, p_file, 8, 8);
   g_file_delete(p_file, NULL, NULL);
   g_object_unref(p_file);
   bytes_insert(p_far, 2, (const guint8 *)"\x00\x11\xFF\x00\x22", 5);
   assert_intact(FALSE, temp_file("pad.jpg", p_far->data, p_far->len));
   g_byte_array_unref(p_far);
}

/* --- PNG image data ------------------------------------------------------- */

static guint32
png_crc(const guint8 *p, gsize u_len) {
   guint32 u_crc = 0xFFFFFFFFu;
   for (gsize u = 0; u < u_len; u++) {
      u_crc ^= p[u];
      for (int i = 0; i < 8; i++) {
         u_crc = (u_crc & 1) ? 0xEDB88320u ^ (u_crc >> 1) : u_crc >> 1;
      }
   }
   return (u_crc ^ 0xFFFFFFFFu);
}

static void
png_chunk(GByteArray *p_a, const char *c_type, const guint8 *p_data,
          gsize u_len) {
   guint32 u_be = GUINT32_TO_BE((guint32)u_len);
   g_byte_array_append(p_a, (const guint8 *)&u_be, 4);
   gsize u_at = p_a->len;
   g_byte_array_append(p_a, (const guint8 *)c_type, 4);
   g_byte_array_append(p_a, p_data, (guint)u_len);
   u_be = GUINT32_TO_BE(png_crc(p_a->data + u_at, u_len + 4));
   g_byte_array_append(p_a, (const guint8 *)&u_be, 4);
}

static GBytes *
zlib_compress(const guint8 *p_raw, gsize u_len) {
   GConverter *p_z =
      G_CONVERTER(g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB, -1));
   guint8 c_out[4096];
   gsize  u_read = 0, u_wrote = 0;
   g_assert_cmpint(g_converter_convert(p_z, p_raw, u_len, c_out, sizeof(c_out),
                                       G_CONVERTER_INPUT_AT_END, &u_read,
                                       &u_wrote, NULL),
                   ==, G_CONVERTER_FINISHED);
   g_object_unref(p_z);
   return (g_bytes_new(c_out, u_wrote));
}

/* A PNG of the given IHDR whose image data is u_raw bytes of p_raw (the
 * filter bytes included), split over u_split IDAT chunks. */
static GFile *
png_file(guint32 u_w, guint32 u_h, guint8 u_type, guint8 u_depth,
         guint8 u_interlace, const guint8 *p_raw, gsize u_raw, guint u_split) {
   GByteArray *p_a = g_byte_array_new();
   g_byte_array_append(p_a, (const guint8 *)"\x89PNG\r\n\x1a\n", 8);
   guint8  c_ihdr[13] = {0};
   guint32 u_be_w     = GUINT32_TO_BE(u_w);
   guint32 u_be_h     = GUINT32_TO_BE(u_h);
   memcpy(c_ihdr, &u_be_w, 4);
   memcpy(c_ihdr + 4, &u_be_h, 4);
   c_ihdr[8]  = u_depth;
   c_ihdr[9]  = u_type;
   c_ihdr[12] = u_interlace;
   png_chunk(p_a, "IHDR", c_ihdr, 13);
   GBytes       *p_z   = zlib_compress(p_raw, u_raw);
   gsize         u_len = 0;
   const guint8 *p_zd  = g_bytes_get_data(p_z, &u_len);
   gsize         u_cut = u_len / u_split;
   for (guint u = 0; u < u_split; u++) {
      gsize u_n = u + 1 == u_split ? u_len - u * u_cut : u_cut;
      png_chunk(p_a, "IDAT", p_zd + u * u_cut, u_n);
   }
   png_chunk(p_a, "IEND", NULL, 0);
   GFile *p_file = temp_file("built.png", p_a->data, p_a->len);
   g_bytes_unref(p_z);
   g_byte_array_unref(p_a);
   return (p_file);
}

static void
assert_png_corrupt(GFile *p_file) {
   GError *p_err = NULL;
   g_assert_false(intact_png(p_file, NULL, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
   g_assert_nonnull(strstr(p_err->message, "corrupt"));
   g_clear_error(&p_err);
   g_file_delete(p_file, NULL, NULL);
   g_object_unref(p_file);
}

/* The rows IHDR promises: 3x3 and 13x7 grey8 Adam7 hold 15 and 105 bytes
 * (the sizes real interlaced files of those sizes inflate to), a 10x2
 * 1-bit palette image 2 x (1 + 2); one byte short is corrupt, a filter
 * type over 4 is corrupt, data over the rows is fine (libpng warns), and
 * the data may span IDAT chunks. */
static void
test_png_rows_are_checked(void) {
   guint8 c_raw[128] = {0};
   assert_intact(TRUE, png_file(3, 3, 0, 8, 1, c_raw, 15, 1));
   assert_png_corrupt(png_file(3, 3, 0, 8, 1, c_raw, 14, 1));
   assert_intact(TRUE, png_file(13, 7, 0, 8, 1, c_raw, 105, 3));
   assert_png_corrupt(png_file(13, 7, 0, 8, 1, c_raw, 104, 3));
   assert_intact(TRUE, png_file(10, 2, 3, 1, 0, c_raw, 6, 1));
   assert_intact(TRUE, png_file(10, 2, 3, 1, 0, c_raw, 40, 2));
   assert_png_corrupt(png_file(10, 2, 3, 1, 0, c_raw, 5, 1));
   c_raw[3] = 5; /* the second row's filter byte */
   assert_png_corrupt(png_file(10, 2, 3, 1, 0, c_raw, 6, 1));
   c_raw[3] = 0;
   assert_png_corrupt(png_file(10, 2, 3, 3, 0, c_raw, 6, 1)); /* depth */
   assert_png_corrupt(png_file(10, 2, 7, 8, 0, c_raw, 6, 1)); /* type */
   assert_png_corrupt(png_file(10, 2, 0, 8, 2, c_raw, 6, 1)); /* interlace */
}

/* Broken chunk structure: a bad critical CRC, a deflate stream that does
 * not inflate, no IDAT at all, an out-of-range length, no signature. */
static void
test_png_corruption_is_found(void) {
   gsize   u_len;
   char   *c_png = fixture_bytes("swapped.png", &u_len);
   guint8 *p_bad = g_memdup2(c_png, u_len);
   p_bad[u_len - 20] ^= 0x55; /* inside IDAT: its CRC no longer matches */
   assert_png_corrupt(temp_file("crc.png", p_bad, u_len));
   memcpy(p_bad, c_png, u_len);
   p_bad[8 + 4] = 'X'; /* IHDR renamed: not the first chunk */
   assert_png_corrupt(temp_file("noihdr.png", p_bad, u_len));
   memcpy(p_bad, c_png, u_len);
   p_bad[8] = 0x80; /* IHDR length over 2^31 - 1 */
   assert_png_corrupt(temp_file("range.png", p_bad, u_len));
   memcpy(p_bad, c_png, u_len);
   p_bad[1] = 'Q'; /* no PNG signature */
   assert_png_corrupt(temp_file("sig.png", p_bad, u_len));
   g_free(p_bad);
   g_free(c_png);
   guint8 c_junk[16];
   memset(c_junk, 0xAB, sizeof(c_junk)); /* raw bytes, no zlib header */
   GByteArray *p_a = g_byte_array_new();
   g_byte_array_append(p_a, (const guint8 *)"\x89PNG\r\n\x1a\n", 8);
   const guint8 C_IHDR[13] = {0, 0, 0, 1, 0, 0, 0, 1, 8, 0, 0, 0, 0};
   png_chunk(p_a, "IHDR", C_IHDR, 13);
   GByteArray *p_noidat = g_byte_array_sized_new(p_a->len);
   g_byte_array_append(p_noidat, p_a->data, p_a->len);
   png_chunk(p_a, "IDAT", c_junk, sizeof(c_junk));
   png_chunk(p_a, "IEND", NULL, 0);
   assert_png_corrupt(temp_file("junk.png", p_a->data, p_a->len));
   png_chunk(p_noidat, "IEND", NULL, 0);
   assert_png_corrupt(temp_file("noidat.png", p_noidat->data, p_noidat->len));
   g_byte_array_unref(p_noidat);
   g_byte_array_unref(p_a);
}

/* --- the libjpeg pass ------------------------------------------------------
 */

/* A good JPEG decodes; one libjpeg gives up on (a second SOF: gegl:jpg-load
 * would exit the process on it) is INVALID_DATA with libjpeg's message; a
 * missing file cannot be opened. Without the jpeg feature every JPEG is
 * NOT_SUPPORTED: there is no libjpeg to vouch with. */
static void
test_jpeg_decodes(void) {
   GFile  *p_file = fixture("swapped.jpg");
   GError *p_err  = NULL;
#if GGAZE_HAVE_JPEG
   g_assert_true(intact_jpeg_decodes(p_file, &p_err));
   g_assert_no_error(p_err);
   GByteArray *p_a   = fixture_array("swapped.jpg");
   gsize       u_sof = 0;
   while (!(p_a->data[u_sof] == 0xFF && p_a->data[u_sof + 1] == 0xC0)) {
      u_sof++;
   }
   gsize u_len = 2 + ((gsize)p_a->data[u_sof + 2] << 8) + p_a->data[u_sof + 3];
   guint8 *p_sof = g_memdup2(p_a->data + u_sof, u_len);
   bytes_insert(p_a, u_sof, p_sof, u_len);
   g_free(p_sof);
   GFile *p_two = temp_file("twosof.jpg", p_a->data, p_a->len);
   g_assert_false(intact_jpeg_decodes(p_two, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
   g_test_message("%s", p_err->message);
   g_clear_error(&p_err);
   g_file_delete(p_two, NULL, NULL);
   g_object_unref(p_two);
   g_byte_array_unref(p_a);
   GFile *p_none = g_file_new_for_path("/nonexistent/ggaze/x.jpg");
   g_assert_false(intact_jpeg_decodes(p_none, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED);
   g_clear_error(&p_err);
   g_object_unref(p_none);
#else
   g_assert_false(intact_jpeg_decodes(p_file, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
   g_clear_error(&p_err);
#endif
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
   g_test_add_func("/intact/sizes_are_reported", test_sizes_are_reported);
   g_test_add_func("/intact/png_rows_are_checked", test_png_rows_are_checked);
   g_test_add_func("/intact/png_corruption_is_found",
                   test_png_corruption_is_found);
   g_test_add_func("/intact/jpeg_decodes", test_jpeg_decodes);
   int i_rc = g_test_run();
   g_rmdir(c_dir);
   g_free(c_dir);
   return (i_rc);
}
