/*:*
 * ggaze — info / EXIF unit test
 *
 * Gathers EXIF from the fixtures: plain.jpg (orientation 1) and rot6.jpg
 * (orientation 6, 8x4 stored). Asserts dimensions, orientation, and that
 * the format string is non-empty. Uses $GGAZE_FIXTURES_DIR.
 *
 * Also covers hardening against malformed/crafted input: oversized JPEG
 * headers (mu0) and a malformed EXIF Orientation entry with a mismatched
 * format/undersized data buffer (lu0) -- both must fail safe, never crash.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "info.h"

#include <glib.h>
#include <libexif/exif-tag.h>
#include <string.h>
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

/* Byte-wise (alignment-safe) 16/32-bit reads and a 16-bit write, since the
 * TIFF fields below sit at arbitrary offsets inside a byte buffer -- casting
 * to a wider integer pointer and dereferencing would be misaligned-access
 * undefined behavior. */
static guint16
_tiff_u16(const guint8 *p, gboolean b_big) {
   return (b_big ? (guint16)((p[0] << 8) | p[1])
                 : (guint16)((p[1] << 8) | p[0]));
}

static guint32
_tiff_u32(const guint8 *p, gboolean b_big) {
   return (b_big ? ((guint32)p[0] << 24) | ((guint32)p[1] << 16) |
                      ((guint32)p[2] << 8) | p[3]
                 : ((guint32)p[3] << 24) | ((guint32)p[2] << 16) |
                      ((guint32)p[1] << 8) | p[0]);
}

static void
_tiff_set_u16(guint8 *p, gboolean b_big, guint16 u_val) {
   if (b_big) {
      p[0] = (guint8)(u_val >> 8);
      p[1] = (guint8)(u_val & 0xFF);
   } else {
      p[0] = (guint8)(u_val & 0xFF);
      p[1] = (guint8)(u_val >> 8);
   }
}

/* Find the Exif TIFF header ("Exif\0\0" followed by "II"/"MM") inside a JPEG
 * byte buffer, locate the IFD0 entry for EXIF_TAG_ORIENTATION (0x0112), and
 * rewrite its declared format from SHORT (3) to UNDEFINED (7) while leaving
 * its component count (1) untouched. libexif's exif_data_load_data_entry()
 * allocates entry->data as exif_format_get_size(format)*components bytes
 * (exif-data.c) -- UNDEFINED x 1 component allocates just 1 byte, versus the
 * 2 bytes exif_get_short() unconditionally reads. UNDEFINED (rather than the
 * more obvious BYTE) is used deliberately: libexif's own exif_data_fix() /
 * exif_entry_fix() pass, run by default after every parse, auto-"repairs" an
 * Orientation entry declared as BYTE/SBYTE/LONG/SLONG/SSHORT back to SHORT
 * with a correctly sized buffer -- verified empirically with a standalone
 * probe against libexif 0.6.26 -- but leaves UNDEFINED (and a few other
 * formats outside that conversion list) untouched, so it survives to reach
 * _get_orientation() as the genuine surviving attack surface. This crafts
 * the exact mismatched-format/undersized-buffer case lu0 exists to guard
 * against: a malformed but well-formed-enough-to-parse EXIF Orientation
 * entry that makes _get_orientation()'s old unchecked exif_get_short() call
 * read one byte past the end of the 1-byte heap allocation (confirmed via
 * Valgrind memcheck: "Invalid read of size 2 ... 0 bytes inside a block of
 * size 1 alloc'd" -- see lu0's annotation for the full non-tautological
 * verification). Returns TRUE if the Orientation entry was found and
 * patched. */
static gboolean
_patch_orientation_format_undefined(guint8 *p_buf, gsize u_len) {
   for (gsize u = 0; u + 10 < u_len; u++) {
      if (memcmp(p_buf + u, "Exif\0\0", 6) != 0) {
         continue;
      }
      guint8  *p_tiff     = p_buf + u + 6;
      gsize    u_tiff_len = u_len - (u + 6);
      gboolean b_big;
      if (memcmp(p_tiff, "MM", 2) == 0) {
         b_big = TRUE;
      } else if (memcmp(p_tiff, "II", 2) == 0) {
         b_big = FALSE;
      } else {
         continue;
      }
      if (u_tiff_len < 8) {
         continue;
      }
      guint32 u_ifd0_off = _tiff_u32(p_tiff + 4, b_big);
      if (u_ifd0_off + 2 > u_tiff_len) {
         continue;
      }
      guint16 u_num_entries = _tiff_u16(p_tiff + u_ifd0_off, b_big);
      for (guint16 u_i = 0; u_i < u_num_entries; u_i++) {
         gsize u_entry_off = u_ifd0_off + 2 + (gsize)u_i * 12;
         if (u_entry_off + 12 > u_tiff_len) {
            break;
         }
         guint8 *p_entry = p_tiff + u_entry_off;
         guint16 u_tag   = _tiff_u16(p_entry, b_big);
         if (u_tag != EXIF_TAG_ORIENTATION) {
            continue;
         }
         _tiff_set_u16(p_entry + 2, b_big, 7); /* EXIF_FORMAT_UNDEFINED */
         return (TRUE);
      }
   }
   return (FALSE);
}

/* lu0: _get_orientation() used to check only p_entry->components/data being
 * non-null before calling exif_get_short(), which always reads 2 bytes. A
 * crafted Orientation entry that declares a mismatched format (here
 * UNDEFINED instead of the required SHORT) gets a 1-byte data allocation
 * from libexif (see _patch_orientation_format_undefined() above) --
 * exif_get_short() then reads one byte past that allocation (confirmed via
 * Valgrind memcheck against a standalone probe -- see lu0's annotation).
 * Exercises the REAL public entry point (info_new()) and asserts the fixed
 * behavior: format/size are validated before any read, so the malformed
 * entry is treated as unknown orientation (0) rather than triggering an
 * out-of-bounds read. */
static void
test_malformed_exif_orientation_format(void) {
   const char *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   char  *c_src = g_build_filename(c_dir, "plain.jpg", NULL);
   gchar *p_buf = NULL;
   gsize  u_len = 0;
   g_assert_true(g_file_get_contents(c_src, &p_buf, &u_len, NULL));
   g_free(c_src);
   g_assert_true(_patch_orientation_format_undefined((guint8 *)p_buf, u_len));

   gchar  *c_tmp = NULL;
   GError *p_sub = NULL;
   gint    i_fd  = g_file_open_tmp("ggaze-info-badexif-XXXXXX", &c_tmp, &p_sub);
   g_assert_no_error(p_sub);
   g_assert_cmpint(i_fd, >=, 0);
   g_assert_cmpint((glong)write(i_fd, p_buf, u_len), ==, (glong)u_len);
   close(i_fd);
   g_free(p_buf);

   GFile     *p_file = g_file_new_for_path(c_tmp);
   GgazeInfo *p_info = info_new(p_file);

   g_assert_nonnull(p_info);
   g_assert_cmpint(p_info->i_orientation, ==, 0); /* unknown, not an OOB read */
   /* Dimensions are unaffected -- only the Orientation tag was patched. */
   g_assert_cmpint(p_info->i_width, ==, 6);
   g_assert_cmpint(p_info->i_height, ==, 3);

   info_delete(p_info);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
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
   g_test_add_func("/info/malformed_exif_orientation_format",
                   test_malformed_exif_orientation_format);
   g_test_add_func("/info/oversized_jpeg", test_oversized_jpeg);
   g_test_add_func("/info/padded_past_prefix_oversized_jpeg",
                   test_padded_past_prefix_oversized_jpeg);
   return (g_test_run());
}