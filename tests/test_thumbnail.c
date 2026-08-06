/*:*
 * ggaze — thumbnail cache unit test
 *
 * Generates a thumbnail for a fixture, verifies the TMS cache file is written
 * (with Thumb::MTime), and that a second get hits the cache. Uses a temp
 * XDG_CACHE_HOME so the real cache is not polluted. No display needed.
 *
 * Persistence (ix0) is covered by the _marker_ tests below. The older
 * "second get should hit the cache" assertion could not see the ix0 bug at
 * all: a regenerated thumbnail and a cached one are indistinguishable if you
 * only assert that a texture came back. So these tests overwrite the persisted
 * PNG with a 17x9 marker image and assert on the *dimensions* that come back —
 * marker dimensions prove the bytes were read from disk, non-marker
 * dimensions prove a regeneration.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "thumbnail.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "loader/detect.h"

/* Dimensions of the marker PNG the persistence tests plant in the cache. Not
 * a size any real thumbnail of the fixtures could have, so "did this texture
 * come off disk?" is a two-integer comparison. */
#define GGAZE_MARKER_W 17
#define GGAZE_MARKER_H 9

static const char *GGAZE_FX_DIR;
static char       *GGAZE_CACHE_DIR;

static GdkTexture *GGAZE_RESULT;
static GMainLoop  *GGAZE_LOOP;
static GError     *GGAZE_ERR;

static void
_thumb_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   (void)p_data;
   GError *p_err = NULL;
   GGAZE_RESULT  = thumbnail_get_finish(NULL, p_res, &p_err);
   g_assert_no_error(p_err);
   g_main_loop_quit(GGAZE_LOOP);
}

/* Like _thumb_cb, but for tests that expect the load to fail: stores the
 * GError in GGAZE_ERR (caller frees) instead of asserting success. */
static void
_thumb_err_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   (void)p_data;
   GGAZE_RESULT = thumbnail_get_finish(NULL, p_res, &GGAZE_ERR);
   g_main_loop_quit(GGAZE_LOOP);
}

/* Get a thumbnail synchronously (pump a main loop until the callback). */
static GdkTexture *
get_thumb(Thumbnail *p_t, GFile *p_file, int i_size) {
   GGAZE_RESULT = NULL;
   GGAZE_LOOP   = g_main_loop_new(NULL, FALSE);
   thumbnail_get_async(p_t, p_file, i_size, NULL, _thumb_cb, NULL);
   g_main_loop_run(GGAZE_LOOP);
   g_main_loop_unref(GGAZE_LOOP);
   GGAZE_LOOP = NULL;
   return (GGAZE_RESULT);
}

static GFile *
fixture_file(const char *c_name) {
   char  *c_path = g_build_filename(GGAZE_FX_DIR, c_name, NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);
   return (p_file);
}

/* --- persistence helpers ------------------------------------------------- */

/* Copy a fixture to a fresh temp file so a test may freely modify or
 * chmod it (and so it gets its own md5-of-URI cache entry, isolated from the
 * other tests in this binary). Caller unlinks + g_frees the path. */
static char *
_copy_fixture_to_tmp(const char *c_name) {
   char  *c_src = g_build_filename(GGAZE_FX_DIR, c_name, NULL);
   gchar *p_buf = NULL;
   gsize  u_len = 0;
   g_assert_true(g_file_get_contents(c_src, &p_buf, &u_len, NULL));
   g_free(c_src);

   gchar  *c_tmp = NULL;
   GError *p_err = NULL;
   gint    i_fd  = g_file_open_tmp("ggaze-thumb-src-XXXXXX", &c_tmp, &p_err);
   g_assert_no_error(p_err);
   g_assert_cmpint(i_fd, >=, 0);
   g_assert_cmpint((glong)write(i_fd, p_buf, u_len), ==, (glong)u_len);
   close(i_fd);
   g_free(p_buf);
   return (c_tmp);
}

static gint64
_mtime_of(GFile *p_file) {
   GFileInfo *p_info = g_file_query_info(p_file, "time::modified",
                                         G_FILE_QUERY_INFO_NONE, NULL, NULL);
   g_assert_nonnull(p_info);
   gint64 i_mtime = (gint64)g_file_info_get_attribute_uint64(
      p_info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
   g_object_unref(p_info);
   return (i_mtime);
}

/* Overwrite the cache entry at c_path with the marker image, tagged with the
 * given TMS metadata. c_uri/i_mtime are what the module verifies, so passing
 * the true values fakes a valid entry and passing wrong ones fakes a stale or
 * foreign one. Creates the bucket directory itself so a test may plant an
 * entry before ggaze has ever written into that bucket. */
static void
_write_marker(const char *c_path, gint64 i_mtime, const char *c_uri) {
   char *c_dir = g_path_get_dirname(c_path);
   g_assert_cmpint(g_mkdir_with_parents(c_dir, 0700), ==, 0);
   g_free(c_dir);
   GdkPixbuf *p_pix = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
                                     GGAZE_MARKER_W, GGAZE_MARKER_H);
   g_assert_nonnull(p_pix);
   gdk_pixbuf_fill(p_pix, 0xff00ffffu);
   char c_mtime[32];
   g_snprintf(c_mtime, sizeof(c_mtime), "%" G_GINT64_FORMAT, i_mtime);
   GError *p_err = NULL;
   gdk_pixbuf_save(p_pix, c_path, "png", &p_err, "tEXt::Thumb::URI", c_uri,
                   "tEXt::Thumb::MTime", c_mtime, NULL);
   g_assert_no_error(p_err);
   g_object_unref(p_pix);
}

static gboolean
_is_marker(GdkTexture *p_tex) {
   return (gdk_texture_get_width(p_tex) == GGAZE_MARKER_W &&
           gdk_texture_get_height(p_tex) == GGAZE_MARKER_H);
}

/* Thumb::MTime recorded in the cache entry at c_path, or -1 if the file is
 * missing/corrupt/untagged. Reads the "tEXt::"-prefixed key, which is the
 * spelling gdk-pixbuf's PNG loader actually exposes — the whole of ix0. */
static gint64
_cached_mtime(const char *c_path) {
   GdkPixbuf *p_pix = gdk_pixbuf_new_from_file(c_path, NULL);
   if (p_pix == NULL) {
      return (-1);
   }
   const char *c_m     = gdk_pixbuf_get_option(p_pix, "tEXt::Thumb::MTime");
   gint64      i_mtime = (c_m != NULL) ? g_ascii_strtoll(c_m, NULL, 10) : -1;
   g_object_unref(p_pix);
   return (i_mtime);
}

/* One thumbnail request through a Thumbnail instance of its own — the closest
 * in-process stand-in for quitting ggaze and starting it again, since nothing
 * but the on-disk cache survives between calls. */
static GdkTexture *
_get_thumb_fresh(GFile *p_file, int i_size) {
   Thumbnail  *p_t   = thumbnail_new();
   GdkTexture *p_tex = get_thumb(p_t, p_file, i_size);
   thumbnail_delete(p_t);
   return (p_tex);
}

/* --- tests --------------------------------------------------------------- */

static void
test_generate_and_cache(void) {
   Thumbnail *p_t    = thumbnail_new();
   GFile     *p_file = fixture_file("plain.jpg");

   GdkTexture *p_tex = get_thumb(p_t, p_file, 128);
   g_assert_nonnull(p_tex);
   g_assert_cmpint(gdk_texture_get_width(p_tex), <=, 128);
   g_assert_cmpint(gdk_texture_get_height(p_tex), <=, 128);
   g_object_unref(p_tex);

   /* The cache file should exist under our temp XDG_CACHE_HOME. */
   char *c_normal =
      g_build_filename(GGAZE_CACHE_DIR, "thumbnails", "normal", NULL);
   GDir *p_dir = g_dir_open(c_normal, 0, NULL);
   g_assert_nonnull(p_dir);
   gboolean    b_found = FALSE;
   const char *c_name;
   while ((c_name = g_dir_read_name(p_dir)) != NULL) {
      if (g_str_has_suffix(c_name, ".png")) {
         b_found = TRUE;
         break;
      }
   }
   g_dir_close(p_dir);
   g_free(c_normal);
   g_assert_true(b_found);

   /* Second get should hit the cache (same result). */
   GdkTexture *p_tex2 = get_thumb(p_t, p_file, 128);
   g_assert_nonnull(p_tex2);
   g_object_unref(p_tex2);

   thumbnail_delete(p_t);
   g_object_unref(p_file);
}

static void
test_different_bucket(void) {
   Thumbnail *p_t    = thumbnail_new();
   GFile     *p_file = fixture_file("rot6.jpg");

   /* Request 200px → bucket = large (256). */
   GdkTexture *p_tex = get_thumb(p_t, p_file, 200);
   g_assert_nonnull(p_tex);
   g_object_unref(p_tex);

   /* The cache file should be under "large". */
   char *c_large =
      g_build_filename(GGAZE_CACHE_DIR, "thumbnails", "large", NULL);
   GDir *p_dir = g_dir_open(c_large, 0, NULL);
   g_assert_nonnull(p_dir);
   gboolean    b_found = FALSE;
   const char *c_name;
   while ((c_name = g_dir_read_name(p_dir)) != NULL) {
      if (g_str_has_suffix(c_name, ".png")) {
         b_found = TRUE;
         break;
      }
   }
   g_dir_close(p_dir);
   g_free(c_large);
   g_assert_true(b_found);

   thumbnail_delete(p_t);
   g_object_unref(p_file);
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

/* mu0 review round 2: _generate() called gdk_pixbuf_new_from_file_at_scale()
 * directly on the source file, bypassing loader.c's guard entirely --
 * reachable simply by scrolling a directory in grid view past a malicious
 * JPEG (gridview.c's _on_pic_map -> thumbnail_get_async, no click needed).
 * Exercises the REAL public entry point (thumbnail_get_async(), the same
 * call gridview.c makes) with the crafted 65500x65500-SOF0 header, and
 * asserts both a clean error and a tight wall-clock budget: pre-fix this
 * stalled ~28s per file (GdkPixbuf/glycin pre-allocating a huge sparse memfd
 * off the declared size before its own internal cap rejected it). */
static void
test_oversized_jpeg(void) {
   char  *c_src = g_build_filename(GGAZE_FX_DIR, "plain.jpg", NULL);
   gchar *p_buf = NULL;
   gsize  u_len = 0;
   g_assert_true(g_file_get_contents(c_src, &p_buf, &u_len, NULL));
   g_free(c_src);
   _patch_sof_dims_huge((guint8 *)p_buf, u_len);

   gchar  *c_tmp = NULL;
   GError *p_sub = NULL;
   gint i_fd = g_file_open_tmp("ggaze-thumb-oversized-XXXXXX", &c_tmp, &p_sub);
   g_assert_no_error(p_sub);
   g_assert_cmpint(i_fd, >=, 0);
   g_assert_cmpint((glong)write(i_fd, p_buf, u_len), ==, (glong)u_len);
   close(i_fd);
   g_free(p_buf);

   Thumbnail *p_t    = thumbnail_new();
   GFile     *p_file = g_file_new_for_path(c_tmp);

   GGAZE_RESULT   = NULL;
   GGAZE_ERR      = NULL;
   GGAZE_LOOP     = g_main_loop_new(NULL, FALSE);
   gint64 i_start = g_get_monotonic_time();
   thumbnail_get_async(p_t, p_file, 128, NULL, _thumb_err_cb, NULL);
   g_main_loop_run(GGAZE_LOOP);
   g_main_loop_unref(GGAZE_LOOP);
   GGAZE_LOOP     = NULL;
   gdouble d_secs = (g_get_monotonic_time() - i_start) / 1e6;

   g_assert_null(GGAZE_RESULT);
   g_assert_nonnull(GGAZE_ERR);
   g_assert_cmpuint(GGAZE_ERR->domain, ==, (guint)G_IO_ERROR);
   g_error_free(GGAZE_ERR);
   GGAZE_ERR = NULL;
   /* 5s leaves generous headroom above the microsecond-scale header peek
    * while catching a regression back to the ~28s stall long before any CI
    * per-test timeout would. */
   g_assert_cmpfloat(d_secs, <, 5.0);

   thumbnail_delete(p_t);
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
 * scan used to come back as plain "no SOF found" and
 * _thumb_reject_if_oversized_jpeg() treated that the same as "not a JPEG,
 * safe to proceed" -- reintroducing the exact ~28-30s GdkPixbuf stall this
 * task exists to close. Exercises the REAL public entry point
 * (thumbnail_get_async()) and asserts the fixed fail-closed behavior: a fast
 * G_IO_ERROR, not a multi-second stall. */
static void
test_padded_past_prefix_oversized_jpeg(void) {
   char  *c_src = g_build_filename(GGAZE_FX_DIR, "plain.jpg", NULL);
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
      g_file_open_tmp("ggaze-thumb-padded-oversized-XXXXXX", &c_tmp, &p_sub);
   g_assert_no_error(p_sub);
   g_assert_cmpint(i_fd, >=, 0);
   g_assert_cmpint((glong)write(i_fd, p_padded, u_padded_len), ==,
                   (glong)u_padded_len);
   close(i_fd);
   g_free(p_padded);

   Thumbnail *p_t    = thumbnail_new();
   GFile     *p_file = g_file_new_for_path(c_tmp);

   GGAZE_RESULT   = NULL;
   GGAZE_ERR      = NULL;
   GGAZE_LOOP     = g_main_loop_new(NULL, FALSE);
   gint64 i_start = g_get_monotonic_time();
   thumbnail_get_async(p_t, p_file, 128, NULL, _thumb_err_cb, NULL);
   g_main_loop_run(GGAZE_LOOP);
   g_main_loop_unref(GGAZE_LOOP);
   GGAZE_LOOP     = NULL;
   gdouble d_secs = (g_get_monotonic_time() - i_start) / 1e6;

   g_assert_null(GGAZE_RESULT);
   g_assert_nonnull(GGAZE_ERR);
   g_assert_cmpuint(GGAZE_ERR->domain, ==, (guint)G_IO_ERROR);
   g_error_free(GGAZE_ERR);
   GGAZE_ERR = NULL;
   /* Same 5s budget as test_oversized_jpeg(): must fail fast, not stall. */
   g_assert_cmpfloat(d_secs, <, 5.0);

   thumbnail_delete(p_t);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
}

/* ix0, the regression test for the reported bug: "when I re-open ggaze all
 * thumbnails seem to be re-generated from scratch". First request generates
 * and persists; we then replace the persisted PNG with the marker (correct
 * Thumb::MTime + Thumb::URI, so it is a *valid* entry) and request again
 * through a brand-new Thumbnail. Getting the marker's 17x9 back proves the
 * second request read the cache file instead of re-decoding the JPEG. Before
 * the fix this returned a freshly decoded 128px thumbnail, because
 * _load_cached() asked for "Thumb::MTime" while gdk-pixbuf's PNG loader
 * exposes it as "tEXt::Thumb::MTime" — so every entry looked stale forever. */
static void
test_cache_survives_reopen(void) {
   char  *c_tmp  = _copy_fixture_to_tmp("plain.jpg");
   GFile *p_file = g_file_new_for_path(c_tmp);
   char  *c_uri  = g_file_get_uri(p_file);
   char  *c_ent  = thumbnail_cache_path(p_file, 128);

   GdkTexture *p_tex = _get_thumb_fresh(p_file, 128);
   g_assert_nonnull(p_tex);
   g_assert_false(_is_marker(p_tex)); /* first run really decoded */
   g_object_unref(p_tex);
   g_assert_true(g_file_test(c_ent, G_FILE_TEST_EXISTS));
   g_assert_cmpint(_cached_mtime(c_ent), ==, _mtime_of(p_file));

   _write_marker(c_ent, _mtime_of(p_file), c_uri);
   GdkTexture *p_tex2 = _get_thumb_fresh(p_file, 128);
   g_assert_nonnull(p_tex2);
   g_assert_true(_is_marker(p_tex2));
   g_object_unref(p_tex2);

   g_free(c_ent);
   g_free(c_uri);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
}

/* Edge case: the source file changed after the thumbnail was made. The cache
 * key is only md5(URI), so nothing about the path changes — the Thumb::MTime
 * check is the only thing standing between the user and a stale picture.
 * Plant a valid marker entry, then rewrite the source with different content
 * and a later mtime; the next request must ignore the marker and regenerate,
 * and must leave the entry describing the *new* mtime. */
static void
test_stale_entry_when_source_changes(void) {
   char  *c_tmp  = _copy_fixture_to_tmp("plain.jpg");
   GFile *p_file = g_file_new_for_path(c_tmp);
   char  *c_uri  = g_file_get_uri(p_file);
   char  *c_ent  = thumbnail_cache_path(p_file, 128);
   _write_marker(c_ent, _mtime_of(p_file), c_uri);

   /* Replace the bytes, then force a distinctly later mtime: a same-second
    * rewrite could otherwise keep the old timestamp on a coarse clock and make
    * this test pass or fail by luck. */
   char  *c_other = _copy_fixture_to_tmp("rot6.jpg");
   gchar *p_buf   = NULL;
   gsize  u_len   = 0;
   g_assert_true(g_file_get_contents(c_other, &p_buf, &u_len, NULL));
   g_assert_true(g_file_set_contents(c_tmp, p_buf, (gssize)u_len, NULL));
   g_free(p_buf);
   unlink(c_other);
   g_free(c_other);
   GStatBuf st;
   g_assert_cmpint(g_stat(c_tmp, &st), ==, 0);
   struct utimbuf ut = {.actime = st.st_atime, .modtime = st.st_mtime + 120};
   g_assert_cmpint(g_utime(c_tmp, &ut), ==, 0);

   GdkTexture *p_tex = _get_thumb_fresh(p_file, 128);
   g_assert_nonnull(p_tex);
   g_assert_false(_is_marker(p_tex)); /* stale entry was not trusted */
   g_object_unref(p_tex);
   g_assert_cmpint(_cached_mtime(c_ent), ==, _mtime_of(p_file));

   g_free(c_ent);
   g_free(c_uri);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
}

/* Edge case: an entry whose Thumb::URI names a different file. The entry is
 * only reachable through md5(URI), so this means an md5 collision or a
 * foreign writer in the shared ~/.cache/thumbnails — either way, showing it
 * would show the wrong image. Must regenerate. */
static void
test_foreign_uri_entry_rejected(void) {
   char  *c_tmp  = _copy_fixture_to_tmp("plain.jpg");
   GFile *p_file = g_file_new_for_path(c_tmp);
   char  *c_ent  = thumbnail_cache_path(p_file, 128);
   _write_marker(c_ent, _mtime_of(p_file), "file:///nowhere/someone-else.jpg");

   GdkTexture *p_tex = _get_thumb_fresh(p_file, 128);
   g_assert_nonnull(p_tex);
   g_assert_false(_is_marker(p_tex));
   g_object_unref(p_tex);

   g_free(c_ent);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
}

/* Edge case: the persisted entry is not a PNG at all (truncated write, disk
 * corruption, someone else's junk under our name). A cache must never turn a
 * displayable image into an error: the request must still succeed by
 * regenerating, and must repair the entry on the way out. */
static void
test_corrupt_entry_regenerated(void) {
   char  *c_tmp  = _copy_fixture_to_tmp("plain.jpg");
   GFile *p_file = g_file_new_for_path(c_tmp);
   char  *c_ent  = thumbnail_cache_path(p_file, 128);
   char  *c_dir  = g_path_get_dirname(c_ent);
   g_assert_cmpint(g_mkdir_with_parents(c_dir, 0700), ==, 0);
   g_free(c_dir);
   g_assert_true(g_file_set_contents(c_ent, "not a png at all", -1, NULL));
   g_assert_cmpint(_cached_mtime(c_ent), ==, -1);

   GdkTexture *p_tex = _get_thumb_fresh(p_file, 128);
   g_assert_nonnull(p_tex);
   g_assert_cmpint(gdk_texture_get_width(p_tex), <=, 128);
   g_object_unref(p_tex);
   g_assert_cmpint(_cached_mtime(c_ent), ==, _mtime_of(p_file));

   g_free(c_ent);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
}

/* Edge case: the entry exists and is valid but cannot be opened (mode 0000),
 * and the rewrite that follows cannot replace it either. The request must
 * still hand back a texture — the on-disk cache is an optimisation, never a
 * dependency. Skipped as root, where the mode bits do not bite. */
static void
test_unreadable_entry_regenerated(void) {
   if (geteuid() == 0) {
      g_test_skip("running as root: file modes are not enforced");
      return;
   }
   char  *c_tmp  = _copy_fixture_to_tmp("plain.jpg");
   GFile *p_file = g_file_new_for_path(c_tmp);
   char  *c_uri  = g_file_get_uri(p_file);
   char  *c_ent  = thumbnail_cache_path(p_file, 128);
   _write_marker(c_ent, _mtime_of(p_file), c_uri);
   g_assert_cmpint(g_chmod(c_ent, 0000), ==, 0);

   GdkTexture *p_tex = _get_thumb_fresh(p_file, 128);
   g_assert_nonnull(p_tex);
   g_assert_false(_is_marker(p_tex)); /* unreadable entry, so a real decode */
   g_object_unref(p_tex);

   g_assert_cmpint(g_chmod(c_ent, 0600), ==, 0);
   unlink(c_ent);
   g_free(c_ent);
   g_free(c_uri);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
}

/* Remove <cache>/thumbnails/<c_sub> and everything in it, so a test can then
 * make its creation fail. Returns the removed directory's path. */
static char *
_wipe_bucket_dir(const char *c_sub) {
   char *c_dir = g_build_filename(GGAZE_CACHE_DIR, "thumbnails", c_sub, NULL);
   GDir *p_dir = g_dir_open(c_dir, 0, NULL);
   if (p_dir != NULL) {
      const char *c_name;
      while ((c_name = g_dir_read_name(p_dir)) != NULL) {
         char *c_child = g_build_filename(c_dir, c_name, NULL);
         unlink(c_child);
         g_free(c_child);
      }
      g_dir_close(p_dir);
      g_rmdir(c_dir);
   }
   return (c_dir);
}

/* Edge case: the cache directory cannot even be created (here: a read-only
 * parent; in the field a read-only or full $XDG_CACHE_HOME, or a file in the
 * way). Thumbnails must still be produced and displayed, just not persisted.
 * Skipped as root, where write permission is not enforced. */
static void
test_cache_dir_not_creatable(void) {
   if (geteuid() == 0) {
      g_test_skip("running as root: directory modes are not enforced");
      return;
   }
   char *c_bucket = _wipe_bucket_dir("x-large"); /* the 512px bucket */
   char *c_parent = g_build_filename(GGAZE_CACHE_DIR, "thumbnails", NULL);
   g_assert_cmpint(g_mkdir_with_parents(c_parent, 0700), ==, 0);
   g_assert_cmpint(g_chmod(c_parent, 0500), ==, 0);

   char  *c_tmp  = _copy_fixture_to_tmp("plain.jpg");
   GFile *p_file = g_file_new_for_path(c_tmp);

   GdkTexture *p_tex = _get_thumb_fresh(p_file, 512);
   g_assert_nonnull(p_tex);
   g_assert_false(g_file_test(c_bucket, G_FILE_TEST_EXISTS));
   g_object_unref(p_tex);

   g_assert_cmpint(g_chmod(c_parent, 0700), ==, 0);
   g_free(c_parent);
   g_free(c_bucket);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);

   GGAZE_FX_DIR = g_getenv("GGAZE_FIXTURES_DIR");
   if (GGAZE_FX_DIR == NULL) {
      g_test_skip("GGAZE_FIXTURES_DIR unset");
      return (g_test_run());
   }

   /* Use a temp XDG_CACHE_HOME so the real cache is not polluted. */
   GError *p_err   = NULL;
   GGAZE_CACHE_DIR = g_dir_make_tmp("ggaze-thumb-cache-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   g_setenv("XDG_CACHE_HOME", GGAZE_CACHE_DIR, TRUE);

   g_test_add_func("/thumbnail/generate_and_cache", test_generate_and_cache);
   g_test_add_func("/thumbnail/different_bucket", test_different_bucket);
   g_test_add_func("/thumbnail/cache_survives_reopen",
                   test_cache_survives_reopen);
   g_test_add_func("/thumbnail/stale_entry_when_source_changes",
                   test_stale_entry_when_source_changes);
   g_test_add_func("/thumbnail/foreign_uri_entry_rejected",
                   test_foreign_uri_entry_rejected);
   g_test_add_func("/thumbnail/corrupt_entry_regenerated",
                   test_corrupt_entry_regenerated);
   g_test_add_func("/thumbnail/unreadable_entry_regenerated",
                   test_unreadable_entry_regenerated);
   g_test_add_func("/thumbnail/cache_dir_not_creatable",
                   test_cache_dir_not_creatable);
   g_test_add_func("/thumbnail/oversized_jpeg", test_oversized_jpeg);
   g_test_add_func("/thumbnail/padded_past_prefix_oversized_jpeg",
                   test_padded_past_prefix_oversized_jpeg);

   int i_ret = g_test_run();

   /* Cleanup the temp cache dir (best-effort recursive). */
   GFile           *p_cd = g_file_new_for_path(GGAZE_CACHE_DIR);
   GFileEnumerator *p_e =
      g_file_enumerate_children(p_cd, "standard::name,standard::type",
                                G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (p_e != NULL) {
      GFileInfo *p_info;
      while ((p_info = g_file_enumerator_next_file(p_e, NULL, NULL)) != NULL) {
         GFile *p_child = g_file_get_child(p_cd, g_file_info_get_name(p_info));
         if (g_file_info_get_file_type(p_info) == G_FILE_TYPE_DIRECTORY) {
            GFileEnumerator *p_e2 = g_file_enumerate_children(
               p_child, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (p_e2 != NULL) {
               GFileInfo *p_i2;
               while ((p_i2 = g_file_enumerator_next_file(p_e2, NULL, NULL)) !=
                      NULL) {
                  GFile *p_c2 =
                     g_file_get_child(p_child, g_file_info_get_name(p_i2));
                  g_file_delete(p_c2, NULL, NULL);
                  g_object_unref(p_c2);
                  g_object_unref(p_i2);
               }
               g_object_unref(p_e2);
            }
         }
         g_file_delete(p_child, NULL, NULL);
         g_object_unref(p_child);
         g_object_unref(p_info);
      }
      g_object_unref(p_e);
   }
   g_file_delete(p_cd, NULL, NULL);
   g_object_unref(p_cd);
   g_free(GGAZE_CACHE_DIR);
   return (i_ret);
}