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
 * The gate runs before the build-dependent JXL refusal, so a truncated JXL
 * gets INVALID_DATA in every build; only a JXL that clears the gate then
 * differs by build (G_IO_ERROR_NOT_SUPPORTED without libjxl, libjxl's own
 * verdict with it) -- test_garbage_jxl_fails_fast and
 * test_tiny_images_load pin both outcomes.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "loader/loader.h"
#include "loader/pixbuf-util.h"

#include <errno.h>
#include <fcntl.h>
#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ggaze-config.h"
#include "loader/detect.h"
#include "mem_file.h"
#include "tiny_images.h"

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

/* One truncated vector per RULE in detect.c's table -- both TIFF byte
 * orders, both JXL spellings, all five ISO BMFF brands -- the same vectors
 * tests/test_detect.c checks against detect_min_file_len(). Each must be
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
   {"avif", {0, 0, 0, 0x1C, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f'}, 12},
   {"avis", {0, 0, 0, 0x1C, 'f', 't', 'y', 'p', 'a', 'v', 'i', 's'}, 12},
   {"heic", {0, 0, 0, 0x1C, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c'}, 12},
   {"heix", {0, 0, 0, 0x1C, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'x'}, 12},
   {"mif1", {0, 0, 0, 0x1C, 'f', 't', 'y', 'p', 'm', 'i', 'f', '1'}, 12},
   {"webp", {'R', 'I', 'F', 'F', 0x10, 0, 0, 0, 'W', 'E', 'B', 'P'}, 12},
   {"png", {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}, 8},
   {"gif", {'G', 'I', 'F', '8', '9', 'a'}, 6},
   {"tiff le", {'I', 'I', 0x2A, 0x00}, 4},
   {"tiff be", {'M', 'M', 0x00, 0x2A}, 4},
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

/* A single byte carries no signature, so the gate imposes nothing -- the
 * error must NOT be the gate's INVALID_DATA (nor the NOT_SUPPORTED of the
 * build-dependent refusal) but gdk-pixbuf's own "unrecognised" verdict, and
 * a prompt one. */
static void
test_one_byte_file(void) {
   const guint8 h[]     = {0xFF};
   gint64       i_start = g_get_monotonic_time();
   GError      *p_err   = NULL;
   GdkTexture  *p_tex   = load_bytes(h, 1, &p_err);
   gdouble      d_secs  = (g_get_monotonic_time() - i_start) / 1e6;
   g_assert_null(p_tex);
   g_assert_nonnull(p_err);
   g_assert_false(g_error_matches(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA));
   g_assert_false(g_error_matches(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED));
   g_error_free(p_err);
   g_assert_cmpfloat(d_secs, <, 5.0);
}

/* Push one buffer through all three entry points and assert each fails
 * (NULL / FALSE) within the 5 s budget. Returns loader_load()'s error for
 * the caller to inspect (caller frees). The peek has no error out-pointer,
 * so the budget is the only thing that can catch a stall there. */
static GError *
_all_entry_points_fail_fast(const guint8 *p_buf, gsize u_len) {
   gchar      *c_path  = write_tmp(p_buf, u_len);
   GFile      *p_file  = g_file_new_for_path(c_path);
   gint64      i_start = g_get_monotonic_time();
   GError     *p_err   = NULL;
   GdkTexture *p_tex   = loader_load(p_file, NULL, &p_err);
   g_assert_null(p_tex);
   g_assert_nonnull(p_err);
   GError    *p_thumb_err = NULL;
   GdkPixbuf *p_pix =
      loader_load_pixbuf_scaled(p_file, 128, NULL, &p_thumb_err);
   g_assert_null(p_pix);
   g_assert_nonnull(p_thumb_err);
   g_assert_cmpuint(p_thumb_err->domain, ==, p_err->domain);
   g_assert_cmpint(p_thumb_err->code, ==, p_err->code);
   g_error_free(p_thumb_err);
   int i_w = -1, i_h = -1;
   g_assert_false(loader_peek_dimensions(p_file, &i_w, &i_h));
   gdouble d_secs = (g_get_monotonic_time() - i_start) / 1e6;
   g_assert_cmpfloat(d_secs, <, 5.0);
   g_object_unref(p_file);
   unlink(c_path);
   g_free(c_path);
   return (p_err);
}

/* The user-facing hazard the length gate alone does not close: a JXL that
 * is LONG enough but garbage. glycin-jxl was measured to wait forever on
 * an 8-byte codestream, a 60-byte one and a box-wrapped container alike,
 * through gdk_pixbuf_loader_close() and gdk_pixbuf_get_file_info() both.
 * Without libjxl the dispatcher must refuse every JXL up front
 * (G_IO_ERROR_NOT_SUPPORTED, "JXL support is not built in"); with libjxl
 * the specific backend must get the file BEFORE any gdk-pixbuf call -- the
 * old loader_peek_dimensions() asked gdk_pixbuf_get_file_info() first and
 * hung the info worker even in the full build. Either way: all three entry
 * points, all three shapes, within the budget. */
static void
test_garbage_jxl_fails_fast(void) {
   guint8 cs8[8]   = {0xFF, 0x0A};
   guint8 cs60[60] = {0xFF, 0x0A};
   guint8 box[72]  = {0,    0,    0,    0x0C, 'J',  'X', 'L',  ' ', 0x0D,
                      0x0A, 0x87, 0x0A, 0,    0,    0,   0x14, 'f', 't',
                      'y',  'p',  'j',  'x',  'l',  ' ', 0,    0,   0,
                      0,    'j',  'x',  'l',  ' ',  0,   0,    0,   0x28,
                      'j',  'x',  'l',  'c',  0xFF, 0x0A};
   const struct {
      const char   *c_name;
      const guint8 *p_buf;
      gsize         u_len;
   } vecs[] = {
      {"8-byte codestream", cs8, sizeof(cs8)},
      {"60-byte codestream", cs60, sizeof(cs60)},
      {"72-byte container", box, sizeof(box)},
   };
   for (gsize u = 0; u < G_N_ELEMENTS(vecs); u++) {
      g_test_message("%s", vecs[u].c_name);
      /* Every vector clears the length gate: that is the point. */
      g_assert_true(
         detect_reject_truncated(vecs[u].p_buf, vecs[u].u_len, NULL));
      GError *p_err = _all_entry_points_fail_fast(vecs[u].p_buf, vecs[u].u_len);
      g_assert_false(
         g_error_matches(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA));
#if GGAZE_HAVE_JXL
      /* libjxl's own verdict, whatever it is: not the dispatcher's. */
      g_assert_false(
         g_error_matches(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED));
#else
      g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
      g_assert_nonnull(strstr(p_err->message, "JXL"));
#endif
      g_error_free(p_err);
   }
}

/* TRUE iff the gdk-pixbuf module c_module ("webp", "tiff", ...) is
 * installed on this machine. */
static gboolean
_pixbuf_module_available(const char *c_module) {
   GSList  *p_formats = gdk_pixbuf_get_formats();
   gboolean b_found   = FALSE;
   for (GSList *p_l = p_formats; p_l != NULL; p_l = p_l->next) {
      gchar *c_name = gdk_pixbuf_format_get_name((GdkPixbufFormat *)p_l->data);
      b_found       = b_found || g_strcmp0(c_name, c_module) == 0;
      g_free(c_name);
   }
   g_slist_free(p_formats);
   return (b_found);
}

/* The modules gdk-pixbuf2 itself ships on CI's fedora:40 (and on any
 * desktop worth the name): a test that needs one of these and finds it
 * missing has found a broken test machine, not an optional feature, and
 * must FAIL rather than quietly skip -- a skipped FIFO test hid nothing
 * for weeks only because the module was there. The escape hatch for a
 * deliberately stripped box is GGAZE_TEST_ALLOW_MISSING_PIXBUF_LOADERS=1.
 * webp/tiff/ico/jxl come from separate packages and stay optional. */
static gboolean
_pixbuf_module_is_core(const char *c_module) {
   return (g_strcmp0(c_module, "png") == 0 || g_strcmp0(c_module, "gif") == 0 ||
           g_strcmp0(c_module, "jpeg") == 0);
}

/* TRUE iff a decode through gdk-pixbuf module c_module may be asserted on
 * this machine. A missing optional module is reported and skipped; a
 * missing core module fails the current test (see _pixbuf_module_is_core())
 * unless the opt-out variable is set, in which case it is skipped too. */
static gboolean
_pixbuf_module_usable(const char *c_module) {
   if (_pixbuf_module_available(c_module)) {
      return (TRUE);
   }
   if (_pixbuf_module_is_core(c_module) &&
       g_strcmp0(g_getenv("GGAZE_TEST_ALLOW_MISSING_PIXBUF_LOADERS"), "1") !=
          0) {
      g_test_fail_printf(
         "gdk-pixbuf module '%s' is missing; it ships with "
         "gdk-pixbuf2 on fedora:40, so this is a broken test "
         "machine (set GGAZE_TEST_ALLOW_MISSING_PIXBUF_LOADERS=1 "
         "to skip instead)",
         c_module);
      return (FALSE);
   }
   g_test_message("  no gdk-pixbuf module '%s' here; decode skipped", c_module);
   return (FALSE);
}

/* One TinyImage through loader_load(): 1x1 where this build decodes it. */
static void
_assert_tiny_image_loads(const TinyImage *p_t) {
   gint64      i_start = g_get_monotonic_time();
   GError     *p_err   = NULL;
   GdkTexture *p_tex   = load_bytes(p_t->p_bytes, p_t->u_len, &p_err);
   gdouble     d_secs  = (g_get_monotonic_time() - i_start) / 1e6;
   g_assert_cmpfloat(d_secs, <, 5.0);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_tex);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 1);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, 1);
   g_object_unref(p_tex);
}

/* TRUE iff this build decodes p_t without a gdk-pixbuf module: the jpeg
 * backend claims every JPEG, so the "jpeg" module is irrelevant with it. */
static gboolean
_tiny_image_has_own_backend(const TinyImage *p_t) {
#if GGAZE_HAVE_JPEG
   return (p_t->e_format == GGAZE_FMT_JPEG);
#else
   (void)p_t;
   return (FALSE);
#endif
}

/* The smallest real file of every format (tests/helpers/tiny_images.h)
 * must get PAST the gate -- test_detect.c proves the gate accepts them;
 * this proves the whole dispatcher does -- and decode as 1x1 wherever this
 * build has a decoder: a JXL needs the jxl feature (without it the
 * dispatcher refuses it as NOT_SUPPORTED, by design), the rest need their
 * gdk-pixbuf module. PNG/GIF/JPEG are required (a missing one fails the
 * test, see _pixbuf_module_usable()); WebP/TIFF/ICO are skipped with a
 * message when their optional module is absent. */
static void
test_tiny_images_load(void) {
   for (gsize u = 0; u < G_N_ELEMENTS(TINY_IMAGES); u++) {
      const TinyImage *p_t = &TINY_IMAGES[u];
      g_test_message("%s (%" G_GSIZE_FORMAT " bytes)", p_t->c_name, p_t->u_len);
      if (p_t->b_needs_jxl) {
#if GGAZE_HAVE_JXL
         _assert_tiny_image_loads(p_t);
#else
         GError     *p_err = NULL;
         GdkTexture *p_tex = load_bytes(p_t->p_bytes, p_t->u_len, &p_err);
         g_assert_null(p_tex);
         g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
         g_error_free(p_err);
#endif
      } else if (_tiny_image_has_own_backend(p_t) ||
                 _pixbuf_module_usable(p_t->c_pixbuf_module)) {
         _assert_tiny_image_loads(p_t);
      }
   }
}

/* inotify_init1()/inotify_add_watch() fail for reasons that have nothing
 * to do with the loader -- fs.inotify.max_user_instances exhausted by a
 * desktop full of file watchers is the usual one -- so both harnesses
 * below report such a failure with its errno text, where a bare
 * "assertion failed: (fd >= 0)" reads like a wrong open count. */
static void
_assert_inotify_ok(int i_ret, const char *c_call) {
   if (i_ret < 0) {
      g_error("%s failed: %s (fs.inotify.max_user_instances or "
              "max_user_watches exhausted?)",
              c_call, g_strerror(errno));
   }
}

/* --- FIFO harness ------------------------------------------------------- */

/* A helper thread that serves a FIFO to the loader under test, one writer
 * session per reader that opens it. It proves that the header reads cope
 * with a file that arrives in two pieces (test_fifo_two_chunk_read for the
 * sniff, test_fifo_two_chunk_jpeg_peek for detect's SOF peek). It also
 * counts the sessions, and two kernel facts shape how:
 *
 * 1. The count is only sound for a file SHORTER than every read the loader
 *    makes of it: then every session ends in the writer's close (the
 *    reader's EOF), so the reader cannot close before the writer and the
 *    next open is a new session. A file that exactly fills a read_all() is
 *    satisfied without EOF, the reader closes first, and its next open is
 *    served by the still-open session -- one open counted for two
 *    (observed: a 64-byte JPEG against the 64-byte sniff, ~1.5 % of runs).
 *    Open counts on regular files are therefore taken with inotify instead
 *    (OpenCounter below), never with this harness.
 *
 * 2. The writer's O_WRONLY|O_NONBLOCK open succeeds while the FIFO has ANY
 *    reader counted, including one that is still closing: the kernel
 *    queues the reader's IN_CLOSE_NOWRITE (fsnotify_close) BEFORE
 *    pipe_release() drops the reader count, so an open issued right after
 *    that event can land on a reader that is gone a few microseconds later
 *    (observed ~1 in 300 runs under load: after the loader's last session
 *    the writer served a third session nobody read, waited the whole
 *    budget for chunk one to drain and failed with "the reader never
 *    consumed the first chunk"). A session is therefore counted -- and
 *    served -- only once the READER's own IN_OPEN has been seen after the
 *    writer's open. Two names for the one inode tell the two opens apart:
 *    the loader opens GGAZE_FIFO_READER_NAME, the writer opens the hard
 *    link GGAZE_FIFO_WRITER_NAME, and an inotify watch on the DIRECTORY
 *    reports each open under the name it went through. (A watch on the
 *    FIFO itself carries no name and merges the two adjacent IN_OPENs into
 *    one event, the coalescing OpenCounter describes.) An open that no
 *    reader IN_OPEN follows before the main thread is done was that
 *    leftover count: closed, not counted, never written to.
 *
 * Every wait is bounded by one deadline (GGAZE_FIFO_BUDGET_US) so a broken
 * loader fails a named assertion instead of meson's 30 s binary timeout,
 * and every wait also ends as soon as the main thread's call has returned
 * (i_done), so whatever a wait is still expecting then costs milliseconds,
 * not the budget. The writer never blocks in open() (it polls O_NONBLOCK),
 * never blocks in read() on the inotify fd (poll() in short slices) and,
 * on its way out after a failure, opens and closes the FIFO once so a
 * reader stuck in open() or read() sees EOF and the main thread's call
 * returns. The first failure is recorded in c_failure for the main thread
 * to assert on; the writer never aborts the process itself. */
#define GGAZE_FIFO_BUDGET_US (5 * G_USEC_PER_SEC)
#define GGAZE_FIFO_POLL_MS 10
#define GGAZE_FIFO_READER_NAME "image.fifo"
#define GGAZE_FIFO_WRITER_NAME "writer.fifo"

typedef struct {
   gchar        *c_fifo; /* the reader's name (what the tests hand out) */
   gchar        *c_writer_link; /* the writer's name: a hard link to c_fifo */
   const guint8 *p_img;
   gsize         u_len;
   gsize         u_first_chunk; /* 0: one write; else split after this */
   int i_inotify; /* watches the directory: IN_OPEN, IN_CLOSE_NOWRITE */
   union {
      struct inotify_event s_align; /* keeps raw aligned for the casts */
      guint8               raw[4096];
   } events; /* read but not yet consumed, u_ev_pos..u_ev_len */
   gsize       u_ev_len;
   gsize       u_ev_pos;
   gint64      i_deadline;    /* monotonic, every wait checks it */
   gint        i_done;        /* atomic: main thread's call returned */
   guint       u_opens;       /* reader sessions served */
   const char *c_failure;     /* first failure, NULL when none */
   char        c_errmsg[160]; /* formatted text c_failure may point at */
   GThread    *p_thread;
   gchar      *c_tmpdir;
} FifoWriter;

static gboolean
_fifo_expired(const FifoWriter *p_w) {
   return (g_get_monotonic_time() >= p_w->i_deadline);
}

/* Record the first failure and release a reader the loader may have
 * blocked in open()/read(): an O_WRONLY|O_NONBLOCK open succeeds exactly
 * when a reader is there, and closing it at once is that reader's EOF. */
static void
_fifo_fail(FifoWriter *p_w, const char *c_what) {
   if (p_w->c_failure == NULL) {
      p_w->c_failure = c_what;
   }
   int i_fd = open(p_w->c_writer_link, O_WRONLY | O_NONBLOCK);
   if (i_fd >= 0) {
      close(i_fd);
   }
}

/* Open the write end as soon as a reader is counted, or return -1 once the
 * main thread is done or the budget is spent. ENXIO is the one errno that
 * means "no reader yet" (what O_NONBLOCK promises on a FIFO) and the only
 * one worth another try; anything else -- ENOENT with the temp dir gone,
 * EACCES, EMFILE -- is the harness's own breakage and is named at once,
 * errno text included, rather than polled for a whole budget and then
 * reported as a reader that never came. The count may be a closing
 * reader's (fact 2 above): the caller confirms a live one before it
 * writes. */
static int
_fifo_open_writer(FifoWriter *p_w) {
   while (!g_atomic_int_get(&p_w->i_done)) {
      int i_fd = open(p_w->c_writer_link, O_WRONLY | O_NONBLOCK);
      if (i_fd >= 0) {
         return (i_fd);
      }
      if (errno != ENXIO) {
         g_snprintf(p_w->c_errmsg, sizeof(p_w->c_errmsg),
                    "opening the FIFO's write end failed: %s",
                    g_strerror(errno));
         _fifo_fail(p_w, p_w->c_errmsg);
         return (-1);
      }
      if (_fifo_expired(p_w)) {
         _fifo_fail(p_w, "no reader opened the FIFO within the budget");
         return (-1);
      }
      g_usleep(1000);
   }
   return (-1);
}

/* The next inotify event on the directory, refilling the buffer with a
 * deadline-bound poll() once it is exhausted. NULL once the main thread is
 * done (no failure: the writer just stops) or the budget is spent (c_what
 * is recorded). Polled in GGAZE_FIFO_POLL_MS slices so i_done is noticed
 * without a wake-up channel of its own. */
static const struct inotify_event *
_fifo_next_event(FifoWriter *p_w, const char *c_what) {
   while (p_w->u_ev_pos >= p_w->u_ev_len) {
      if (g_atomic_int_get(&p_w->i_done)) {
         return (NULL);
      }
      gint64 i_left_ms = (p_w->i_deadline - g_get_monotonic_time()) / 1000;
      if (i_left_ms <= 0) {
         _fifo_fail(p_w, c_what);
         return (NULL);
      }
      struct pollfd pfd = {.fd = p_w->i_inotify, .events = POLLIN};
      if (poll(&pfd, 1, (int)MIN(i_left_ms, GGAZE_FIFO_POLL_MS)) <= 0) {
         continue;
      }
      ssize_t i_n =
         read(p_w->i_inotify, p_w->events.raw, sizeof(p_w->events.raw));
      if (i_n <= 0) {
         _fifo_fail(p_w, "reading the inotify queue failed");
         return (NULL);
      }
      p_w->u_ev_len = (gsize)i_n;
      p_w->u_ev_pos = 0;
   }
   const struct inotify_event *p_ev =
      (const struct inotify_event *)(p_w->events.raw + p_w->u_ev_pos);
   p_w->u_ev_pos += sizeof(*p_ev) + p_ev->len;
   return (p_ev);
}

/* Wait for the READER's event of u_mask (IN_OPEN: a session has really
 * begun; IN_CLOSE_NOWRITE: it is over, and the next open is a new one --
 * without that wait the writer's next open succeeded at once, the next
 * session's bytes landed in this session's pipe and the loader's own next
 * open later blocked with no writer left, seen with strace). Events under
 * the writer's own name and on the directory itself are skipped; the
 * writer's closes are IN_CLOSE_WRITE and not watched at all. FALSE once
 * the main thread is done or the budget is spent. */
static gboolean
_fifo_wait_reader_event(FifoWriter *p_w, guint32 u_mask, const char *c_what) {
   for (;;) {
      const struct inotify_event *p_ev = _fifo_next_event(p_w, c_what);
      if (p_ev == NULL) {
         return (FALSE);
      }
      if ((p_ev->mask & u_mask) != 0 && p_ev->len > 0 &&
          strcmp(p_ev->name, GGAZE_FIFO_READER_NAME) == 0) {
         return (TRUE);
      }
   }
}

/* poll() on a FIFO's write end with no events requested wakes for POLLERR
 * only, which Linux raises the moment the last reader is gone; with a
 * timeout it doubles as the pause between looks at FIONREAD. */
static gboolean
_fifo_reader_gone(int i_fd, int i_timeout_ms) {
   struct pollfd pfd = {.fd = i_fd, .events = 0};
   return (poll(&pfd, 1, i_timeout_ms) > 0 && (pfd.revents & POLLERR) != 0);
}

/* Block until the reader has consumed everything written so far (the pipe
 * is empty: FIONREAD == 0), so the next write really is a SECOND chunk the
 * reader's first read() cannot have merged in. A merged read would only
 * make test_fifo_two_chunk_read less sensitive, never fail it; this keeps
 * it sensitive. Gives up as soon as the main thread is done -- a loader
 * that returned with chunk one unread is the failure named here, and it
 * is named in milliseconds rather than after the budget -- and the moment
 * the reader closes with the chunk unread. */
static void
_fifo_wait_drained(FifoWriter *p_w, int i_fd) {
   for (;;) {
      int i_pending = 0;
      if (ioctl(i_fd, FIONREAD, &i_pending) != 0) {
         _fifo_fail(p_w, "FIONREAD on the FIFO failed");
         return;
      }
      if (i_pending == 0) {
         return;
      }
      if (g_atomic_int_get(&p_w->i_done) || _fifo_expired(p_w)) {
         _fifo_fail(p_w, "the reader never consumed the first chunk");
         return;
      }
      if (_fifo_reader_gone(i_fd, 1)) {
         _fifo_fail(p_w, "the reader closed with the first chunk unread");
         return;
      }
   }
}

/* Deliver the image to the reader on i_fd: whole, or as u_first_chunk
 * bytes, a wait until the reader has taken them, and the rest. Closing
 * after the last write is what gives the reader its EOF. */
static void
_fifo_serve_session(FifoWriter *p_w, int i_fd) {
   gsize u_first = (p_w->u_first_chunk > 0) ? p_w->u_first_chunk : p_w->u_len;
   if ((gsize)write(i_fd, p_w->p_img, u_first) != u_first) {
      _fifo_fail(p_w, "short write of the first chunk");
   } else if (u_first < p_w->u_len) {
      _fifo_wait_drained(p_w, i_fd);
      gsize u_rest = p_w->u_len - u_first;
      if ((gsize)write(i_fd, p_w->p_img + u_first, u_rest) != u_rest) {
         _fifo_fail(p_w, "short write of the second chunk");
      }
   }
   close(i_fd);
}

static gpointer
_fifo_writer_thread(gpointer p_data) {
   FifoWriter *p_w = (FifoWriter *)p_data;
   for (;;) {
      int i_fd = _fifo_open_writer(p_w);
      if (i_fd < 0) {
         return (NULL);
      }
      /* The open proves a reader is COUNTED, not that one is coming (fact
       * 2 in the harness comment): only the reader's own IN_OPEN starts a
       * session. Without it the fd was a closing reader's leftover count,
       * and the main thread being done is how that shows. */
      if (!_fifo_wait_reader_event(
             p_w, IN_OPEN, "no reader completed its open within the budget")) {
         close(i_fd);
         return (NULL);
      }
      p_w->u_opens++;
      _fifo_serve_session(p_w, i_fd);
      _fifo_wait_reader_event(
         p_w, IN_CLOSE_NOWRITE,
         "the reader never closed the FIFO within the budget");
      if (p_w->c_failure != NULL) {
         return (NULL);
      }
   }
}

/* Create the FIFO and the writer's hard link to it in a private temp dir,
 * arm the inotify watch on that dir and start the writer. p_w must already
 * carry p_img/u_len/u_first_chunk. */
static void
_fifo_start(FifoWriter *p_w) {
   p_w->c_tmpdir = g_dir_make_tmp("ggaze-fifo-XXXXXX", NULL);
   g_assert_nonnull(p_w->c_tmpdir);
   p_w->c_fifo = g_build_filename(p_w->c_tmpdir, GGAZE_FIFO_READER_NAME, NULL);
   p_w->c_writer_link =
      g_build_filename(p_w->c_tmpdir, GGAZE_FIFO_WRITER_NAME, NULL);
   g_assert_cmpint(mkfifo(p_w->c_fifo, 0600), ==, 0);
   g_assert_cmpint(link(p_w->c_fifo, p_w->c_writer_link), ==, 0);
   p_w->i_inotify = inotify_init1(IN_CLOEXEC);
   _assert_inotify_ok(p_w->i_inotify, "inotify_init1");
   _assert_inotify_ok(inotify_add_watch(p_w->i_inotify, p_w->c_tmpdir,
                                        IN_OPEN | IN_CLOSE_NOWRITE),
                      "inotify_add_watch");
   /* Should a reader ever close early, the writer must see EPIPE, not
    * take the whole test binary down with SIGPIPE. */
   signal(SIGPIPE, SIG_IGN);
   p_w->i_deadline  = g_get_monotonic_time() + GGAZE_FIFO_BUDGET_US;
   p_w->i_done      = 0;
   p_w->u_opens     = 0;
   p_w->u_ev_len    = 0;
   p_w->u_ev_pos    = 0;
   p_w->c_failure   = NULL;
   p_w->c_errmsg[0] = '\0';
   p_w->p_thread    = g_thread_new("fifo-writer", _fifo_writer_thread, p_w);
}

/* Stop the writer (the call under test has returned), surface its first
 * failure as a named assertion, and return how many reader sessions it
 * served. */
static guint
_fifo_finish(FifoWriter *p_w) {
   g_atomic_int_set(&p_w->i_done, 1);
   g_thread_join(p_w->p_thread);
   g_assert_cmpstr(p_w->c_failure, ==, NULL);
   close(p_w->i_inotify);
   unlink(p_w->c_writer_link);
   unlink(p_w->c_fifo);
   g_rmdir(p_w->c_tmpdir);
   g_free(p_w->c_fifo);
   g_free(p_w->c_writer_link);
   g_free(p_w->c_tmpdir);
   return (p_w->u_opens);
}

/* The header read must be min(file, 64) bytes, not "whatever the first
 * read() returned": on a FIFO, a pipe or a GVFS stream a single read may
 * return fewer bytes than the file holds, and the old single
 * g_input_stream_read() then made detect_reject_truncated() refuse a valid
 * file delivered in two writes as a "truncated GIF file: 10 bytes". A real
 * FIFO written from a helper thread reproduces exactly that, with the
 * second write held back until the reader has taken the first. The vector
 * is the 43-byte GIF from tiny_images.h, chosen because it fits inside the
 * 64-byte sniff: the sniff then drains the FIFO to EOF, so the backend's
 * own open (session two) can never inherit bytes the sniff left behind --
 * with a file longer than the sniff, the second open could race the
 * writer's close and read the tail of session one instead. Exactly two
 * opens: the sniff and the pixbuf backend's read. */
static void
test_fifo_two_chunk_read(void) {
   if (!_pixbuf_module_usable("gif")) {
      return;
   }
   FifoWriter s_writer = {
      .p_img = TINY_GIF, .u_len = sizeof(TINY_GIF), .u_first_chunk = 10};
   g_assert_cmpuint(s_writer.u_len, <=, GGAZE_DETECT_SNIFF_LEN);
   g_assert_cmpuint(s_writer.u_len, >, s_writer.u_first_chunk);
   _fifo_start(&s_writer);

   GFile      *p_file = g_file_new_for_path(s_writer.c_fifo);
   GError     *p_err  = NULL;
   GdkTexture *p_tex  = loader_load(p_file, NULL, &p_err);
   /* The loader's verdict first: on a regression it names the cause
    * ("truncated GIF file: 10 bytes"), where the writer only sees the
    * EPIPE of a reader that gave up after the first chunk. */
   g_assert_no_error(p_err);
   g_assert_nonnull(p_tex);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 1);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, 1);
   g_assert_cmpuint(_fifo_finish(&s_writer), ==, 2);
   g_object_unref(p_tex);
   g_object_unref(p_file);
}

/* A GGAZE_SOF_JPEG_LEN-byte JPEG -- SOI, one baseline SOF0 declaring
 * u_h x u_w with three components, an SOS -- for the peek tests below: it
 * clears the 25-byte minimum, detect's SOF scan finds the frame header at
 * offset 2, and gdk-pixbuf's header parse (should it be consulted) reports
 * the same size. Deliberately SHORTER than the 64-byte sniff, so that
 * served from the FIFO every read of it runs to EOF and the session count
 * is sound (see the harness comment). */
#define GGAZE_SOF_JPEG_LEN 35

static void
_build_sof_jpeg(guint8 *p_out, guint16 u_w, guint16 u_h) {
   const guint8 head[GGAZE_SOF_JPEG_LEN] = {
      0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0,    0,    0,    0,    0x03,
      0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xFF, 0xDA, 0x00,
      0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00};
   memcpy(p_out, head, sizeof(head));
   p_out[7]  = (guint8)(u_h >> 8);
   p_out[8]  = (guint8)(u_h & 0xFF);
   p_out[9]  = (guint8)(u_w >> 8);
   p_out[10] = (guint8)(u_w & 0xFF);
}

/* detect_jpeg_peek_dims_from_path() must read min(file, 64 KB), not
 * "whatever the first read() returned", for the same reason as the sniff:
 * with a single g_input_stream_read() a 10-byte first chunk was scanned as
 * the whole file, its SOF payload was cut off, the peek reported NOT_JPEG
 * and loader_peek_dimensions() fell through to gdk-pixbuf's header parse
 * -- a third open, the very sandbox spawn the SOF peek exists to avoid,
 * and the wrong verdict for a FIFO, a pipe or a GVFS stream that happens
 * to deliver in pieces. With read_all the oversized header is seen whole
 * in session two and refused there: FALSE, and exactly two sessions
 * (sniff + SOF peek). Reverting detect.c to a single read makes this
 * assert 3. */
static void
test_fifo_two_chunk_jpeg_peek(void) {
   guint8 jpg[GGAZE_SOF_JPEG_LEN];
   _build_sof_jpeg(jpg, 65500, 65500);
   FifoWriter s_writer = {
      .p_img = jpg, .u_len = sizeof(jpg), .u_first_chunk = 10};
   g_assert_cmpuint(s_writer.u_len, <, GGAZE_DETECT_SNIFF_LEN);
   _fifo_start(&s_writer);

   GFile   *p_file = g_file_new_for_path(s_writer.c_fifo);
   int      i_w = -1, i_h = -1;
   gboolean b_ok    = loader_peek_dimensions(p_file, &i_w, &i_h);
   guint    u_opens = _fifo_finish(&s_writer);
   g_assert_false(b_ok);
   g_assert_cmpuint(u_opens, ==, 2);
   g_object_unref(p_file);
}

/* --- open counting on a regular file ------------------------------------ */

/* How many times a regular temp file is opened during one call: an
 * inotify watch on the file. The kernel queues IN_OPEN inside openat()
 * itself, synchronously, whoever the opener is (this process or a glycin
 * sandbox), so once the call under test has returned the queue holds
 * exactly its opens: no helper thread, no deadline, no assumption about
 * which end closes first. One kernel rule shapes the watch: inotify
 * COALESCES an event identical (wd, mask, cookie, name) to the one at the
 * tail of its unread queue, so two back-to-back IN_OPENs would count as
 * one (observed: 1 for the two opens of the oversized case). Watching
 * IN_CLOSE_NOWRITE as well keeps every IN_OPEN distinct, because each
 * open the loader makes is closed before the next -- the sniff stream is
 * unreffed before detect's peek opens, that closes before gdk-pixbuf's
 * fopen -- so the queue alternates OPEN, CLOSE, OPEN, CLOSE and nothing
 * merges; _open_counter_finish() asserts that alternation. The count is
 * the only proof from OUTSIDE the loader that gdk-pixbuf was NOT
 * consulted -- the sniff is one open, detect's SOF peek another, and
 * gdk_pixbuf_get_file_info() would be a third. Linux-only, like the app. */
typedef struct {
   gchar *c_path;
   int    i_inotify;
} OpenCounter;

static void
_open_counter_start(OpenCounter *p_c, const guint8 *p_buf, gsize u_len) {
   p_c->c_path    = write_tmp(p_buf, u_len);
   p_c->i_inotify = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
   _assert_inotify_ok(p_c->i_inotify, "inotify_init1");
   _assert_inotify_ok(inotify_add_watch(p_c->i_inotify, p_c->c_path,
                                        IN_OPEN | IN_CLOSE_NOWRITE),
                      "inotify_add_watch");
}

/* Drain the queue (non-blocking: EAGAIN is "empty", anything else a
 * failure), count the IN_OPEN events while asserting each is followed by
 * its IN_CLOSE_NOWRITE before the next (the alternation the comment above
 * relies on), tear down. A watch on a FILE never carries a name, so every
 * event is exactly sizeof(struct inotify_event). */
static guint
_open_counter_finish(OpenCounter *p_c) {
   struct inotify_event evs[64];
   guint                u_opens = 0;
   gboolean             b_open  = FALSE; /* an OPEN awaits its CLOSE */
   for (;;) {
      ssize_t i_n = read(p_c->i_inotify, evs, sizeof(evs));
      if (i_n < 0) {
         g_assert_cmpint(errno, ==, EAGAIN);
         break;
      }
      g_assert_cmpint(i_n % (ssize_t)sizeof(evs[0]), ==, 0);
      for (ssize_t i = 0; i < i_n / (ssize_t)sizeof(evs[0]); i++) {
         gboolean b_is_open = (evs[i].mask & IN_OPEN) != 0;
         g_assert_cmpint(b_is_open, !=, b_open);
         b_open = b_is_open;
         u_opens += b_is_open ? 1 : 0;
      }
   }
   g_assert_false(b_open);
   close(p_c->i_inotify);
   unlink(p_c->c_path);
   g_free(p_c->c_path);
   return (u_opens);
}

/* loader_peek_dimensions() on a regular file holding p_jpg: returns the
 * peek's verdict, stores the exact number of opens in *p_opens and asserts
 * the budget. */
static gboolean
_peek_counting_opens(const guint8 *p_jpg, gsize u_len, guint *p_opens) {
   OpenCounter s_counter;
   _open_counter_start(&s_counter, p_jpg, u_len);
   GFile   *p_file = g_file_new_for_path(s_counter.c_path);
   int      i_w = -1, i_h = -1;
   gint64   i_start = g_get_monotonic_time();
   gboolean b_ok    = loader_peek_dimensions(p_file, &i_w, &i_h);
   gdouble  d_secs  = (g_get_monotonic_time() - i_start) / 1e6;
   *p_opens         = _open_counter_finish(&s_counter);
   g_object_unref(p_file);
   g_assert_cmpfloat(d_secs, <, 5.0);
   return (b_ok);
}

/* A JPEG whose SOF declares 65500x65500 has no honest size, and the peek
 * must say so WITHOUT asking gdk-pixbuf: the old code returned a plain
 * FALSE for "oversized" and "SOF not in the prefix" alike and fell through
 * to gdk_pixbuf_get_file_info() -- a glycin sandbox spawn on Fedora >= 41
 * whose 65500x65500 answer was then rejected a second time. Exactly two
 * opens (sniff + SOF peek) prove the third call never happened; the two
 * tests right below prove the same counter does see the third open when
 * the peek legitimately defers. (An earlier version counted FIFO sessions
 * and could pass with 2 even when gdk-pixbuf WAS asked: see the harness
 * comment above.) */
static void
test_oversized_jpeg_peek_skips_pixbuf(void) {
   guint8 jpg[GGAZE_SOF_JPEG_LEN];
   _build_sof_jpeg(jpg, 65500, 65500);
   guint u_opens = 0;
   g_assert_false(_peek_counting_opens(jpg, sizeof(jpg), &u_opens));
   g_assert_cmpuint(u_opens, ==, 2);
}

/* A SOF declaring height 0 (a DNL marker would supply it later) is legal
 * JPEG that detect_jpeg_dims_within_bounds() deliberately lets through, so
 * the peek used to return TRUE with *p_h == 0. It must never report a zero
 * side: the verdict is FALSE, reached by deferring to gdk-pixbuf's header
 * parse (the third open) exactly as loader_load_pixbuf_scaled() lets the
 * real decoder judge such a file, and that parse's own > 0 check. */
static void
test_zero_height_jpeg_peek_is_false(void) {
   guint8 jpg[GGAZE_SOF_JPEG_LEN];
   _build_sof_jpeg(jpg, 6, 0);
   guint u_opens = 0;
   g_assert_false(_peek_counting_opens(jpg, sizeof(jpg), &u_opens));
   g_assert_cmpuint(u_opens, ==, 3);
}

/* A JPEG with no SOF at all (SOI, an APP0, then zero fill to the end of
 * the file): detect's scan reaches the end of the WHOLE file without a
 * frame header, a definitive "nothing to size here" that cannot hide a
 * large declared size, so the peek defers to gdk-pixbuf's header parse
 * (third open) -- the same courtesy loader_load_pixbuf_scaled() extends to
 * such a file -- which then fails it. Distinct from the padded case below,
 * where the scan ran out of PREFIX, not file. */
static void
test_sofless_jpeg_peek_defers_to_pixbuf(void) {
   guint8 jpg[64] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F',
                     'I',  'F',  0,    1,    1,    0,    0,   0};
   guint  u_opens = 0;
   g_assert_false(_peek_counting_opens(jpg, sizeof(jpg), &u_opens));
   g_assert_cmpuint(u_opens, ==, 3);
}

/* A SOF pushed past the 64 KB peek prefix by a maximal filler segment:
 * detect reports GGAZE_JPEG_PEEK_INCONCLUSIVE, and the peek fails closed
 * (FALSE, fast) like the thumbnail path does for the same file, instead of
 * handing the file to gdk-pixbuf to find the SOF for it. The declared size
 * behind the filler is a perfectly acceptable 6x3, so a fall-through would
 * show up as TRUE here -- but only if gdk-pixbuf's header parse got past
 * the filler, which is a decoder's choice; the open count is not: exactly
 * two (sniff + SOF peek), so gdk-pixbuf was never asked, whatever it
 * would have said. */
static void
test_padded_past_prefix_jpeg_peek_refused(void) {
   const guint16 u_seglen   = 0xFFFD; /* max marker segment length */
   GByteArray   *p_buf      = g_byte_array_new();
   const guint8  soi_app0[] = {0xFF,
                               0xD8,
                               0xFF,
                               0xE0,
                               (guint8)(u_seglen >> 8),
                               (guint8)(u_seglen & 0xFF)};
   g_byte_array_append(p_buf, soi_app0, sizeof(soi_app0));
   g_byte_array_set_size(p_buf, p_buf->len + u_seglen - 2); /* zero fill */
   guint8 tail[64];
   _build_sof_jpeg(tail, 6, 3);
   g_byte_array_append(p_buf, tail + 2, sizeof(tail) - 2); /* skip its SOI */
   g_assert_cmpuint(p_buf->len, >, GGAZE_JPEG_PEEK_LEN);

   guint u_opens = 0;
   g_assert_false(_peek_counting_opens(p_buf->data, p_buf->len, &u_opens));
   g_assert_cmpuint(u_opens, ==, 2);
   g_byte_array_unref(p_buf);
}

/* --- the pixbuf backend on a GFile served from memory ------------------ */

/* The backend's post-read cancel check (pixbuf.c: "last chance to honour a
 * superseded load") has a TRUE branch only a cancel that lands AFTER the
 * read's own pre-read check can reach -- i.e. while the final read() is in
 * flight. A cancel from another thread hits that window by luck; the
 * memory-served GFile (tests/helpers/mem_file.c) hits it every time by
 * cancelling from inside the stream's read_fn as it reports EOF. Control
 * first: unarmed, the file decodes as 1x1, which proves the stream path
 * is real and that a load is two opens (sniff, then backend). Then armed
 * on the fourth open overall -- the armed load's SECOND, so the
 * dispatcher's sniff (its first) passes untouched and it is the backend's
 * g_file_load_contents() that returns success on a GCancellable cancelled
 * meanwhile: the only place left to notice is the check under test, and
 * the verdict must be G_IO_ERROR_CANCELLED with nothing decoded. */
static void
test_cancel_between_read_and_decode(void) {
   if (!_pixbuf_module_usable("gif")) {
      return;
   }
   GFile      *p_file = ggtest_mem_file_new(TINY_GIF, sizeof(TINY_GIF));
   GError     *p_err  = NULL;
   GdkTexture *p_tex  = loader_load(p_file, NULL, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_tex);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 1);
   g_assert_cmpuint(ggtest_mem_file_opens(p_file), ==, 2);
   g_object_unref(p_tex);

   GCancellable *p_cancel = g_cancellable_new();
   ggtest_mem_file_cancel_at_eof(p_file, p_cancel, 4);
   p_tex = loader_load(p_file, p_cancel, &p_err);
   g_assert_null(p_tex);
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
   g_assert_true(g_cancellable_is_cancelled(p_cancel));
   g_assert_cmpuint(ggtest_mem_file_opens(p_file), ==, 4);
   g_error_free(p_err);
   g_object_unref(p_cancel);
   g_object_unref(p_file);
}

/* The dispatcher's sniff and the backend's read are two opens, so the
 * backend re-runs the gate on the bytes it actually loaded (pixbuf.c
 * _pixbuf_bytes_decodable()): called DIRECTLY -- as a swap of the file
 * between the two opens would in effect call it -- with a truncated JXL
 * and with nothing, it must refuse with the gate's INVALID_DATA and never
 * reach the GdkPixbufLoader (budget). */
static void
test_backend_regates_loaded_bytes(void) {
   const guint8 jxl[] = {0xFF, 0x0A, 0x10, 0x00};
   const struct {
      const char   *c_name;
      const guint8 *p_buf;
      gsize         u_len;
   } vecs[] = {
      {"truncated JXL", jxl, sizeof(jxl)},
      {"empty", jxl, 0},
   };
   for (gsize u = 0; u < G_N_ELEMENTS(vecs); u++) {
      g_test_message("%s", vecs[u].c_name);
      GFile      *p_file  = ggtest_mem_file_new(vecs[u].p_buf, vecs[u].u_len);
      gint64      i_start = g_get_monotonic_time();
      GError     *p_err   = NULL;
      GdkTexture *p_tex   = pixbuf_backend.load(p_file, NULL, &p_err);
      gdouble     d_secs  = (g_get_monotonic_time() - i_start) / 1e6;
      g_assert_null(p_tex);
      g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
      g_assert_cmpfloat(d_secs, <, 5.0);
      g_error_free(p_err);
      g_object_unref(p_file);
   }
}

/* The re-gate is exact only if it refuses a JXL in EVERY build. Without
 * libjxl the not-built-in rule does; with libjxl loader_sniff_bytes()
 * admits a JXL (the jxl backend decodes complete ones), so the fallback
 * must also refuse what a specific backend claims -- or a garbage JXL
 * swapped in between the dispatcher's sniff and the backend's read reaches
 * the GdkPixbufLoader and, on a glycin desktop, hangs it (measured:
 * forever). pixbuf_backend.load is called DIRECTLY with JXLs long enough
 * to clear the length gate -- garbage, and the smallest real codestream
 * and container, so the refusal is shown to be the gate's and not a
 * decoder's verdict -- and must fail with the dispatch rule's error
 * (libjxl) or the not-built-in one (no libjxl), within the budget. Before
 * the dispatch rule the garbage vector hung this test in the full build. */
static void
test_fallback_refuses_jxl_in_every_build(void) {
   guint8 garbage[60] = {0xFF, 0x0A};
   const struct {
      const char   *c_name;
      const guint8 *p_buf;
      gsize         u_len;
   } vecs[] = {
      {"garbage 60-byte codestream", garbage, sizeof(garbage)},
      {"smallest real codestream", TINY_JXL_CODESTREAM,
       sizeof(TINY_JXL_CODESTREAM)},
      {"smallest real container", TINY_JXL_CONTAINER,
       sizeof(TINY_JXL_CONTAINER)},
   };
   for (gsize u = 0; u < G_N_ELEMENTS(vecs); u++) {
      g_test_message("%s", vecs[u].c_name);
      GFile      *p_file  = ggtest_mem_file_new(vecs[u].p_buf, vecs[u].u_len);
      gint64      i_start = g_get_monotonic_time();
      GError     *p_err   = NULL;
      GdkTexture *p_tex   = pixbuf_backend.load(p_file, NULL, &p_err);
      gdouble     d_secs  = (g_get_monotonic_time() - i_start) / 1e6;
      g_assert_null(p_tex);
      g_assert_cmpfloat(d_secs, <, 5.0);
#if GGAZE_HAVE_JXL
      g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_BUSY);
      g_assert_nonnull(strstr(p_err->message, "changed while loading"));
      g_assert_nonnull(strstr(p_err->message, "JXL"));
#else
      g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
      g_assert_nonnull(strstr(p_err->message, "not built in"));
#endif
      g_error_free(p_err);
      g_object_unref(p_file);
   }
}

/* loader_sniff_bytes_for_fallback() on its own: the plain gate first (a
 * truncated JXL is INVALID_DATA, as before), then the dispatch rule --
 * bytes a specific backend of THIS build claims are refused with BUSY
 * and the status-line message naming the format, bytes only gdk-pixbuf
 * decodes pass. JPEG is the
 * build-dependent probe: refused with the jpeg backend, accepted without
 * it (there the fallback IS the JPEG decoder). PNG passes in every build. */
static void
test_fallback_gate_follows_dispatch(void) {
   const guint8 jxl4[] = {0xFF, 0x0A, 0x10, 0x00};
   GError      *p_err  = NULL;
   g_assert_false(loader_sniff_bytes_for_fallback(jxl4, sizeof(jxl4), &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
   g_clear_error(&p_err);

   g_assert_true(
      loader_sniff_bytes_for_fallback(TINY_PNG, sizeof(TINY_PNG), &p_err));
   g_assert_no_error(p_err);

   gboolean b_jpeg =
      loader_sniff_bytes_for_fallback(TINY_JPEG, sizeof(TINY_JPEG), &p_err);
#if GGAZE_HAVE_JPEG
   g_assert_false(b_jpeg);
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_BUSY);
   g_assert_cmpstr(p_err->message, ==,
                   "JPEG file changed while loading; try again");
   g_clear_error(&p_err);
#else
   g_assert_true(b_jpeg);
   g_assert_no_error(p_err);
#endif
}

/* loader_sniff_bytes() promises p_err on every FALSE (loader.h), the
 * caller-bug case included: a NULL buffer with a length is refused with
 * G_IO_ERROR_INVALID_ARGUMENT, not a silent FALSE -- and not a critical
 * either, which under g_test's fatal-criticals would have aborted here. A
 * NULL buffer with NO length is simply the empty file (INVALID_DATA). */
static void
test_sniff_bytes_null_buffer_is_invalid_argument(void) {
   GError *p_err = NULL;
   g_assert_false(loader_sniff_bytes(NULL, 4, NULL, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
   g_clear_error(&p_err);
   g_assert_false(loader_sniff_bytes(NULL, 0, NULL, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
   g_clear_error(&p_err);
   /* p_err may be NULL: still FALSE, still no critical. */
   g_assert_false(loader_sniff_bytes(NULL, 4, NULL, NULL));
}

/* The loader's own "the read itself failed" exits, which no disk in a
 * test reaches: the memory-served GFile fails every read on the Nth open.
 * Open one is the dispatcher's sniff (loader.c _read_header(): the error
 * is propagated as -1 and nothing is dispatched -- one open in total);
 * open three, on the re-armed file, is the next load's second open, the
 * pixbuf backend's g_file_load_contents() (its own error exit; the sniff
 * before it passed). Both surface the helper's error message unchanged,
 * so nothing in between wrapped or swallowed it. */
static void
test_read_failure_is_reported(void) {
   GFile *p_file = ggtest_mem_file_new(TINY_GIF, sizeof(TINY_GIF));
   ggtest_mem_file_fail_read(p_file, 1);
   GError     *p_err = NULL;
   GdkTexture *p_tex = loader_load(p_file, NULL, &p_err);
   g_assert_null(p_tex);
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED);
   g_assert_cmpstr(p_err->message, ==, GGTEST_MEM_FILE_READ_ERROR);
   g_assert_cmpuint(ggtest_mem_file_opens(p_file), ==, 1);
   g_clear_error(&p_err);

   ggtest_mem_file_fail_read(p_file, 3);
   p_tex = loader_load(p_file, NULL, &p_err);
   g_assert_null(p_tex);
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED);
   g_assert_cmpstr(p_err->message, ==, GGTEST_MEM_FILE_READ_ERROR);
   g_assert_cmpuint(ggtest_mem_file_opens(p_file), ==, 3);
   g_clear_error(&p_err);
   g_object_unref(p_file);
}

/* loader_load_pixbuf_scaled() decodes by PATH (gdk_pixbuf_new_from_file_
 * at_scale), so a GFile without one is refused -- after the sniff, which
 * the memory-served file passes, so it is this exit and not the gate's
 * that answers (one open: the sniff), with a G_IO_ERROR_FAILED that names
 * the reason. */
static void
test_scaled_refuses_non_local_file(void) {
   GFile     *p_file = ggtest_mem_file_new(TINY_PNG, sizeof(TINY_PNG));
   GError    *p_err  = NULL;
   GdkPixbuf *p_pix  = loader_load_pixbuf_scaled(p_file, 128, NULL, &p_err);
   g_assert_null(p_pix);
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED);
   g_assert_nonnull(strstr(p_err->message, "non-local"));
   g_assert_cmpuint(ggtest_mem_file_opens(p_file), ==, 1);
   g_error_free(p_err);
   g_object_unref(p_file);
}

/* pixbuf_util_decode_bytes()'s write-failure exit. A GdkPixbufLoader
 * matches a module once it holds enough header (its 4 KiB sniff buffer);
 * fed MORE than that with no recognisable signature, the write itself
 * fails (GDK_PIXBUF_ERROR_UNKNOWN_TYPE), where a shorter unrecognised
 * buffer fails only at close (test_one_byte_file goes that way). That exit
 * must still close the loader (gdk-pixbuf warns about an unclosed one at
 * finalize, fatal under g_test) and report the write's error. Bytes with
 * no signature are not constrained by the gate, so the verdict really is
 * gdk-pixbuf's. */
static void
test_decode_bytes_fails_at_write_on_unrecognised(void) {
   const gsize u_len = 5000;
   guint8     *p_buf = g_malloc(u_len);
   memset(p_buf, 'x', u_len);
   GError    *p_err = NULL;
   GdkPixbuf *p_pix = pixbuf_util_decode_bytes(p_buf, u_len, &p_err);
   g_assert_null(p_pix);
   g_assert_error(p_err, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_UNKNOWN_TYPE);
   g_error_free(p_err);
   g_free(p_buf);
}

/* loader_load_async()/loader_load_finish(): the GTask wrapper the window
 * uses, driven to completion on a main loop. A progress callback is passed
 * so the ProgressPair path is exercised; the pixbuf backend has no
 * progressive load, so it is never called. */
static void
_async_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   GAsyncResult **pp_out = (GAsyncResult **)p_data;
   *pp_out               = g_object_ref(p_res);
}

static void
_never_progress_cb(GdkTexture *p_partial, gpointer p_data) {
   (void)p_partial;
   (void)p_data;
   g_assert_not_reached();
}

static GdkTexture *
_load_async_sync(GFile *p_file, GError **p_err) {
   GAsyncResult *p_res = NULL;
   loader_load_async(p_file, NULL, _never_progress_cb, NULL, _async_done_cb,
                     &p_res);
   while (p_res == NULL) {
      g_main_context_iteration(NULL, TRUE);
   }
   GdkTexture *p_tex = loader_load_finish(p_res, p_err);
   g_object_unref(p_res);
   return (p_tex);
}

static void
test_load_async(void) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar      *c_path = g_build_filename(c_dir, "small.png", NULL);
   GFile      *p_file = g_file_new_for_path(c_path);
   GError     *p_err  = NULL;
   GdkTexture *p_tex  = _load_async_sync(p_file, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_tex);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 5);
   g_object_unref(p_tex);
   g_object_unref(p_file);
   g_free(c_path);

   /* The failure path carries the sniff's error through the task. */
   gchar *c_empty = write_tmp((const guint8 *)"", 0);
   p_file         = g_file_new_for_path(c_empty);
   p_tex          = _load_async_sync(p_file, &p_err);
   g_assert_null(p_tex);
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
   g_error_free(p_err);
   g_object_unref(p_file);
   unlink(c_empty);
   g_free(c_empty);
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
 * parse is what fails); loader_load_pixbuf_scaled() on the same text file
 * fails with gdk-pixbuf's own error, not the gate's. */
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
   GError    *p_err = NULL;
   GdkPixbuf *p_pix = loader_load_pixbuf_scaled(p_file, 128, NULL, &p_err);
   g_assert_null(p_pix);
   g_assert_nonnull(p_err);
   g_assert_false(g_error_matches(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA));
   g_error_free(p_err);
   g_object_unref(p_file);
   unlink(c_path);
   g_free(c_path);
}

/* Registration is split by theme so no function approaches the 50-line
 * mark (c-best-practices). */
static void
_add_fixture_tests(void) {
   g_test_add_func("/loader/pixbuf/plain_jpeg", test_plain_jpeg);
   g_test_add_func("/loader/pixbuf/rotated_exif", test_rotated_exif_jpeg);
   g_test_add_func("/loader/pixbuf/png", test_png);
   g_test_add_func("/loader/pixbuf/rgba_png", test_rgba_png);
   g_test_add_func("/loader/pixbuf/missing_file", test_missing_file_errors);
   g_test_add_func("/loader/pixbuf/corrupt_jpeg", test_corrupt_jpeg);
   g_test_add_func("/loader/pixbuf/oversized_jpeg", test_oversized_jpeg);
   g_test_add_func("/loader/pixbuf/load_async", test_load_async);
   g_test_add_func("/loader/pixbuf/cancelled_before_decode",
                   test_cancelled_before_decode);
   g_test_add_func("/loader/pixbuf/pixbuf_util", test_pixbuf_util);
   g_test_add_func("/loader/pixbuf/decode_bytes_fails_at_write_on_unrecognised",
                   test_decode_bytes_fails_at_write_on_unrecognised);
}

static void
_add_gate_tests(void) {
   g_test_add_func("/loader/pixbuf/unsupported_jxl", test_unsupported_jxl);
   g_test_add_func("/loader/pixbuf/unsupported_avif", test_unsupported_avif);
   g_test_add_func("/loader/pixbuf/unsupported_heif", test_unsupported_heif);
   g_test_add_func("/loader/pixbuf/truncated_signatures",
                   test_truncated_signatures);
   g_test_add_func("/loader/pixbuf/one_byte_file", test_one_byte_file);
   g_test_add_func("/loader/pixbuf/empty_file_sets_error",
                   test_empty_file_sets_error);
   g_test_add_func("/loader/pixbuf/garbage_jxl_fails_fast",
                   test_garbage_jxl_fails_fast);
   g_test_add_func("/loader/pixbuf/tiny_images_load", test_tiny_images_load);
   g_test_add_func("/loader/pixbuf/truncated_thumbnail_and_peek",
                   test_truncated_thumbnail_and_peek);
   g_test_add_func("/loader/pixbuf/thumbnail_and_peek_still_work",
                   test_thumbnail_and_peek_still_work);
   g_test_add_func("/loader/pixbuf/sniff_bytes_null_buffer_is_invalid_argument",
                   test_sniff_bytes_null_buffer_is_invalid_argument);
   g_test_add_func("/loader/pixbuf/fallback_gate_follows_dispatch",
                   test_fallback_gate_follows_dispatch);
}

static void
_add_peek_tests(void) {
   g_test_add_func("/loader/pixbuf/fifo_two_chunk_read",
                   test_fifo_two_chunk_read);
   g_test_add_func("/loader/pixbuf/fifo_two_chunk_jpeg_peek",
                   test_fifo_two_chunk_jpeg_peek);
   g_test_add_func("/loader/pixbuf/oversized_jpeg_peek_skips_pixbuf",
                   test_oversized_jpeg_peek_skips_pixbuf);
   g_test_add_func("/loader/pixbuf/zero_height_jpeg_peek_is_false",
                   test_zero_height_jpeg_peek_is_false);
   g_test_add_func("/loader/pixbuf/sofless_jpeg_peek_defers_to_pixbuf",
                   test_sofless_jpeg_peek_defers_to_pixbuf);
   g_test_add_func("/loader/pixbuf/padded_past_prefix_jpeg_peek_refused",
                   test_padded_past_prefix_jpeg_peek_refused);
   g_test_add_func("/loader/pixbuf/peek_dimensions_unknown_cases",
                   test_peek_dimensions_unknown_cases);
}

/* The pixbuf backend on a GFile served from memory (mem_file.h). */
static void
_add_stream_tests(void) {
   g_test_add_func("/loader/pixbuf/cancel_between_read_and_decode",
                   test_cancel_between_read_and_decode);
   g_test_add_func("/loader/pixbuf/backend_regates_loaded_bytes",
                   test_backend_regates_loaded_bytes);
   g_test_add_func("/loader/pixbuf/fallback_refuses_jxl_in_every_build",
                   test_fallback_refuses_jxl_in_every_build);
   g_test_add_func("/loader/pixbuf/read_failure_is_reported",
                   test_read_failure_is_reported);
   g_test_add_func("/loader/pixbuf/scaled_refuses_non_local_file",
                   test_scaled_refuses_non_local_file);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   _add_fixture_tests();
   _add_gate_tests();
   _add_peek_tests();
   _add_stream_tests();
   return (g_test_run());
}
