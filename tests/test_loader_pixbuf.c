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
 * test_oversized_jpeg (mu0 review) exercises loader_load() -- the real
 * public dispatch entry point used by clipboard.c and any prefetch without
 * a progress callback -- with a JPEG whose SOF0 declares 65500x65500. Pre-
 * fix, this drove pixbuf.c's GdkPixbufLoader into a ~28s stall (glycin
 * pre-allocating a huge sparse memfd off the declared size before its own
 * internal cap rejected it); it never crashed, but it never returned
 * quickly either. The test asserts both a clean G_IO_ERROR *and* a tight
 * wall-clock budget, so a regression of the pre-decode guard shows up as a
 * failing assertion rather than a merely-slow-but-passing test.
 *
 * test_truncated_signatures and friends (task tb2) do the same for the
 * other stall: a file that carries a recognised container signature but is
 * shorter than the smallest complete file of that format. On a glycin
 * desktop gdk-pixbuf hands such a file to a sandboxed loader subprocess and
 * the JXL one waits forever (the old 4-byte /unsupported_jxl vector hung
 * this suite for minutes), so every entry point -- loader_load(),
 * loader_load_pixbuf_scaled(), loader_peek_dimensions() -- must refuse it
 * with G_IO_ERROR_INVALID_DATA before gdk-pixbuf sees it, within a budget.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "loader/loader.h"
#include "loader/pixbuf-util.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <string.h>
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

/* Write raw bytes to a unique temp file; caller unlinks and g_frees the
 * returned path. */
static gchar *
write_tmp(const guint8 *p_buf, gsize u_len) {
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
   return (c_path);
}

/* Write raw bytes to a temp file and load them (magic-byte / corrupt cases). */
static GdkTexture *
load_bytes(const guint8 *p_buf, gsize u_len, GError **p_err) {
   gchar      *c_path = write_tmp(p_buf, u_len);
   GFile      *p_file = g_file_new_for_path(c_path);
   GdkTexture *p_tex  = loader_load(p_file, NULL, p_err);
   g_object_unref(p_file);
   unlink(c_path);
   g_free(c_path);
   return (p_tex);
}

/* A signature-only header must fail with a G_IO_ERROR whatever is built in:
 * these vectors are all shorter than their format's minimum complete file,
 * so the dispatcher's truncation gate rejects them (INVALID_DATA) before
 * any backend -- built-in or gdk-pixbuf/glycin -- is consulted. */
static void
assert_unsupported(const guint8 *p_buf, gsize u_len) {
   GError     *p_err = NULL;
   GdkTexture *p_tex = load_bytes(p_buf, u_len, &p_err);
   g_assert_null(p_tex);
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
   g_error_free(p_err);
}

static void
test_unsupported_jxl(void) {
   /* JXL codestream magic. Pre-fix this exact vector reached
    * GdkPixbufLoader and, on a glycin desktop, never returned (task tb2). */
   const guint8 h[] = {0xFF, 0x0A, 0x10, 0x00};
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
   /* Corrupt JPEG: SOI + APP0 marker, then zeros instead of image data.
    * Padded to 64 bytes so it clears the truncation gate (25 bytes for a
    * JPEG) and it is really GdkPixbuf that produces the error. */
   guint8      h[64] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F',
                        'I',  'F',  0,    1,    1,    0,    0,   0};
   GError     *p_err = NULL;
   GdkTexture *p_tex = load_bytes(h, G_N_ELEMENTS(h), &p_err);
   g_assert_null(p_tex);
   g_assert_nonnull(p_err);
   g_assert_false(g_error_matches(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA));
   g_error_free(p_err);
}

/* One truncated signature per format the sniffer knows (the same vectors
 * tests/test_detect.c checks against detect_min_file_len()). Each must be
 * refused as INVALID_DATA within a budget: pre-fix the JXL ones hung for
 * minutes on a glycin desktop, and the 5 s bound mirrors
 * test_oversized_jpeg's so a regression fails rather than merely slows. */
typedef struct {
   const char *c_name;
   guint8      buf[12];
   gsize       u_len;
} TruncatedVec;

static const TruncatedVec TRUNCATED[] = {
   {"jxl codestream 2B", {0xFF, 0x0A}, 2},
   {"jxl container",
    {0, 0, 0, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A},
    12},
   {"webp", {'R', 'I', 'F', 'F', 0x10, 0, 0, 0, 'W', 'E', 'B', 'P'}, 12},
   {"png", {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}, 8},
   {"gif", {'G', 'I', 'F', '8', '9', 'a'}, 6},
   {"tiff", {'I', 'I', 0x2A, 0x00}, 4},
   {"ico", {0x00, 0x00, 0x01, 0x00}, 4},
   {"jpeg", {0xFF, 0xD8, 0xFF}, 3},
};

static void
test_truncated_signatures(void) {
   for (gsize u = 0; u < G_N_ELEMENTS(TRUNCATED); u++) {
      g_test_message("%s", TRUNCATED[u].c_name);
      gint64 i_start = g_get_monotonic_time();
      assert_unsupported(TRUNCATED[u].buf, TRUNCATED[u].u_len);
      gdouble d_secs = (g_get_monotonic_time() - i_start) / 1e6;
      g_assert_cmpfloat(d_secs, <, 5.0);
   }
}

/* A single byte carries no signature, so the gate imposes nothing and
 * gdk-pixbuf reports its own "unrecognised" error -- promptly. */
static void
test_one_byte_file(void) {
   const guint8 h[]     = {0xFF};
   gint64       i_start = g_get_monotonic_time();
   GError      *p_err   = NULL;
   GdkTexture  *p_tex   = load_bytes(h, 1, &p_err);
   gdouble      d_secs  = (g_get_monotonic_time() - i_start) / 1e6;
   g_assert_null(p_tex);
   g_assert_nonnull(p_err);
   g_error_free(p_err);
   g_assert_cmpfloat(d_secs, <, 5.0);
}

/* The thumbnail (loader_load_pixbuf_scaled) and info (loader_peek_
 * dimensions) entry points hand a PATH to gdk-pixbuf, so they need the
 * same gate as the full load or a truncated JXL stalls the thumbnail pool
 * and the info worker instead of the large view. */
static void
test_truncated_thumbnail_and_peek(void) {
   const guint8 h[]     = {0xFF, 0x0A, 0x10, 0x00};
   gchar       *c_path  = write_tmp(h, G_N_ELEMENTS(h));
   GFile       *p_file  = g_file_new_for_path(c_path);
   gint64       i_start = g_get_monotonic_time();
   GError      *p_err   = NULL;
   GdkPixbuf   *p_pix   = loader_load_pixbuf_scaled(p_file, 128, NULL, &p_err);
   g_assert_null(p_pix);
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
   g_error_free(p_err);
   int i_w = -1, i_h = -1;
   g_assert_false(loader_peek_dimensions(p_file, &i_w, &i_h));
   gdouble d_secs = (g_get_monotonic_time() - i_start) / 1e6;
   g_assert_cmpfloat(d_secs, <, 5.0);
   g_object_unref(p_file);
   unlink(c_path);
   g_free(c_path);
}

/* The gate must not turn away real files on the path-based entry points:
 * a fixture PNG still thumbnails and still reports its stored size. */
static void
test_thumbnail_and_peek_still_work(void) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar     *c_path = g_build_filename(c_dir, "small.png", NULL);
   GFile     *p_file = g_file_new_for_path(c_path);
   GError    *p_err  = NULL;
   GdkPixbuf *p_pix  = loader_load_pixbuf_scaled(p_file, 128, NULL, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_pix);
   /* The at-scale path fits the box (it scales a tiny image UP to it), so
    * assert the fit and the kept 5:2 aspect rather than the source size. */
   g_assert_cmpint(gdk_pixbuf_get_width(p_pix), <=, 128);
   g_assert_cmpint(gdk_pixbuf_get_height(p_pix), <=, 128);
   g_assert_cmpint(gdk_pixbuf_get_width(p_pix), >,
                   gdk_pixbuf_get_height(p_pix));
   g_object_unref(p_pix);
   int i_w = 0, i_h = 0;
   g_assert_true(loader_peek_dimensions(p_file, &i_w, &i_h));
   g_assert_cmpint(i_w, ==, 5);
   g_assert_cmpint(i_h, ==, 2);
   g_object_unref(p_file);
   g_free(c_path);
}

/* Locate the baseline SOF0 marker (0xFF 0xC0) in a JPEG byte buffer and
 * overwrite its declared height/width with 65500 (0xFFDC), the largest
 * value libjpeg's own JPEG_MAX_DIMENSION check still accepts at header-read
 * time. Mirrors tests/test_loader_jpeg.c's _patch_sof_dims_huge and
 * tests/test_detect.c's local copy (kept separate per test binary rather
 * than shared, matching this suite's existing helper-per-file convention). */
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
test_oversized_jpeg(void) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   guint8 *p_buf  = NULL;
   gsize   u_len  = 0;
   g_assert_true(g_file_get_contents(c_path, (gchar **)&p_buf, &u_len, NULL));
   g_free(c_path);
   _patch_sof_dims_huge(p_buf, u_len);

   gint64      i_start = g_get_monotonic_time();
   GError     *p_err   = NULL;
   GdkTexture *p_tex   = load_bytes(p_buf, u_len, &p_err);
   gdouble     d_secs  = (g_get_monotonic_time() - i_start) / 1e6;
   g_free(p_buf);

   g_assert_null(p_tex);
   g_assert_nonnull(p_err);
   g_assert_cmpuint(p_err->domain, ==, (guint)G_IO_ERROR);
   g_error_free(p_err);
   /* Pre-fix this call observed a ~28s stall; 5s leaves generous headroom
    * above the microsecond-scale header peek while still catching a
    * regression back to the GdkPixbuf-driven stall long before any CI
    * per-test timeout would. */
   g_assert_cmpfloat(d_secs, <, 5.0);
}

static void
test_rgba_png(void) {
   /* 5x2 RGBA PNG -> exercises the has-alpha branch of texture_from_pixbuf. */
   GdkTexture *p_tex = load_fixture("rgba.png");
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 5);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, 2);
   g_object_unref(p_tex);
}

/* Regression: an empty file used to make loader_load() return NULL with NO
 * GError (the sync path tested the error out-pointer instead of an error),
 * which left downstream GTasks incomplete. It must be a real error now. */
static void
test_empty_file_sets_error(void) {
   GError     *p_err = NULL;
   GdkTexture *p_tex = load_bytes((const guint8 *)"", 0, &p_err);
   g_assert_null(p_tex);
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
   g_error_free(p_err);
}

/* A load whose cancellable is already cancelled must fail with
 * G_IO_ERROR_CANCELLED before decoding anything. */
static void
test_cancelled_before_decode(void) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar        *c_path   = g_build_filename(c_dir, "small.png", NULL);
   GFile        *p_file   = g_file_new_for_path(c_path);
   GCancellable *p_cancel = g_cancellable_new();
   g_cancellable_cancel(p_cancel);
   GError     *p_err = NULL;
   GdkTexture *p_tex = loader_load(p_file, p_cancel, &p_err);
   g_assert_null(p_tex);
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
   g_error_free(p_err);
   g_object_unref(p_cancel);
   g_object_unref(p_file);
   g_free(c_path);
}

/* pixbuf_util_to_texture adds alpha to an RGB pixbuf and keeps an RGBA one;
 * pixbuf_util_upright returns a new ref for a pixbuf without orientation. */
static void
test_pixbuf_util(void) {
   GdkPixbuf *p_rgb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 3, 2);
   gdk_pixbuf_fill(p_rgb, 0x10203000);
   GdkTexture *p_tex = pixbuf_util_to_texture(p_rgb);
   g_assert_nonnull(p_tex);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 3);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, 2);
   /* gdk_texture_download yields premultiplied B8G8R8A8. */
   guchar px[3 * 2 * 4];
   gdk_texture_download(p_tex, px, 3 * 4);
   g_assert_cmpuint(px[2], ==, 0x10); /* R */
   g_assert_cmpuint(px[0], ==, 0x30); /* B */
   g_assert_cmpuint(px[3], ==, 0xff); /* alpha forced opaque */
   g_object_unref(p_tex);

   GdkPixbuf *p_up = pixbuf_util_upright(p_rgb);
   g_assert_nonnull(p_up);
   g_assert_cmpint(gdk_pixbuf_get_width(p_up), ==, 3);
   g_object_unref(p_up);
   g_object_unref(p_rgb);

   GdkPixbuf *p_rgba = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 2, 2);
   gdk_pixbuf_fill(p_rgba, 0x11223380);
   p_tex = pixbuf_util_to_upright_texture(p_rgba);
   g_assert_nonnull(p_tex);
   gdk_texture_download(p_tex, px, 2 * 4);
   g_assert_cmpuint(px[3], ==, 0x80);
   g_object_unref(p_tex);
   g_object_unref(p_rgba);
}

/* loader_peek_dimensions() says "unknown" for a non-local GFile (no path to
 * hand gdk-pixbuf; decided before any I/O) and for a local file that clears
 * the truncation gate but that gdk-pixbuf cannot parse (64 bytes of text
 * carry no signature, so the gate imposes nothing and gdk-pixbuf's header
 * parse is what fails). */
static void
test_peek_dimensions_unknown_cases(void) {
   int    i_w = -1, i_h = -1;
   GFile *p_remote = g_file_new_for_uri("http://localhost.invalid/x.png");
   g_assert_false(loader_peek_dimensions(p_remote, &i_w, &i_h));
   g_object_unref(p_remote);

   guint8 text[64];
   memset(text, 'x', sizeof(text));
   gchar *c_path = write_tmp(text, sizeof(text));
   GFile *p_file = g_file_new_for_path(c_path);
   g_assert_false(loader_peek_dimensions(p_file, &i_w, &i_h));
   g_object_unref(p_file);
   unlink(c_path);
   g_free(c_path);
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
   g_test_add_func("/loader/pixbuf/truncated_signatures",
                   test_truncated_signatures);
   g_test_add_func("/loader/pixbuf/one_byte_file", test_one_byte_file);
   g_test_add_func("/loader/pixbuf/truncated_thumbnail_and_peek",
                   test_truncated_thumbnail_and_peek);
   g_test_add_func("/loader/pixbuf/thumbnail_and_peek_still_work",
                   test_thumbnail_and_peek_still_work);
   g_test_add_func("/loader/pixbuf/peek_dimensions_unknown_cases",
                   test_peek_dimensions_unknown_cases);
   g_test_add_func("/loader/pixbuf/oversized_jpeg", test_oversized_jpeg);
   g_test_add_func("/loader/pixbuf/rgba_png", test_rgba_png);
   g_test_add_func("/loader/pixbuf/empty_file_sets_error",
                   test_empty_file_sets_error);
   g_test_add_func("/loader/pixbuf/cancelled_before_decode",
                   test_cancelled_before_decode);
   g_test_add_func("/loader/pixbuf/pixbuf_util", test_pixbuf_util);
   return (g_test_run());
}