/*:*
 * ggaze — thumbnail cache unit test
 *
 * Generates a thumbnail for a fixture, verifies the TMS cache file is written
 * (with Thumb::MTime), and that a second get hits the cache. Uses a temp
 * XDG_CACHE_HOME so the real cache is not polluted. No display needed.
 *
 * The truncated-JXL / empty / garbage-JXL cases at the end prove the loader's
 * decode gate (task tb2) sits in front of this public entry point too: the
 * pool is a GTask worker that cannot be cancelled once glycin has the file.
 * The _entry_ variants plant the same bytes as a CACHE entry: the shared
 * ~/.cache/thumbnails is written by every TMS app, so the cache read needs
 * the gate as much as the source decode does, only a PNG may be decoded
 * from it at all, and its size is bounded (test_oversize_entry_regenerated
 * pins GGAZE_THUMB_ENTRY_MAX_BYTES from both sides). The cancellation pair
 * at the end proves a pre-cancelled request opens neither the entry nor the
 * source (inotify, test_precancelled_request) and that the task's
 * GCancellable reaches the cache read itself (test_cancel_mid_entry_read
 * parks the worker inside it, on a FIFO).
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

#include <errno.h>
#include <fcntl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "ggaze-config.h"
#include "loader/detect.h"
#include "open_counter.h"
#include "tiny_images.h"
#include "wait_until.h"

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
   GGAZE_RESULT  = thumbnail_get_finish(p_res, &p_err);
   g_assert_no_error(p_err);
   g_main_loop_quit(GGAZE_LOOP);
}

/* Like _thumb_cb, but for tests that expect the load to fail: stores the
 * GError in GGAZE_ERR (caller frees) instead of asserting success. */
static void
_thumb_err_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   (void)p_data;
   GGAZE_RESULT = thumbnail_get_finish(p_res, &GGAZE_ERR);
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

/* Write p_buf/u_len to a fresh temp file named after c_prefix; caller
 * unlinks and g_frees the path. The three crafted-file tests and the gate
 * helper all need exactly this. */
static gchar *
_write_tmp_bytes(const char *c_prefix, const guint8 *p_buf, gsize u_len) {
   gchar  *c_tmp = NULL;
   GError *p_sub = NULL;
   gint    i_fd  = g_file_open_tmp(c_prefix, &c_tmp, &p_sub);
   g_assert_no_error(p_sub);
   g_assert_cmpint(i_fd, >=, 0);
   g_assert_cmpint((glong)write(i_fd, p_buf, u_len), ==, (glong)u_len);
   close(i_fd);
   return (c_tmp);
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
   gchar *c_tmp =
      _write_tmp_bytes("ggaze-thumb-oversized-XXXXXX", (guint8 *)p_buf, u_len);
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
   gchar *c_tmp = _write_tmp_bytes("ggaze-thumb-padded-oversized-XXXXXX",
                                   p_padded, u_padded_len);
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

/* Regression: thumbnail_delete() with requests still queued used to discard
 * the queued GTasks without completing them, leaking each one (and the refs
 * its callback data carried). Every request must now finish -- as a texture
 * or as G_IO_ERROR_CANCELLED -- so the owners' callbacks run and release.
 *
 * How long that takes is not a property of the test: thumbnail_delete()
 * returns at once, and every request -- the ones the pool's workers (up
 * to 4) had already taken, NULL cancellables so they decode to the end,
 * and the ones still queued, which the workers answer CANCELLED after the
 * owner is gone -- completes on a pool thread whenever that thread gets
 * the CPU. So the test waits for the COUNT, under a budget that only says
 * how long it is willing to wait before calling it a bug. It used to be
 * 2000 iterations of a 1 ms sleep, ~2 s however starved the pool was, and
 * on a parallel --repeat lane at load average ~10 that read 23 == 24
 * (hd2). The bounded wait told the two readings apart: with the budget at
 * 20 s the count still stuck at 23 of 24 (4 of 300 runs on 8 loaded
 * cores), so it was a lost completion in thumbnail.c and not a slow lane
 * -- g_thread_pool_free(immediate=TRUE) let a worker that woke late drop
 * the request it had popped (see _thumb_pool_func in thumbnail.c). 20 s
 * (scaled for sanitizer lanes and GGAZE_TEST_TIMEOUT_SCALE) is two orders
 * above the unloaded time and under the suite's meson timeout, so a
 * regression fails the named assertion below, not the harness. */
#define GGAZE_QUEUE_DRAIN_BUDGET_US (20 * G_USEC_PER_SEC)

static guint GGAZE_DONE_COUNT;

static void
_count_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   (void)p_data;
   GError     *p_err = NULL;
   GdkTexture *p_tex = thumbnail_get_finish(p_res, &p_err);
   if (p_tex != NULL) {
      g_object_unref(p_tex);
   } else {
      g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_error_free(p_err);
   }
   GGAZE_DONE_COUNT++;
}

/* GgtestCondFn: every one of the p_data (a guint) requests has completed.
 * ">=" and not "==" so an over-completion (a callback run twice) does not
 * make the wait burn its whole budget before the "==" assertion names it. */
static gboolean
_all_requests_done(gpointer p_data) {
   return (GGAZE_DONE_COUNT >= GPOINTER_TO_UINT(p_data));
}

static void
test_delete_completes_queued_requests(void) {
   Thumbnail  *p_t  = thumbnail_new();
   GFile      *p_a  = fixture_file("plain.jpg");
   GFile      *p_b  = fixture_file("rot6.jpg");
   const guint u_n  = 24;
   GGAZE_DONE_COUNT = 0;
   for (guint u = 0; u < u_n; u++) {
      thumbnail_get_async(p_t, (u % 2 == 0) ? p_a : p_b, 128, NULL,
                          _count_done_cb, NULL);
   }
   thumbnail_delete(p_t); /* queue still holds most of the requests */
   if (!ggtest_wait_until(_all_requests_done, GUINT_TO_POINTER(u_n),
                          GGAZE_QUEUE_DRAIN_BUDGET_US)) {
      g_error("thumbnail_delete(): only %u of %u queued requests completed "
              "within %d s (scale x%g)",
              GGAZE_DONE_COUNT, u_n,
              (int)(GGAZE_QUEUE_DRAIN_BUDGET_US / G_USEC_PER_SEC),
              ggtest_wait_scale());
   }
   g_assert_cmpuint(GGAZE_DONE_COUNT, ==, u_n);
   g_object_unref(p_a);
   g_object_unref(p_b);
}

/* Run thumbnail_get_async() on a temp file holding p_buf/u_len and expect
 * it to FAIL: asserts no texture, a G_IO_ERROR within the 5 s budget, and
 * returns the error code (the error itself is freed). The gate lives in
 * the loader, but the thumbnail pool is a public entry point of its own
 * (a GTask worker with no cancel once the decode starts), so it gets its
 * own proof that the gate is in front of it (task tb2). */
static gint
_thumb_error_code_fast(const guint8 *p_buf, gsize u_len) {
   gchar     *c_tmp = _write_tmp_bytes("ggaze-thumb-gate-XXXXXX", p_buf, u_len);
   Thumbnail *p_t   = thumbnail_new();
   GFile     *p_file = g_file_new_for_path(c_tmp);
   GGAZE_RESULT      = NULL;
   GGAZE_ERR         = NULL;
   GGAZE_LOOP        = g_main_loop_new(NULL, FALSE);
   gint64 i_start    = g_get_monotonic_time();
   thumbnail_get_async(p_t, p_file, 128, NULL, _thumb_err_cb, NULL);
   g_main_loop_run(GGAZE_LOOP);
   g_main_loop_unref(GGAZE_LOOP);
   GGAZE_LOOP     = NULL;
   gdouble d_secs = (g_get_monotonic_time() - i_start) / 1e6;

   g_assert_null(GGAZE_RESULT);
   g_assert_nonnull(GGAZE_ERR);
   g_assert_cmpuint(GGAZE_ERR->domain, ==, (guint)G_IO_ERROR);
   gint i_code = GGAZE_ERR->code;
   g_error_free(GGAZE_ERR);
   GGAZE_ERR = NULL;
   g_assert_cmpfloat(d_secs, <, 5.0);

   thumbnail_delete(p_t);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
   return (i_code);
}

/* A 4-byte JXL signature: pre-fix this exact file went from the thumbnail
 * pool straight into gdk_pixbuf_new_from_file_at_scale() and, on a glycin
 * desktop, never came back. The length gate refuses it in every build. */
static void
test_truncated_jxl_fails_fast(void) {
   const guint8 h[] = {0xFF, 0x0A, 0x10, 0x00};
   g_assert_cmpint(_thumb_error_code_fast(h, G_N_ELEMENTS(h)), ==,
                   G_IO_ERROR_INVALID_DATA);
}

/* An empty file is refused by the same sniff, before any decoder. */
static void
test_empty_file_fails_fast(void) {
   g_assert_cmpint(_thumb_error_code_fast((const guint8 *)"", 0), ==,
                   G_IO_ERROR_INVALID_DATA);
}

/* A JXL long enough to clear the gate but garbage: without libjxl the
 * loader refuses every JXL (NOT_SUPPORTED) rather than let glycin-jxl hang
 * the pool; with libjxl the backend fails it promptly on its own. */
static void
test_garbage_jxl_fails_fast(void) {
   guint8 h[60]  = {0xFF, 0x0A};
   gint   i_code = _thumb_error_code_fast(h, sizeof(h));
   g_assert_cmpint(i_code, !=, G_IO_ERROR_INVALID_DATA);
#if !GGAZE_HAVE_JXL
   g_assert_cmpint(i_code, ==, G_IO_ERROR_NOT_SUPPORTED);
#endif
}

/* Plant p_buf/u_len as the cache entry for p_file's 128 px bucket (creating
 * the bucket directory), the way a foreign or torn writer would leave it.
 * Deliberately NOT inspected with _cached_mtime() afterwards: that helper
 * hands the path to gdk-pixbuf itself, which is exactly what these bytes
 * must never reach. Returns the entry path (caller frees). */
static char *
_plant_entry(GFile *p_file, const guint8 *p_buf, gsize u_len) {
   char *c_ent = thumbnail_cache_path(p_file, 128);
   char *c_dir = g_path_get_dirname(c_ent);
   g_assert_cmpint(g_mkdir_with_parents(c_dir, 0700), ==, 0);
   g_free(c_dir);
   g_assert_true(
      g_file_set_contents(c_ent, (const gchar *)p_buf, (gssize)u_len, NULL));
   return (c_ent);
}

/* TRUE iff the file at c_path starts with the 8-byte PNG signature. */
static gboolean
_starts_with_png_signature(const char *c_path) {
   static const guint8 sig[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
   gchar              *c_buf = NULL;
   gsize               u_len = 0;
   if (!g_file_get_contents(c_path, &c_buf, &u_len, NULL)) {
      return (FALSE);
   }
   gboolean b_png =
      (u_len >= sizeof(sig) && memcmp(c_buf, sig, sizeof(sig)) == 0);
   g_free(c_buf);
   return (b_png);
}

/* A cache entry holding p_buf/u_len must be REGENERATED, fast: the request
 * still yields a real (non-marker) thumbnail of the source within the 5 s
 * budget, and the entry afterwards describes the source again. Pre-fix
 * _load_cached() called gdk_pixbuf_new_from_file() on the entry with no
 * gate, so a foreign corrupt entry sniffing as JXL reached glycin from the
 * pool worker and hung it (task tb2). The rewritten entry is checked for
 * the PNG signature BEFORE _cached_mtime() hands its path to gdk-pixbuf:
 * were the entry not rewritten (the planted bytes still there), that call
 * would be exactly the glycin hang this test exists to catch, and a
 * regression must fail an assertion, not the meson timeout. */
static void
_assert_entry_regenerated_fast(const guint8 *p_buf, gsize u_len) {
   char  *c_tmp  = _copy_fixture_to_tmp("plain.jpg");
   GFile *p_file = g_file_new_for_path(c_tmp);
   char  *c_ent  = _plant_entry(p_file, p_buf, u_len);

   gint64      i_start = g_get_monotonic_time();
   GdkTexture *p_tex   = _get_thumb_fresh(p_file, 128);
   gdouble     d_secs  = (g_get_monotonic_time() - i_start) / 1e6;
   g_assert_cmpfloat(d_secs, <, 5.0);
   g_assert_nonnull(p_tex);
   g_assert_false(_is_marker(p_tex));
   g_assert_cmpint(gdk_texture_get_width(p_tex), <=, 128);
   g_object_unref(p_tex);
   g_assert_true(_starts_with_png_signature(c_ent));
   g_assert_cmpint(_cached_mtime(c_ent), ==, _mtime_of(p_file));

   g_free(c_ent);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
}

/* An entry that is a 4-byte JXL signature: shorter than any JXL, refused by
 * the length gate before gdk-pixbuf sees the path. */
static void
test_truncated_jxl_entry_regenerated(void) {
   const guint8 h[] = {0xFF, 0x0A, 0x10, 0x00};
   _assert_entry_regenerated_fast(h, G_N_ELEMENTS(h));
}

/* An entry that is a JXL long enough to clear the length gate but garbage:
 * with libjxl built in the gate alone would let it through to gdk-pixbuf,
 * which is why the cache read decodes PNG and nothing else. */
static void
test_garbage_jxl_entry_regenerated(void) {
   guint8 h[60] = {0xFF, 0x0A};
   _assert_entry_regenerated_fast(h, sizeof(h));
}

/* An entry that is a valid file of the wrong format (the 43-byte GIF from
 * tiny_images.h): a TMS entry is a PNG by spec, so even a decodable
 * non-PNG is treated as junk and regenerated rather than shown. */
static void
test_non_png_entry_regenerated(void) {
   _assert_entry_regenerated_fast(TINY_GIF, sizeof(TINY_GIF));
}

/* Extend the file at c_path to u_total bytes with a hole (ftruncate(2):
 * the new tail reads as zeros and occupies no disk), so a 16 MiB decoy
 * costs the test nothing. */
static void
_extend_sparse(const char *c_path, gsize u_total) {
   int i_fd = open(c_path, O_WRONLY);
   g_assert_cmpint(i_fd, >=, 0);
   g_assert_cmpint(ftruncate(i_fd, (off_t)u_total), ==, 0);
   close(i_fd);
}

/* One thumbnail request against a cache entry that is the marker PNG
 * padded with trailing zeros -- sparse, via _extend_sparse(), so the test
 * costs no disk -- to u_total bytes. Returns the texture within the budget; the
 * caller decides whether marker or regenerated is the right answer.
 * gdk-pixbuf's PNG loader ignores bytes after IEND, so the padding by
 * itself never spoils the marker: only the size cap can. */
static GdkTexture *
_thumb_of_padded_marker_entry(GFile *p_file, const char *c_ent, gsize u_total) {
   char *c_uri = g_file_get_uri(p_file);
   _write_marker(c_ent, _mtime_of(p_file), c_uri);
   g_free(c_uri);
   _extend_sparse(c_ent, u_total);
   gint64      i_start = g_get_monotonic_time();
   GdkTexture *p_tex   = _get_thumb_fresh(p_file, 128);
   gdouble     d_secs  = (g_get_monotonic_time() - i_start) / 1e6;
   g_assert_cmpfloat(d_secs, <, 5.0);
   g_assert_nonnull(p_tex);
   return (p_tex);
}

/* A cache entry's SIZE is bounded before it is decoded (thumbnail.h,
 * GGAZE_THUMB_ENTRY_MAX_BYTES): a foreign writer can leave anything under
 * ggaze's entry name, and pre-fix _read_png_entry() pulled the whole entry
 * into the pool worker with g_file_load_contents(), so a planted multi-GB
 * file was a multi-GB allocation. The pair pins the bound from both sides:
 * the marker padded to EXACTLY the cap is still served (marker dimensions
 * come back, so the bytes were read from disk and the padding did not
 * spoil the decode -- the refusal below is the cap's, not the padding's),
 * and the same entry one byte longer is never read to its end: it is
 * regenerated, fast, and rewritten at its true size. */
static void
test_oversize_entry_regenerated(void) {
   char  *c_tmp  = _copy_fixture_to_tmp("plain.jpg");
   GFile *p_file = g_file_new_for_path(c_tmp);
   char  *c_ent  = thumbnail_cache_path(p_file, 128);

   GdkTexture *p_tex =
      _thumb_of_padded_marker_entry(p_file, c_ent, GGAZE_THUMB_ENTRY_MAX_BYTES);
   g_assert_true(_is_marker(p_tex));
   g_object_unref(p_tex);

   p_tex = _thumb_of_padded_marker_entry(p_file, c_ent,
                                         GGAZE_THUMB_ENTRY_MAX_BYTES + 1);
   g_assert_false(_is_marker(p_tex));
   g_assert_cmpint(gdk_texture_get_width(p_tex), <=, 128);
   g_object_unref(p_tex);
   struct stat st;
   g_assert_cmpint(stat(c_ent, &st), ==, 0);
   g_assert_cmpuint((guint64)st.st_size, <, GGAZE_THUMB_ENTRY_MAX_BYTES);
   g_assert_cmpint(_cached_mtime(c_ent), ==, _mtime_of(p_file));

   g_free(c_ent);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
}

/* --- cancellation -------------------------------------------------------- */

/* One request for p_file at i_size with p_cancel through a fresh Thumbnail,
 * expected to end in an error: GGAZE_RESULT/GGAZE_ERR are left for the
 * caller to assert on and free. */
static void
_request_expecting_error(GFile *p_file, int i_size, GCancellable *p_cancel) {
   Thumbnail *p_t = thumbnail_new();
   GGAZE_RESULT   = NULL;
   GGAZE_ERR      = NULL;
   GGAZE_LOOP     = g_main_loop_new(NULL, FALSE);
   thumbnail_get_async(p_t, p_file, i_size, p_cancel, _thumb_err_cb, NULL);
   g_main_loop_run(GGAZE_LOOP);
   g_main_loop_unref(GGAZE_LOOP);
   GGAZE_LOOP = NULL;
   thumbnail_delete(p_t);
}

/* The request for p_file at i_size with the (cancelled) p_cancel ends in
 * G_IO_ERROR_CANCELLED and no texture. */
static void
_assert_refused_as_cancelled(GFile *p_file, int i_size,
                             GCancellable *p_cancel) {
   _request_expecting_error(p_file, i_size, p_cancel);
   g_assert_null(GGAZE_RESULT);
   g_assert_error(GGAZE_ERR, G_IO_ERROR, G_IO_ERROR_CANCELLED);
   g_clear_error(&GGAZE_ERR);
}

/* A request handed an already-cancelled GCancellable (a grid item scrolled
 * out of view before the pool got to it) completes with
 * G_IO_ERROR_CANCELLED and does no I/O on its behalf. Proven from outside
 * the module: inotify IN_OPEN watches (open_counter.h) on the planted
 * marker entry and on the source file see zero opens, the entry is byte
 * for byte what it was, and a second request at a bucket whose directory
 * was removed leaves it absent -- the mkdir is the first thing after the
 * source stat. What this pins is the module's contract, not the line
 * that meets it: _thumb_run() bails before any call, and every GIO call
 * it would make takes the task's cancellable and refuses a cancelled one
 * itself (g_file_query_info(), g_file_read()), so neutralising the bails
 * alone still passes here (GIO refuses the stat first). Measured with
 * the bails gone, the single regression this test catches is the STAT
 * losing its cancellable, as thumbnail.c had it before tb2: the worker
 * gets past the stat and creates the bucket directory (with the read's
 * cancellable gone as well it also opens the entry, count 1). The entry
 * READ losing its cancellable on its own does NOT fail here -- the stat
 * is refused first, so the read is never reached and every count stays
 * 0; that one is pinned by test_cancel_mid_entry_read, which parks the
 * worker inside the read loop and fails "the reader kept the entry open
 * after the cancel". */
static void
test_precancelled_request(void) {
   char  *c_tmp  = _copy_fixture_to_tmp("plain.jpg");
   GFile *p_file = g_file_new_for_path(c_tmp);
   char  *c_uri  = g_file_get_uri(p_file);
   char  *c_ent  = thumbnail_cache_path(p_file, 128);
   _write_marker(c_ent, _mtime_of(p_file), c_uri);
   gchar *c_before = NULL;
   gsize  u_before = 0;
   g_assert_true(g_file_get_contents(c_ent, &c_before, &u_before, NULL));
   char *c_large = _wipe_bucket_dir("large"); /* the 256px bucket */

   GgtestOpenCounter s_entry, s_source;
   ggtest_open_counter_start(&s_entry, c_ent);
   ggtest_open_counter_start(&s_source, c_tmp);
   GCancellable *p_cancel = g_cancellable_new();
   g_cancellable_cancel(p_cancel);
   _assert_refused_as_cancelled(p_file, 128, p_cancel);
   _assert_refused_as_cancelled(p_file, 256, p_cancel);
   g_object_unref(p_cancel);
   g_assert_cmpuint(ggtest_open_counter_finish(&s_entry), ==, 0);
   g_assert_cmpuint(ggtest_open_counter_finish(&s_source), ==, 0);
   g_assert_false(g_file_test(c_large, G_FILE_TEST_EXISTS));

   gchar *c_after = NULL;
   gsize  u_after = 0;
   g_assert_true(g_file_get_contents(c_ent, &c_after, &u_after, NULL));
   g_assert_cmpmem(c_before, u_before, c_after, u_after);
   g_free(c_before);
   g_free(c_after);
   g_free(c_large);
   g_free(c_ent);
   g_free(c_uri);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
}

/* The writer behind a FIFO planted under the cache entry's name: the one
 * way to park the pool worker INSIDE _read_entry_bounded(), between two
 * reads, and cancel it there. (A large regular entry is read to its end in
 * milliseconds, so a cancel timed from another thread lands before, during
 * or after the read by luck and proves nothing either way.) The writer
 * serves the valid marker PNG in two chunks: chunk one, a wait until the
 * reader has taken it, THEN the cancel, then chunk two -- so wherever the
 * reader is at that moment (about to call read(), or blocked in it for
 * chunk two) its next read() call is one it enters with the cancellable
 * already cancelled, which GLocalFileInputStream checks before read(2).
 *
 * What proves the READ LOOP honoured the cancel, and not a later check:
 * GTask's own check_cancellable makes thumbnail_get_finish() report
 * CANCELLED for any cancelled request whatever _thumb_run() returned, so
 * the result alone would pass with a read loop that ignores its
 * cancellable. What such a loop cannot fake is when it lets go of the
 * FIFO: a loop that honours the cancel fails its next read() call without
 * touching the pipe and closes -- the write end sees POLLERR at once --
 * while one that ignores it takes chunk two and blocks in read() for more,
 * holding the FIFO open until the writer gives up. So the writer keeps the
 * write end open after chunk two and waits, within the budget, for the
 * reader to be gone; a reader still there when the budget is spent is the
 * named failure, and the writer closes anyway so that reader gets its EOF
 * and the test fails an assertion rather than meson's timeout. Every wait
 * is bounded by the one deadline, and every failure exit cancels the
 * request before it lets the worker go (_entry_writer_fail()): a worker
 * released uncancelled would regenerate the entry INTO the FIFO and hang
 * there, past the budget and the assertion alike. */
#define GGAZE_ENTRY_WRITER_BUDGET_US (5 * G_USEC_PER_SEC)

typedef struct {
   const char   *c_fifo; /* the entry path, a FIFO */
   const guint8 *p_png;  /* the valid marker entry, served in two */
   gsize         u_len;
   GCancellable *p_cancel;      /* cancelled between the two chunks */
   gint64        i_deadline;    /* monotonic, bounds every wait */
   const char   *c_failure;     /* first failure, NULL when none */
   char          c_errmsg[128]; /* formatted text c_failure may point at */
} EntryWriter;

static gboolean
_entry_writer_expired(const EntryWriter *p_w) {
   return (g_get_monotonic_time() >= p_w->i_deadline);
}

/* Record the first failure and make sure the worker comes back to report
 * it. The cancel comes FIRST: four of the five failure exits happen
 * before the planned cancel, and an uncancelled worker that gets its EOF
 * then goes on to _generate() -> _write_cache() -> gdk_pixbuf_save() on
 * the entry path -- an open-for-write of a FIFO nobody reads, which blocks
 * forever (observed: meson's 30 s timeout, the pool thread parked in
 * wait_for_partner, no diagnosis). Cancelled, the worker's next read()
 * call and the bail before the decode both end in CANCELLED instead.
 * Then release a reader the pool may have blocked in open(), the way
 * test_loader_pixbuf's _fifo_fail() does: an O_WRONLY|O_NONBLOCK open
 * succeeds exactly when a reader is there, and closing it at once is that
 * reader's EOF. A reader blocked in read() gets its EOF from the thread's
 * own close of the write end, which every exit reaches. So every failure
 * ends in the named assertion within the budget. */
static void
_entry_writer_fail(EntryWriter *p_w, const char *c_what) {
   if (p_w->c_failure == NULL) {
      p_w->c_failure = c_what;
   }
   g_cancellable_cancel(p_w->p_cancel);
   int i_fd = open(p_w->c_fifo, O_WRONLY | O_NONBLOCK);
   if (i_fd >= 0) {
      close(i_fd);
   }
}

/* Open the write end once the reader's open has counted it (ENXIO until
 * then, the only errno retried); any other errno, or the budget, is a
 * failure named with its text. */
static int
_entry_writer_open(EntryWriter *p_w) {
   for (;;) {
      int i_fd = open(p_w->c_fifo, O_WRONLY | O_NONBLOCK);
      if (i_fd >= 0) {
         return (i_fd);
      }
      if (errno != ENXIO) {
         g_snprintf(p_w->c_errmsg, sizeof(p_w->c_errmsg),
                    "opening the entry FIFO for writing failed: %s",
                    g_strerror(errno));
         _entry_writer_fail(p_w, p_w->c_errmsg);
         return (-1);
      }
      if (_entry_writer_expired(p_w)) {
         _entry_writer_fail(p_w, "the pool never opened the entry");
         return (-1);
      }
      g_usleep(1000);
   }
}

/* TRUE once the pipe is empty (FIONREAD == 0: the reader took chunk one,
 * so the cancel that follows lands between two reads); FALSE, failure
 * recorded, if bytes are still there when the budget is spent. */
static gboolean
_entry_writer_wait_drained(EntryWriter *p_w, int i_fd) {
   for (;;) {
      int i_pending = 0;
      if (ioctl(i_fd, FIONREAD, &i_pending) != 0) {
         _entry_writer_fail(p_w, "FIONREAD on the entry FIFO failed");
         return (FALSE);
      }
      if (i_pending == 0) {
         return (TRUE);
      }
      if (_entry_writer_expired(p_w)) {
         _entry_writer_fail(p_w, "the reader never took chunk one");
         return (FALSE);
      }
      g_usleep(1000);
   }
}

/* TRUE once no reader holds the FIFO: poll() on a write end with no events
 * requested wakes for POLLERR, which Linux raises the moment the last
 * reader is gone. FALSE, failure recorded, if a reader is still there
 * when the budget is spent -- the read loop that ignored its cancellable. */
static gboolean
_entry_writer_wait_reader_gone(EntryWriter *p_w, int i_fd) {
   for (;;) {
      struct pollfd pfd = {.fd = i_fd, .events = 0};
      if (poll(&pfd, 1, 10) > 0 && (pfd.revents & POLLERR) != 0) {
         return (TRUE);
      }
      if (_entry_writer_expired(p_w)) {
         _entry_writer_fail(p_w,
                            "the reader kept the entry open after the cancel");
         return (FALSE);
      }
   }
}

/* Chunk one, drain, cancel, chunk two, then wait for the reader to let go.
 * EPIPE on chunk two is fine: the reader saw the cancel before it called
 * read() again and is gone already (SIGPIPE is ignored by the test). The
 * close at the end is the EOF a reader that ignored the cancel needs to
 * come back at all. */
static gpointer
_entry_writer_thread(gpointer p_data) {
   EntryWriter *p_w  = (EntryWriter *)p_data;
   int          i_fd = _entry_writer_open(p_w);
   if (i_fd < 0) {
      return (NULL);
   }
   gsize u_first = p_w->u_len / 2;
   if ((gsize)write(i_fd, p_w->p_png, u_first) != u_first) {
      _entry_writer_fail(p_w, "short write of chunk one");
   } else if (_entry_writer_wait_drained(p_w, i_fd)) {
      g_cancellable_cancel(p_w->p_cancel);
      gsize u_rest = p_w->u_len - u_first;
      if ((gsize)write(i_fd, p_w->p_png + u_first, u_rest) != u_rest &&
          errno != EPIPE) {
         _entry_writer_fail(p_w, "short write of chunk two");
      } else {
         _entry_writer_wait_reader_gone(p_w, i_fd);
      }
   }
   close(i_fd);
   return (NULL);
}

/* The bytes of a valid marker entry for p_file (correct Thumb::MTime and
 * Thumb::URI), built through a scratch file so gdk_pixbuf_save()'s tEXt
 * options are the ones the module verifies. Caller g_frees. */
static guint8 *
_marker_entry_bytes(GFile *p_file, gsize *p_len) {
   gchar *c_scratch = NULL;
   gint   i_fd = g_file_open_tmp("ggaze-thumb-marker-XXXXXX", &c_scratch, NULL);
   g_assert_cmpint(i_fd, >=, 0);
   close(i_fd);
   char *c_uri = g_file_get_uri(p_file);
   _write_marker(c_scratch, _mtime_of(p_file), c_uri);
   g_free(c_uri);
   gchar *c_png = NULL;
   g_assert_true(g_file_get_contents(c_scratch, &c_png, p_len, NULL));
   unlink(c_scratch);
   g_free(c_scratch);
   return ((guint8 *)c_png);
}

/* The task's GCancellable reaches the cache read itself: a request
 * cancelled while _read_entry_bounded() is between two reads of a VALID
 * entry ends with G_IO_ERROR_CANCELLED, no texture, no critical (g_test
 * makes criticals fatal) and no leak of the partial buffer (the ASan lane
 * runs this under LSan) -- and, the proof the read loop and not a later
 * check stopped it, the reader lets go of the entry at once (EntryWriter
 * above). A regression that drops the cancellable from the read loop
 * fails "the reader kept the entry open after the cancel" after the
 * budget. */
static void
test_cancel_mid_entry_read(void) {
   char   *c_tmp  = _copy_fixture_to_tmp("plain.jpg");
   GFile  *p_file = g_file_new_for_path(c_tmp);
   gsize   u_len  = 0;
   guint8 *p_png  = _marker_entry_bytes(p_file, &u_len);
   char   *c_ent  = thumbnail_cache_path(p_file, 128);
   char   *c_dir  = g_path_get_dirname(c_ent);
   g_assert_cmpint(g_mkdir_with_parents(c_dir, 0700), ==, 0);
   g_free(c_dir);
   g_assert_cmpint(mkfifo(c_ent, 0600), ==, 0);
   signal(SIGPIPE, SIG_IGN); /* a reader gone early is EPIPE, not death */

   EntryWriter s_w = {
      .c_fifo     = c_ent,
      .p_png      = p_png,
      .u_len      = u_len,
      .p_cancel   = g_cancellable_new(),
      .i_deadline = g_get_monotonic_time() + GGAZE_ENTRY_WRITER_BUDGET_US,
      .c_failure  = NULL,
   };
   GThread *p_thread = g_thread_new("entry-writer", _entry_writer_thread, &s_w);
   _request_expecting_error(p_file, 128, s_w.p_cancel);
   g_thread_join(p_thread);

   g_assert_cmpstr(s_w.c_failure, ==, NULL);
   g_assert_null(GGAZE_RESULT);
   g_assert_error(GGAZE_ERR, G_IO_ERROR, G_IO_ERROR_CANCELLED);
   g_clear_error(&GGAZE_ERR);

   g_object_unref(s_w.p_cancel);
   g_free(p_png);
   unlink(c_ent);
   g_free(c_ent);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
}

/* Registration is split by theme so no function approaches the 50-line
 * mark (c-best-practices). */
static void
_add_cache_tests(void) {
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
   g_test_add_func("/thumbnail/delete_completes_queued_requests",
                   test_delete_completes_queued_requests);
}

static void
_add_gate_tests(void) {
   g_test_add_func("/thumbnail/oversized_jpeg", test_oversized_jpeg);
   g_test_add_func("/thumbnail/padded_past_prefix_oversized_jpeg",
                   test_padded_past_prefix_oversized_jpeg);
   g_test_add_func("/thumbnail/truncated_jxl_fails_fast",
                   test_truncated_jxl_fails_fast);
   g_test_add_func("/thumbnail/empty_file_fails_fast",
                   test_empty_file_fails_fast);
   g_test_add_func("/thumbnail/garbage_jxl_fails_fast",
                   test_garbage_jxl_fails_fast);
   g_test_add_func("/thumbnail/truncated_jxl_entry_regenerated",
                   test_truncated_jxl_entry_regenerated);
   g_test_add_func("/thumbnail/garbage_jxl_entry_regenerated",
                   test_garbage_jxl_entry_regenerated);
   g_test_add_func("/thumbnail/non_png_entry_regenerated",
                   test_non_png_entry_regenerated);
   g_test_add_func("/thumbnail/oversize_entry_regenerated",
                   test_oversize_entry_regenerated);
}

static void
_add_cancel_tests(void) {
   g_test_add_func("/thumbnail/precancelled_request",
                   test_precancelled_request);
   g_test_add_func("/thumbnail/cancel_mid_entry_read",
                   test_cancel_mid_entry_read);
}

/* Delete every file in the bucket directory p_dir. */
static void
_remove_bucket_files(GFile *p_dir) {
   GFileEnumerator *p_e = g_file_enumerate_children(
      p_dir, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (p_e == NULL) {
      return;
   }
   GFileInfo *p_info;
   while ((p_info = g_file_enumerator_next_file(p_e, NULL, NULL)) != NULL) {
      GFile *p_child = g_file_get_child(p_dir, g_file_info_get_name(p_info));
      g_file_delete(p_child, NULL, NULL);
      g_object_unref(p_child);
      g_object_unref(p_info);
   }
   g_object_unref(p_e);
}

/* Remove the temp XDG_CACHE_HOME (best-effort, two levels: thumbnails/ and
 * its bucket directories). */
static void
_remove_cache_dir(void) {
   GFile           *p_cd = g_file_new_for_path(GGAZE_CACHE_DIR);
   GFileEnumerator *p_e =
      g_file_enumerate_children(p_cd, "standard::name,standard::type",
                                G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (p_e != NULL) {
      GFileInfo *p_info;
      while ((p_info = g_file_enumerator_next_file(p_e, NULL, NULL)) != NULL) {
         GFile *p_child = g_file_get_child(p_cd, g_file_info_get_name(p_info));
         if (g_file_info_get_file_type(p_info) == G_FILE_TYPE_DIRECTORY) {
            _remove_bucket_files(p_child);
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

   _add_cache_tests();
   _add_gate_tests();
   _add_cancel_tests();
   int i_ret = g_test_run();
   _remove_cache_dir();
   return (i_ret);
}
