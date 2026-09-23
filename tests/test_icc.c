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

#include "streamread.h"

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

/* icc_container_searched(): the PNG / JPEG signatures only. A format the
 * walk never looks into (AVIF), a file too short for a signature and a
 * missing file are all "not searched". */
static void
test_container_searched(void) {
   const char *C_YES[] = {"swapped.png", "plain.jpg", "small.png"};
   for (gsize u = 0; u < G_N_ELEMENTS(C_YES); u++) {
      GFile *p_file = fixture(C_YES[u]);
      g_assert_true(icc_container_searched(p_file));
      g_object_unref(p_file);
   }
   GFile *p_avif = fixture("tiny.avif");
   g_assert_false(icc_container_searched(p_avif));
   g_object_unref(p_avif);

   char *c_dir   = g_dir_make_tmp("ggaze-icc-XXXXXX", NULL);
   char *c_short = g_build_filename(c_dir, "short.png", NULL);
   g_assert_true(g_file_set_contents(c_short, "\x89PNG", 4, NULL));
   GFile *p_short = g_file_new_for_path(c_short);
   g_assert_false(icc_container_searched(p_short));
   g_object_unref(p_short);
   g_remove(c_short);
   g_rmdir(c_dir);
   g_free(c_short);
   g_free(c_dir);

   GFile *p_gone = g_file_new_for_path("/nonexistent/ggaze/x.png");
   g_assert_false(icc_container_searched(p_gone));
   g_object_unref(p_gone);
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
/* Stray bytes between segments, and a stuffed FF 00 where a marker was
 * due, are skipped the way libjpeg skips them: the profile after them is
 * still found (xb2 review: the walk used to call such a file broken, and
 * the info card said "unreadable"). */
static void
test_jpeg_padding_between_segments_is_skipped(void) {
   GByteArray  *p_jpg   = jpeg_head();
   const guint8 C_PAD[] = {0x00, 0x11, 0x22, 0xFF, 0x00, 0x33};
   g_byte_array_append(p_jpg, C_PAD, sizeof(C_PAD));
   append_icc_app2(p_jpg, 1, 1, "padded");
   append_sos(p_jpg);
   GError *p_err = NULL;
   GBytes *p_icc = extract_array(p_jpg, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_icc);
   g_assert_cmpmem(g_bytes_get_data(p_icc, NULL), g_bytes_get_size(p_icc),
                   "padded", 6);
   g_bytes_unref(p_icc);
   g_byte_array_unref(p_jpg);
}

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

   /* Junk where a marker should start is padding (libjpeg skips it), but
    * not without end: past STREAMREAD_JPEG_MAX_PAD it is no marker stream. */
   GByteArray *p_junk = jpeg_head();
   g_byte_array_set_size(p_junk, p_junk->len + STREAMREAD_JPEG_MAX_PAD + 8);
   memset(p_junk->data + p_junk->len - STREAMREAD_JPEG_MAX_PAD - 8, 0x12,
          STREAMREAD_JPEG_MAX_PAD + 8);
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
    * a tag entry pointing past the data (the header-level breakage is
    * test_description_broken_headers'). */
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
}

/* The valid 18-byte ASCII 'desc' profile with u_len bytes of p_patch
 * written at offset u_at: one broken header or tag-table field per call. */
static GBytes *
patched_profile(gsize u_at, const void *p_patch, gsize u_len) {
   guint8 c_tag[12 + 6];
   memcpy(c_tag, "desc\0\0\0\0", 8);
   put_be32(c_tag + 8, 6);
   memcpy(c_tag + 12, "  Foo\0", 6);
   GBytes *p_ok = profile_with_desc(c_tag, sizeof(c_tag), 144, 18);
   guint8 *p_b  = g_memdup2(g_bytes_get_data(p_ok, NULL), 144 + 18);
   g_bytes_unref(p_ok);
   memcpy(p_b + u_at, p_patch, u_len);
   return (g_bytes_new_take(p_b, 144 + 18));
}

/* An oversized tag table, a declared profile size beyond the bytes, no
 * 'acsp', and a tag table naming no 'desc' at all. */
static void
test_description_broken_headers(void) {
   const guint8 C_COUNT[4] = {0, 0, 0x03, 0xE8}; /* 1000 tags */
   assert_desc(patched_profile(128, C_COUNT, 4), NULL);
   const guint8 C_SIZE[4] = {0, 0, 0x13, 0x88}; /* 5000 bytes */
   GBytes      *p_big     = patched_profile(0, C_SIZE, 4);
   g_assert_false(icc_is_profile(p_big));
   assert_desc(p_big, NULL);
   assert_desc(patched_profile(36, "nope", 4), NULL);
   assert_desc(patched_profile(132, "cprt", 4), NULL);
}

/* --- icc_profile_is_sane: what babl may be handed --------------------------
 *
 * Each case mutates one field of a fixture profile (tag tables printed in
 * tests/fixtures/gen.py's terms: swapped.png's r/g/bTRC are 14-byte
 * 'curv' tags with one point, srgb-icc.png's are 32-byte 'para' type 3,
 * grey-icc.png has a kTRC, cmyk-icc.jpg an A2B0 CLUT). */

/* A writable copy of c_name's embedded profile. */
static GByteArray *
profile_copy(const char *c_name) {
   GBytes     *p_icc = read_fixture_profile(c_name);
   gsize       u_len = 0;
   const void *p_src = g_bytes_get_data(p_icc, &u_len);
   GByteArray *p_arr = g_byte_array_sized_new((guint)u_len);
   g_byte_array_append(p_arr, p_src, (guint)u_len);
   g_bytes_unref(p_icc);
   return (p_arr);
}

static guint32
get_be32(const guint8 *p) {
   return (((guint32)p[0] << 24) | ((guint32)p[1] << 16) |
           ((guint32)p[2] << 8) | p[3]);
}

/* The tag-table entry of c_sig (signature, offset, size) in p_arr. */
static guint8 *
tag_entry(GByteArray *p_arr, const char *c_sig) {
   guint32 u_count = get_be32(p_arr->data + 128);
   for (guint32 u = 0; u < u_count; u++) {
      guint8 *p_e = p_arr->data + 132 + 12 * u;
      if (memcmp(p_e, c_sig, 4) == 0) {
         return (p_e);
      }
   }
   g_assert_not_reached();
   return (NULL);
}

/* The data of tag c_sig in p_arr. */
static guint8 *
tag_data(GByteArray *p_arr, const char *c_sig) {
   return (p_arr->data + get_be32(tag_entry(p_arr, c_sig) + 4));
}

static gboolean
is_sane(GByteArray *p_arr) {
   GBytes  *p_b    = g_bytes_new(p_arr->data, p_arr->len);
   gboolean b_sane = icc_profile_is_sane(p_b);
   g_bytes_unref(p_b);
   return (b_sane);
}

/* Every fixture profile passes: matrix/TRC with 'curv' and 'para' curves,
 * grey, CMYK; garbage and NULL do not. */
static void
test_sane_fixture_profiles_pass(void) {
   const char *C_NAMES[] = {"swapped.png",  "srgb-icc.png", "grey-icc.png",
                            "cmyk-icc.jpg", "grey-icc.jpg", "srgb-icc.jpg"};
   for (gsize u = 0; u < G_N_ELEMENTS(C_NAMES); u++) {
      GByteArray *p_arr = profile_copy(C_NAMES[u]);
      g_assert_true(is_sane(p_arr));
      g_byte_array_unref(p_arr);
   }
   g_assert_false(icc_profile_is_sane(NULL));
   GBytes *p_bad = read_fixture_profile("badicc.png");
   g_assert_false(icc_profile_is_sane(p_bad));
   g_bytes_unref(p_bad);
}

/* The header: a size field other than the byte count (either way), and a
 * tag count whose table would run past the profile. */
static void
test_sane_header_and_table(void) {
   GByteArray *p_arr  = profile_copy("swapped.png");
   guint8      c_zero = 0;
   g_byte_array_append(p_arr, &c_zero, 1); /* one byte the size omits */
   g_assert_false(is_sane(p_arr));
   put_be32(p_arr->data, p_arr->len); /* now it says so */
   g_assert_true(is_sane(p_arr));
   put_be32(p_arr->data, p_arr->len + 4);
   g_assert_false(is_sane(p_arr));
   g_byte_array_unref(p_arr);
   p_arr = profile_copy("swapped.png");
   put_be32(p_arr->data + 128, 0x7fffffffu);
   g_assert_false(is_sane(p_arr));
   put_be32(p_arr->data + 128, 10); /* one entry past the real nine: the
                                     * 'desc' data read as a tag entry */
   g_assert_false(is_sane(p_arr));
   g_byte_array_unref(p_arr);
}

/* More than ICC_MAX_TAGS entries, each a sound 8-byte tag, is refused
 * even though every one of them lies inside the profile. */
static void
test_sane_tag_count_is_bounded(void) {
   for (guint32 u_n = ICC_MAX_TAGS; u_n <= ICC_MAX_TAGS + 1; u_n++) {
      guint32 u_data = 132 + 12 * u_n;
      guint32 u_len  = u_data + 8;
      guint8 *p      = g_malloc0(u_len);
      put_be32(p, u_len);
      memcpy(p + 36, "acsp", 4);
      put_be32(p + 128, u_n);
      for (guint32 u = 0; u < u_n; u++) {
         memcpy(p + 132 + 12 * u, "zzzz", 4);
         put_be32(p + 132 + 12 * u + 4, u_data);
         put_be32(p + 132 + 12 * u + 8, 8);
      }
      memcpy(p + u_data, "junk", 4);
      GBytes *p_b = g_bytes_new_take(p, u_len);
      g_assert_true(icc_profile_is_sane(p_b) == (u_n <= ICC_MAX_TAGS));
      g_bytes_unref(p_b);
   }
}

/* A tag outside the profile, over the header / table, too small for its
 * type header, or with an offset + size that wraps 32 bits ("negative"
 * to babl's int arithmetic) is refused; tags SHARING data -- rTRC, gTRC
 * and bTRC pointing at one curve, as real profiles do -- are fine. */
static void
test_sane_tag_bounds(void) {
   struct {
      guint32  u_off;
      guint32  u_size;
      gboolean b_ok;
   } CASES[] = {
      {488, 14, TRUE},            /* as it is */
      {472, 14, TRUE},            /* rTRC's data, shared */
      {512, 14, FALSE},           /* past the end (520 bytes) */
      {488, 33, FALSE},           /* runs past the end */
      {0, 14, FALSE},             /* over the header */
      {140, 14, FALSE},           /* over the tag table */
      {488, 4, FALSE},            /* no room for the type header */
      {0xfffffff0u, 0x20, FALSE}, /* offset + size wraps */
      {0x80000000u, 14, FALSE},   /* a negative int to babl */
   };
   for (gsize u = 0; u < G_N_ELEMENTS(CASES); u++) {
      GByteArray *p_arr = profile_copy("swapped.png");
      guint8     *p_e   = tag_entry(p_arr, "gTRC");
      put_be32(p_e + 4, CASES[u].u_off);
      put_be32(p_e + 8, CASES[u].u_size);
      g_assert_true(is_sane(p_arr) == CASES[u].b_ok);
      g_byte_array_unref(p_arr);
   }
   /* A tag babl never reads is held to the same bounds (LCMS reads it). */
   GByteArray *p_arr = profile_copy("swapped.png");
   put_be32(tag_entry(p_arr, "cprt") + 8, 1000);
   g_assert_false(is_sane(p_arr));
   g_byte_array_unref(p_arr);
}

/* A 'curv' count: the one babl turned into a crash (0x01000000 points in
 * a 14-byte tag), and the edges -- exactly filling the tag, one more. */
static void
test_sane_curv_count(void) {
   GByteArray *p_arr = profile_copy("swapped.png");
   put_be32(tag_data(p_arr, "gTRC") + 8, 0x01000000u);
   g_assert_false(is_sane(p_arr));
   put_be32(tag_data(p_arr, "gTRC") + 8, 0xffffffffu);
   g_assert_false(is_sane(p_arr));
   put_be32(tag_data(p_arr, "gTRC") + 8, 0); /* identity: no points */
   g_assert_true(is_sane(p_arr));
   put_be32(tag_entry(p_arr, "gTRC") + 8, 16); /* room for 2 points */
   put_be32(tag_data(p_arr, "gTRC") + 8, 2);
   g_assert_true(is_sane(p_arr));
   put_be32(tag_data(p_arr, "gTRC") + 8, 3);
   g_assert_false(is_sane(p_arr));
   put_be32(tag_entry(p_arr, "gTRC") + 8, 11); /* no room for the count */
   put_be32(tag_data(p_arr, "gTRC") + 8, 0);
   g_assert_false(is_sane(p_arr));
   g_byte_array_unref(p_arr);
   /* The point cap, in a profile large enough to hold the points. */
   for (guint32 u_n = ICC_MAX_CURVE_POINTS; u_n <= ICC_MAX_CURVE_POINTS + 1;
        u_n++) {
      p_arr          = profile_copy("grey-icc.png");
      guint32 u_off  = p_arr->len;
      guint32 u_size = 12 + 2 * u_n;
      g_byte_array_set_size(p_arr, u_off + u_size);
      memset(p_arr->data + u_off, 0, u_size);
      memcpy(p_arr->data + u_off, "curv", 4);
      put_be32(p_arr->data + u_off + 8, u_n);
      put_be32(tag_entry(p_arr, "kTRC") + 4, u_off);
      put_be32(tag_entry(p_arr, "kTRC") + 8, u_size);
      put_be32(p_arr->data, p_arr->len);
      g_assert_true(is_sane(p_arr) == (u_n <= ICC_MAX_CURVE_POINTS));
      g_byte_array_unref(p_arr);
   }
}

/* A 'para' curve: a truncated one (type 3 needs 12 + 5 * 4 bytes), each
 * known function type at its own least size, an unknown type, and a TRC
 * of a type babl would misread as a 'curv'. */
static void
test_sane_para_and_trc_types(void) {
   GByteArray *p_arr = profile_copy("srgb-icc.png");
   put_be32(tag_entry(p_arr, "gTRC") + 8, 31);
   g_assert_false(is_sane(p_arr));
   const guint32 C_NEED[] = {16, 24, 28, 32};
   for (guint u_fn = 0; u_fn < G_N_ELEMENTS(C_NEED); u_fn++) {
      tag_data(p_arr, "gTRC")[9] = (guint8)u_fn;
      put_be32(tag_entry(p_arr, "gTRC") + 8, C_NEED[u_fn]);
      g_assert_true(is_sane(p_arr));
      put_be32(tag_entry(p_arr, "gTRC") + 8, C_NEED[u_fn] - 1);
      g_assert_false(is_sane(p_arr));
   }
   put_be32(tag_entry(p_arr, "gTRC") + 8, 32);
   tag_data(p_arr, "gTRC")[9] = 4; /* type 4 needs 7 parameters: 40 */
   g_assert_false(is_sane(p_arr));
   tag_data(p_arr, "gTRC")[9] = 5; /* no such type */
   g_assert_false(is_sane(p_arr));
   tag_data(p_arr, "gTRC")[9] = 3;
   g_assert_true(is_sane(p_arr));
   memcpy(tag_data(p_arr, "gTRC"), "text", 4);
   g_assert_false(is_sane(p_arr));
   g_byte_array_unref(p_arr);
}

/* The XYZ tags babl reads three numbers from: 20 bytes and the 'XYZ '
 * type, for the primaries and the white point. */
static void
test_sane_xyz_tags(void) {
   const char *C_SIGS[] = {"rXYZ", "gXYZ", "bXYZ", "wtpt"};
   for (gsize u = 0; u < G_N_ELEMENTS(C_SIGS); u++) {
      GByteArray *p_arr = profile_copy("swapped.png");
      put_be32(tag_entry(p_arr, C_SIGS[u]) + 8, 19);
      g_assert_false(is_sane(p_arr));
      put_be32(tag_entry(p_arr, C_SIGS[u]) + 8, 20);
      g_assert_true(is_sane(p_arr));
      memcpy(tag_data(p_arr, C_SIGS[u]), "curv", 4);
      g_assert_false(is_sane(p_arr));
      g_byte_array_unref(p_arr);
   }
}

/* Random damage to every fixture profile, many times over: the validator
 * (and icc_description over the same bytes) must never read outside them
 * -- the ASan lane is where this bites -- and whatever it passes must still
 * be what it promises for the tags babl reads. A fixed seed per run
 * (g_test_rand_*), so a failure reproduces with the printed seed. */
static void
mutate(GByteArray *p_arr) {
   guint u_pos = (guint)g_test_rand_int_range(0, (gint32)p_arr->len);
   switch (g_test_rand_int_range(0, 4)) {
   case 0:
      p_arr->data[u_pos] = (guint8)g_test_rand_int_range(0, 256);
      break;
   case 1: {
      static const guint32 C_EDGE[] = {
         0, 1, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0x01000000u, 20, 14};
      if (u_pos + 4 <= p_arr->len) {
         put_be32(p_arr->data + u_pos,
                  C_EDGE[g_test_rand_int_range(0, G_N_ELEMENTS(C_EDGE))]);
      }
      break;
   }
   case 2:
      g_byte_array_set_size(
         p_arr, (guint)g_test_rand_int_range(132, (gint32)p_arr->len + 1));
      break;
   default:
      put_be32(p_arr->data, p_arr->len); /* keep the size field honest */
      break;
   }
}

static void
test_sane_fuzz(void) {
   const char *C_NAMES[] = {"swapped.png", "srgb-icc.png", "grey-icc.png",
                            "cmyk-icc.jpg"};
   guint       u_passed  = 0;
   for (guint u_run = 0; u_run < 20000; u_run++) {
      GByteArray *p_arr = profile_copy(C_NAMES[u_run % G_N_ELEMENTS(C_NAMES)]);
      guint       u_n   = (guint)g_test_rand_int_range(1, 6);
      for (guint u = 0; u < u_n; u++) {
         mutate(p_arr);
      }
      GBytes *p_b = g_bytes_new(p_arr->data, p_arr->len);
      if (icc_profile_is_sane(p_b)) {
         u_passed++;
         g_assert_cmpuint(get_be32(p_arr->data), ==, p_arr->len);
      }
      g_free(icc_description(p_b));
      g_bytes_unref(p_b);
      g_byte_array_unref(p_arr);
   }
   g_test_message("%u of 20000 mutated profiles passed", u_passed);
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
   g_test_add_func("/icc/jpeg_padding_between_segments_is_skipped",
                   test_jpeg_padding_between_segments_is_skipped);
   g_test_add_func("/icc/jpeg_broken_markers_are_invalid_data",
                   test_jpeg_broken_markers_are_invalid_data);
   g_test_add_func("/icc/png_iccp_that_does_not_inflate_is_invalid_data",
                   test_png_iccp_that_does_not_inflate_is_invalid_data);
   g_test_add_func("/icc/png_without_iccp_and_foreign_bytes_have_no_profile",
                   test_png_without_iccp_and_foreign_bytes_have_no_profile);
   g_test_add_func("/icc/container_searched", test_container_searched);
   g_test_add_func("/icc/description_mluc_prefers_english",
                   test_description_mluc_prefers_english);
   g_test_add_func("/icc/description_broken_headers",
                   test_description_broken_headers);
   g_test_add_func("/icc/description_ascii_and_broken_tables",
                   test_description_ascii_and_broken_tables);
   g_test_add_func("/icc/sane_fixture_profiles_pass",
                   test_sane_fixture_profiles_pass);
   g_test_add_func("/icc/sane_header_and_table", test_sane_header_and_table);
   g_test_add_func("/icc/sane_tag_count_is_bounded",
                   test_sane_tag_count_is_bounded);
   g_test_add_func("/icc/sane_tag_bounds", test_sane_tag_bounds);
   g_test_add_func("/icc/sane_curv_count", test_sane_curv_count);
   g_test_add_func("/icc/sane_para_and_trc_types",
                   test_sane_para_and_trc_types);
   g_test_add_func("/icc/sane_xyz_tags", test_sane_xyz_tags);
   g_test_add_func("/icc/sane_fuzz", test_sane_fuzz);
   return (g_test_run());
}
