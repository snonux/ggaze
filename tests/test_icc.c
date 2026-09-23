/*:*
 * ggaze — embedded ICC profile extraction unit test (every lane, no GEGL)
 *
 * Pins icc.h over the committed fixtures (swapped.png / swapped.jpg carry
 * the same hand-built profile, badicc.png an iCCP that inflates to
 * garbage, srgb-icc.png a real sRGB profile, plain.jpg / small.png none)
 * and over hand-built containers for what no fixture should carry: a
 * profile split over out-of-order JPEG APP2 segments, a missing or
 * duplicate segment, a segment past the file's end, a marker stream that
 * is not one, an iCCP that does not inflate, a length over the cap (which
 * must allocate nothing), and 'desc' tags of both types with broken
 * offsets. Also the I/O side: a missing file is a GIO error, a file that
 * is neither PNG nor JPEG is simply "no profile".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "icc.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static GFile *
fixture(const char *c_name) {
   const char *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   char  *c_path = g_build_filename(c_dir, c_name, NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);
   return (p_file);
}

static GBytes *
read_fixture_profile(const char *c_name) {
   GFile  *p_file = fixture(c_name);
   GError *p_err  = NULL;
   GBytes *p_icc  = icc_read_embedded(p_file, &p_err);
   g_assert_no_error(p_err);
   g_object_unref(p_file);
   return (p_icc);
}

/* The PNG and the JPEG fixture embed the SAME profile bytes, and the
 * description is the one gen.py wrote. */
static void
test_fixtures_png_and_jpeg_carry_the_same_profile(void) {
   GBytes *p_png = read_fixture_profile("swapped.png");
   GBytes *p_jpg = read_fixture_profile("swapped.jpg");
   g_assert_nonnull(p_png);
   g_assert_nonnull(p_jpg);
   g_assert_true(g_bytes_equal(p_png, p_jpg));
   g_assert_true(icc_is_profile(p_png));
   char *c_desc = icc_description(p_png);
   g_assert_cmpstr(c_desc, ==, "ggaze swapped RGB");
   g_free(c_desc);
   c_desc = icc_description(p_jpg);
   g_assert_cmpstr(c_desc, ==, "ggaze swapped RGB");
   g_free(c_desc);
   g_bytes_unref(p_png);
   g_bytes_unref(p_jpg);

   GBytes *p_srgb = read_fixture_profile("srgb-icc.png");
   g_assert_true(icc_is_profile(p_srgb));
   c_desc = icc_description(p_srgb);
   g_assert_cmpstr(c_desc, ==, "ggaze sRGB test");
   g_free(c_desc);
   g_bytes_unref(p_srgb);
}

/* No profile is NULL without an error, for both containers. */
static void
test_untagged_fixtures_have_no_profile(void) {
   g_assert_null(read_fixture_profile("plain.jpg"));
   g_assert_null(read_fixture_profile("small.png"));
   g_assert_null(read_fixture_profile("rgba.png"));
}

/* badicc.png: the container is fine (the iCCP inflates), the content is
 * not a profile -- so the bytes come back and fail icc_is_profile(), which
 * is what lets the card say "unreadable" instead of naming nothing. */
static void
test_garbage_profile_is_returned_but_not_a_profile(void) {
   GBytes *p_bad = read_fixture_profile("badicc.png");
   g_assert_nonnull(p_bad);
   g_assert_false(icc_is_profile(p_bad));
   g_assert_null(icc_description(p_bad));
   g_bytes_unref(p_bad);
   g_assert_false(icc_is_profile(NULL));
   g_assert_null(icc_description(NULL));
}

static void
test_missing_file_is_an_io_error(void) {
   GFile  *p_file = g_file_new_for_path("/nonexistent/ggaze/x.png");
   GError *p_err  = NULL;
   g_assert_null(icc_read_embedded(p_file, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
   g_clear_error(&p_err);
   g_object_unref(p_file);
}

/* --- hand-built JPEG marker streams -------------------------------------- */

/* Append one APP2 ICC_PROFILE segment (sequence u_seq of u_count) carrying
 * c_data to p_jpg. */
static void
append_icc_app2(GByteArray *p_jpg, guint u_seq, guint u_count,
                const char *c_data) {
   gsize  u_n      = strlen(c_data);
   gsize  u_len    = 2 + 14 + u_n;
   guint8 c_hdr[4] = {0xFF, 0xE2, (guint8)(u_len >> 8), (guint8)u_len};
   g_byte_array_append(p_jpg, c_hdr, 4);
   g_byte_array_append(p_jpg, (const guint8 *)"ICC_PROFILE\0", 12);
   guint8 c_seq[2] = {(guint8)u_seq, (guint8)u_count};
   g_byte_array_append(p_jpg, c_seq, 2);
   g_byte_array_append(p_jpg, (const guint8 *)c_data, (guint)u_n);
}

/* SOI, then a 0xFF fill byte before an APP0 that is skipped, an RST
 * (standalone), and a non-ICC APP2 that is read and dropped. */
static GByteArray *
jpeg_head(void) {
   static const guint8 C_HEAD[] = {
      0xFF, 0xD8,                                   /* SOI */
      0xFF, 0xFF, 0xE0, 0x00, 0x04, 0x4A, 0x46,     /* fill + APP0 "JF" */
      0xFF, 0xD0,                                   /* RST0: no length */
      0xFF, 0xE2, 0x00, 0x06, 'F',  'P',  'X',  'R' /* APP2, not ICC */
   };
   GByteArray *p_jpg = g_byte_array_new();
   g_byte_array_append(p_jpg, C_HEAD, sizeof(C_HEAD));
   return (p_jpg);
}

static void
append_sos(GByteArray *p_jpg) {
   static const guint8 C_SOS[] = {0xFF, 0xDA, 0x00, 0x02, 0x12, 0x34};
   g_byte_array_append(p_jpg, C_SOS, sizeof(C_SOS));
}

static GBytes *
extract_array(GByteArray *p_arr, GError **p_err) {
   return (icc_extract(p_arr->data, p_arr->len, p_err));
}

static void
test_jpeg_segments_are_joined_in_sequence_order(void) {
   GByteArray *p_jpg = jpeg_head();
   append_icc_app2(p_jpg, 2, 3, "second");
   append_icc_app2(p_jpg, 3, 3, "third");
   append_icc_app2(p_jpg, 1, 3, "first-");
   append_sos(p_jpg);
   GError *p_err = NULL;
   GBytes *p_icc = extract_array(p_jpg, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_icc);
   gsize       u_n;
   const char *c_all = g_bytes_get_data(p_icc, &u_n);
   g_assert_cmpmem(c_all, u_n, "first-secondthird", 17);
   g_bytes_unref(p_icc);

   /* Without any ICC segment the head alone is "no profile". */
   GByteArray *p_none = jpeg_head();
   append_sos(p_none);
   g_assert_null(extract_array(p_none, &p_err));
   g_assert_no_error(p_err);
   g_byte_array_unref(p_none);
   g_byte_array_unref(p_jpg);
}

static void
assert_invalid(GByteArray *p_arr) {
   GError *p_err = NULL;
   g_assert_null(extract_array(p_arr, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
   g_clear_error(&p_err);
   g_byte_array_unref(p_arr);
}

static void
test_jpeg_broken_sequences_are_invalid_data(void) {
   GByteArray *p_gap = jpeg_head(); /* 1 of 2 only: 2 is missing */
   append_icc_app2(p_gap, 1, 2, "a");
   append_sos(p_gap);
   assert_invalid(p_gap);

   GByteArray *p_dup = jpeg_head(); /* 1 of 1 twice */
   append_icc_app2(p_dup, 1, 1, "a");
   append_icc_app2(p_dup, 1, 1, "b");
   append_sos(p_dup);
   assert_invalid(p_dup);

   GByteArray *p_seq = jpeg_head(); /* sequence 3 of a count of 2 */
   append_icc_app2(p_seq, 3, 2, "a");
   append_sos(p_seq);
   assert_invalid(p_seq);

   GByteArray *p_cnt = jpeg_head(); /* counts disagree between segments */
   append_icc_app2(p_cnt, 1, 2, "a");
   append_icc_app2(p_cnt, 2, 3, "b");
   append_sos(p_cnt);
   assert_invalid(p_cnt);

   GByteArray *p_zero = jpeg_head(); /* sequence numbers are 1-based */
   append_icc_app2(p_zero, 0, 1, "a");
   append_sos(p_zero);
   assert_invalid(p_zero);
}

/* A segment whose declared length runs past the file's end, a length
 * field below its own size, and a byte where a marker must start. */
static void
test_jpeg_broken_markers_are_invalid_data(void) {
   GByteArray *p_short = jpeg_head();
   append_icc_app2(p_short, 1, 1, "abcdef");
   g_byte_array_set_size(p_short, p_short->len - 3); /* truncated */
   assert_invalid(p_short);

   GByteArray  *p_len   = jpeg_head();
   const guint8 C_BAD[] = {0xFF, 0xE1, 0x00, 0x01};
   g_byte_array_append(p_len, C_BAD, sizeof(C_BAD));
   assert_invalid(p_len);

   GByteArray  *p_junk   = jpeg_head();
   const guint8 C_JUNK[] = {0x12, 0x34};
   g_byte_array_append(p_junk, C_JUNK, sizeof(C_JUNK));
   assert_invalid(p_junk);

   /* EOF right after SOI, or inside a fill run: nothing found, no error. */
   const guint8 C_SOI[]  = {0xFF, 0xD8};
   const guint8 C_FILL[] = {0xFF, 0xD8, 0xFF, 0xFF};
   GError      *p_err    = NULL;
   g_assert_null(icc_extract(C_SOI, sizeof(C_SOI), &p_err));
   g_assert_no_error(p_err);
   g_assert_null(icc_extract(C_FILL, sizeof(C_FILL), &p_err));
   g_assert_no_error(p_err);
}

/* --- hand-built PNG chunk streams ---------------------------------------- */

static void
append_png_chunk(GByteArray *p_png, const char *c_type, const guint8 *p_data,
                 gsize u_len) {
   guint8 c_len[4] = {(guint8)(u_len >> 24), (guint8)(u_len >> 16),
                      (guint8)(u_len >> 8), (guint8)u_len};
   g_byte_array_append(p_png, c_len, 4);
   g_byte_array_append(p_png, (const guint8 *)c_type, 4);
   g_byte_array_append(p_png, p_data, (guint)u_len);
   g_byte_array_append(p_png, (const guint8 *)"crc!", 4); /* not checked */
}

static GByteArray *
png_head(void) {
   static const guint8 C_IHDR[13] = {0, 0, 0, 1, 0, 0, 0, 1, 8, 2, 0, 0, 0};
   GByteArray         *p_png      = g_byte_array_new();
   g_byte_array_append(p_png, (const guint8 *)"\x89PNG\r\n\x1a\n", 8);
   append_png_chunk(p_png, "IHDR", C_IHDR, sizeof(C_IHDR));
   return (p_png);
}

static void
test_png_iccp_that_does_not_inflate_is_invalid_data(void) {
   GByteArray  *p_png   = png_head();
   const guint8 C_BAD[] = "ggaze\0\0not zlib at all";
   append_png_chunk(p_png, "iCCP", C_BAD, sizeof(C_BAD) - 1);
   assert_invalid(p_png);

   /* A name without its NUL terminator / a wrong compression method. */
   GByteArray  *p_name    = png_head();
   const guint8 C_NONUL[] = "ggaze";
   append_png_chunk(p_name, "iCCP", C_NONUL, sizeof(C_NONUL) - 1);
   assert_invalid(p_name);
   GByteArray  *p_meth   = png_head();
   const guint8 C_METH[] = "ggaze\0\1x";
   append_png_chunk(p_meth, "iCCP", C_METH, sizeof(C_METH) - 1);
   assert_invalid(p_meth);

   /* A declared length over the cap is refused before reading it. */
   GByteArray  *p_huge  = png_head();
   const guint8 C_LEN[] = {0x7F, 0xFF, 0xFF, 0xFF, 'i', 'C', 'C', 'P'};
   g_byte_array_append(p_huge, C_LEN, sizeof(C_LEN));
   assert_invalid(p_huge);

   /* A chunk cut off by the end of the file. */
   GByteArray  *p_cut   = png_head();
   const guint8 C_CUT[] = {0x00, 0x00, 0x01, 0x00, 'i', 'C', 'C', 'P', 'g'};
   g_byte_array_append(p_cut, C_CUT, sizeof(C_CUT));
   assert_invalid(p_cut);
}

/* IDAT before any iCCP, EOF inside a skipped chunk, and a file that is
 * neither PNG nor JPEG: all "no profile", no error. */
static void
test_png_without_iccp_and_foreign_bytes_have_no_profile(void) {
   GError     *p_err = NULL;
   GByteArray *p_png = png_head();
   append_png_chunk(p_png, "IDAT", (const guint8 *)"x", 1);
   g_assert_null(extract_array(p_png, &p_err));
   g_assert_no_error(p_err);
   g_byte_array_unref(p_png);

   GByteArray  *p_eof    = png_head();
   const guint8 C_TEXT[] = {0x00, 0x00, 0x00, 0x20, 't', 'E', 'X', 't', 'a'};
   g_byte_array_append(p_eof, C_TEXT, sizeof(C_TEXT));
   g_assert_null(extract_array(p_eof, &p_err));
   g_assert_no_error(p_err);
   g_byte_array_unref(p_eof);

   g_assert_null(icc_extract((const guint8 *)"hello world", 11, &p_err));
   g_assert_no_error(p_err);
   g_assert_null(icc_extract((const guint8 *)"\x89PNG", 4, &p_err));
   g_assert_no_error(p_err);
   g_assert_null(icc_extract(NULL, 0, &p_err));
   g_assert_no_error(p_err);
}

/* --- hand-built profiles: header + one 'desc' tag ------------------------ */

static void
put_be32(guint8 *p, guint32 u) {
   p[0] = (guint8)(u >> 24);
   p[1] = (guint8)(u >> 16);
   p[2] = (guint8)(u >> 8);
   p[3] = (guint8)u;
}

/* A profile of a 128-byte header, a one-entry tag table naming 'desc' at
 * u_off / u_size, and p_tag's bytes at offset 144. */
static GBytes *
profile_with_desc(const guint8 *p_tag, gsize u_tag, guint32 u_off,
                  guint32 u_size) {
   gsize   u_len = 144 + u_tag;
   guint8 *p     = g_malloc0(u_len);
   put_be32(p, (guint32)u_len);
   memcpy(p + 36, "acsp", 4);
   put_be32(p + 128, 1);
   memcpy(p + 132, "desc", 4);
   put_be32(p + 136, u_off);
   put_be32(p + 140, u_size);
   memcpy(p + 144, p_tag, u_tag);
   return (g_bytes_new_take(p, u_len));
}

static void
assert_desc(GBytes *p_icc, const char *c_want) {
   char *c_desc = icc_description(p_icc);
   g_assert_cmpstr(c_desc, ==, c_want);
   g_free(c_desc);
   g_bytes_unref(p_icc);
}

static void
test_description_mluc_prefers_english(void) {
   /* Two records: "de" first ("Weit"), then "en" ("Wide gamut"). */
   guint8 c_tag[16 + 24 + 8 + 20];
   memset(c_tag, 0, sizeof(c_tag));
   memcpy(c_tag, "mluc", 4);
   put_be32(c_tag + 8, 2);
   put_be32(c_tag + 12, 12);
   memcpy(c_tag + 16, "deDE", 4);
   put_be32(c_tag + 20, 8);
   put_be32(c_tag + 24, 40);
   memcpy(c_tag + 28, "enUS", 4);
   put_be32(c_tag + 32, 20);
   put_be32(c_tag + 36, 48);
   const char *c_de = "Weit", *c_en = "Wide gamut";
   for (gsize u = 0; u < 4; u++) {
      c_tag[40 + 2 * u + 1] = (guint8)c_de[u];
   }
   for (gsize u = 0; u < 10; u++) {
      c_tag[48 + 2 * u + 1] = (guint8)c_en[u];
   }
   assert_desc(profile_with_desc(c_tag, sizeof(c_tag), 144, sizeof(c_tag)),
               "Wide gamut");

   /* Only the German record: the first one is taken. */
   put_be32(c_tag + 8, 1);
   assert_desc(profile_with_desc(c_tag, sizeof(c_tag), 144, sizeof(c_tag)),
               "Weit");

   /* A record whose text runs past the tag: NULL, not a read past it. */
   put_be32(c_tag + 20, 4000);
   assert_desc(profile_with_desc(c_tag, sizeof(c_tag), 144, sizeof(c_tag)),
               NULL);
   /* An odd byte length is not UTF-16. */
   put_be32(c_tag + 20, 7);
   assert_desc(profile_with_desc(c_tag, sizeof(c_tag), 144, sizeof(c_tag)),
               NULL);
   /* A record size below the 12 bytes a record needs. */
   put_be32(c_tag + 20, 8);
   put_be32(c_tag + 12, 4);
   assert_desc(profile_with_desc(c_tag, sizeof(c_tag), 144, sizeof(c_tag)),
               NULL);
}

static void
test_description_ascii_and_broken_tables(void) {
   guint8 c_tag[12 + 6];
   memcpy(c_tag, "desc\0\0\0\0", 8);
   put_be32(c_tag + 8, 6);
   memcpy(c_tag + 12, "  Foo\0", 6);
   assert_desc(profile_with_desc(c_tag, sizeof(c_tag), 144, sizeof(c_tag)),
               "Foo");

   /* Count past the tag, an empty count, invalid UTF-8, an unknown type,
    * a tag entry pointing past the data, an oversized tag table, a
    * declared profile size beyond the bytes, no 'acsp'. */
   put_be32(c_tag + 8, 60);
   assert_desc(profile_with_desc(c_tag, sizeof(c_tag), 144, sizeof(c_tag)),
               NULL);
   put_be32(c_tag + 8, 0);
   assert_desc(profile_with_desc(c_tag, sizeof(c_tag), 144, sizeof(c_tag)),
               NULL);
   put_be32(c_tag + 8, 3);
   memcpy(c_tag + 12, "\xff\xfe\xfd", 3);
   assert_desc(profile_with_desc(c_tag, sizeof(c_tag), 144, sizeof(c_tag)),
               NULL);
   memcpy(c_tag, "text", 4);
   assert_desc(profile_with_desc(c_tag, sizeof(c_tag), 144, sizeof(c_tag)),
               NULL);
   memcpy(c_tag, "desc", 4);
   assert_desc(profile_with_desc(c_tag, sizeof(c_tag), 144, 4000), NULL);
   assert_desc(profile_with_desc(c_tag, sizeof(c_tag), 4000, 4), NULL);
   assert_desc(profile_with_desc(c_tag, sizeof(c_tag), 144, 4), NULL);

   GBytes *p_many = profile_with_desc(c_tag, sizeof(c_tag), 144, 18);
   guint8 *p_m    = g_memdup2(g_bytes_get_data(p_many, NULL), 144 + 18);
   put_be32(p_m + 128, 1000);
   g_bytes_unref(p_many);
   assert_desc(g_bytes_new_take(p_m, 144 + 18), NULL);

   GBytes *p_size = profile_with_desc(c_tag, sizeof(c_tag), 144, 18);
   guint8 *p_s    = g_memdup2(g_bytes_get_data(p_size, NULL), 144 + 18);
   put_be32(p_s, 5000);
   g_bytes_unref(p_size);
   GBytes *p_big = g_bytes_new_take(p_s, 144 + 18);
   g_assert_false(icc_is_profile(p_big));
   assert_desc(p_big, NULL);

   GBytes *p_sig = profile_with_desc(c_tag, sizeof(c_tag), 144, 18);
   guint8 *p_g   = g_memdup2(g_bytes_get_data(p_sig, NULL), 144 + 18);
   memcpy(p_g + 36, "nope", 4);
   g_bytes_unref(p_sig);
   assert_desc(g_bytes_new_take(p_g, 144 + 18), NULL);

   /* A profile with a tag table naming no 'desc' at all. */
   GBytes *p_no = profile_with_desc(c_tag, sizeof(c_tag), 144, 18);
   guint8 *p_n  = g_memdup2(g_bytes_get_data(p_no, NULL), 144 + 18);
   memcpy(p_n + 132, "cprt", 4);
   g_bytes_unref(p_no);
   assert_desc(g_bytes_new_take(p_n, 144 + 18), NULL);
}

int
main(int argc, char **argv) {
   g_test_init(&argc, &argv, NULL);
   g_test_add_func("/icc/fixtures_png_and_jpeg_carry_the_same_profile",
                   test_fixtures_png_and_jpeg_carry_the_same_profile);
   g_test_add_func("/icc/untagged_fixtures_have_no_profile",
                   test_untagged_fixtures_have_no_profile);
   g_test_add_func("/icc/garbage_profile_is_returned_but_not_a_profile",
                   test_garbage_profile_is_returned_but_not_a_profile);
   g_test_add_func("/icc/missing_file_is_an_io_error",
                   test_missing_file_is_an_io_error);
   g_test_add_func("/icc/jpeg_segments_are_joined_in_sequence_order",
                   test_jpeg_segments_are_joined_in_sequence_order);
   g_test_add_func("/icc/jpeg_broken_sequences_are_invalid_data",
                   test_jpeg_broken_sequences_are_invalid_data);
   g_test_add_func("/icc/jpeg_broken_markers_are_invalid_data",
                   test_jpeg_broken_markers_are_invalid_data);
   g_test_add_func("/icc/png_iccp_that_does_not_inflate_is_invalid_data",
                   test_png_iccp_that_does_not_inflate_is_invalid_data);
   g_test_add_func("/icc/png_without_iccp_and_foreign_bytes_have_no_profile",
                   test_png_without_iccp_and_foreign_bytes_have_no_profile);
   g_test_add_func("/icc/description_mluc_prefers_english",
                   test_description_mluc_prefers_english);
   g_test_add_func("/icc/description_ascii_and_broken_tables",
                   test_description_ascii_and_broken_tables);
   return (g_test_run());
}
