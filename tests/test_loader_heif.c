/*:*
 * ggaze — HEIF/HEIC loader backend unit test (libheif lane)
 *
 * Loads a committed HEIC fixture via loader_load() and asserts the resulting
 * GdkTexture dimensions. Critically, the texture must OWN its pixel memory:
 * the HEIF backend used to wrap libheif's decoded plane (owned by the
 * heif_image object) with a non-owning g_bytes_new_static and then release
 * that heif_image, leaving GdkMemoryTexture backed by freed memory
 * (use-after-free — task 7u0). This test drops the texture after touching it
 * so ASan/LeakSanitizer catch any UAF or leak. Only compiled/linked when the
 * meson feature `heif` is enabled (heif_dep.found()). No display needed.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "loader/backends/heif_internal.h"
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
test_heif_dims_and_lifetime(void) {
   /* tiny.heic is a 4x3 lossless HEIC (HEVC still picture). */
   GdkTexture *p_tex = load_fixture("tiny.heic");
   g_assert_nonnull(p_tex);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 4);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, 3);

   /* Touch every pixel through a downloaded snapshot so the pixel memory is
    * actually read; if the texture were backed by freed decoder memory (the
    * original bug), ASan flags the read-after-free right here rather than
    * on some unrelated later render or texturecache access. */
   GdkTextureDownloader *p_dl     = gdk_texture_downloader_new(p_tex);
   gsize                 u_stride = 0;
   GBytes *p_bytes = gdk_texture_downloader_download_bytes(p_dl, &u_stride);
   g_assert_nonnull(p_bytes);
   g_assert_cmpuint(u_stride, >=, (guint)4 * 4u);
   g_assert_cmpuint(g_bytes_get_size(p_bytes), >=, (guint)4 * 4u * 3u);

   gsize         u_len  = 0;
   const guint8 *p_data = g_bytes_get_data(p_bytes, &u_len);

   /* tiny.heic DOES carry a real embedded alpha auxiliary image
    * (urn:mpeg:hevc:2015:auxid:1) — heif_image_handle_has_alpha_channel()
    * returns TRUE for this fixture, confirmed independently while fixing
    * this comment. Every decoded alpha byte comes out 0xFF because that is
    * what the embedded alpha auxiliary image actually decodes to (it was
    * encoded fully opaque), not because alpha is absent and libheif is
    * synthesizing a default. This is still a real content invariant of the
    * fixture (not just "the memory is nonzero"), and it reproduces the
    * original bug reliably: verified against the pre-fix backend, the
    * freed/reused heif_image buffer came back with its first pixels' bytes
    * zeroed (including alpha), while ASan itself does not always flag the
    * read because the freed 528-byte plane (stride=256 * height=3, not 48
    * bytes as an earlier commit message said) gets silently reused by an
    * unrelated small GLib allocation before this download runs. Checking
    * content therefore catches the use-after-free even on runs where ASan
    * alone would not. */
   for (gsize u_row = 0; u_row < 3; u_row++) {
      for (gsize u_col = 0; u_col < 4; u_col++) {
         gsize u_off = u_row * u_stride + u_col * 4;
         g_assert_cmpuint(u_off + 3, <, u_len);
         g_assert_cmpuint(p_data[u_off + 3], ==, 0xFF);
      }
   }

   g_bytes_unref(p_bytes);
   gdk_texture_downloader_free(p_dl);
   g_object_unref(p_tex);
}

static void
test_heif_corrupt(void) {
   /* Truncated tiny.heic: ftyp + a few bytes of meta, no coded image data ->
    * decode error, never a crash. */
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar *c_path = g_build_filename(c_dir, "tiny.heic", NULL);
   gchar *c_buf  = NULL;
   gsize  u_len  = 0;
   g_assert_true(g_file_get_contents(c_path, &c_buf, &u_len, NULL));
   g_free(c_path);
   gsize u_cut = (u_len < 40) ? u_len : 40;

   gchar  *c_tmp = NULL;
   GError *p_sub = NULL;
   gint    i_fd  = g_file_open_tmp("ggaze-heif-XXXXXX", &c_tmp, &p_sub);
   g_assert_no_error(p_sub);
   g_assert_cmpint(i_fd, >=, 0);
   g_assert_cmpint((glong)write(i_fd, c_buf, u_cut), ==, (glong)u_cut);
   close(i_fd);
   g_free(c_buf);

   GFile      *p_file = g_file_new_for_path(c_tmp);
   GError     *p_err  = NULL;
   GdkTexture *p_tex  = loader_load(p_file, NULL, &p_err);
   g_assert_null(p_tex);
   g_assert_nonnull(p_err);
   g_error_free(p_err);
   g_object_unref(p_file);
   unlink(c_tmp);
   g_free(c_tmp);
}

static void
test_heif_check_plane_guards(void) {
   /* Direct boundary-value coverage of _heif_check_plane()'s validation
    * branches (task 7u0 review). These guard decoder-reported geometry
    * *after* a successful decode, so a real HEIF bitstream cannot be
    * crafted to trip them the way tests/test_loader_jxl.c crafts an
    * oversized codestream for _jxl_check_dims(): libheif's own decode
    * success already implies internally-consistent width/height/stride.
    * _heif_check_plane() is a pure function of primitives with no
    * heif_image dependency (see heif_internal.h), so it is called here
    * directly with crafted boundary ints — this exercises the exact guard
    * lines in heif.c with real assertions on the resulting GError, rather
    * than faking coverage with a tautological check. */
   gsize   u_len = 0;
   GError *p_err = NULL;

   /* Zero / negative width or height. */
   g_assert_false(_heif_check_plane(0, 3, 16, &u_len, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED);
   g_clear_error(&p_err);

   g_assert_false(_heif_check_plane(-1, 3, 16, &u_len, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED);
   g_clear_error(&p_err);

   g_assert_false(_heif_check_plane(4, 0, 16, &u_len, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED);
   g_clear_error(&p_err);

   g_assert_false(_heif_check_plane(4, -2, 16, &u_len, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED);
   g_clear_error(&p_err);

   /* Zero / negative stride. */
   g_assert_false(_heif_check_plane(4, 3, 0, &u_len, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED);
   g_clear_error(&p_err);

   g_assert_false(_heif_check_plane(4, 3, -16, &u_len, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED);
   g_clear_error(&p_err);

   /* Stride narrower than one RGBA row (row = 4 * 4 = 16 bytes; 8 is
    * short). */
   g_assert_false(_heif_check_plane(4, 3, 8, &u_len, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED);
   g_clear_error(&p_err);

   /* Positive control: valid geometry succeeds and computes the exact byte
    * length ((h-1)*stride + row_bytes), matching tiny.heic's own real
    * stride=256/height=3 shape used in test_heif_dims_and_lifetime(). */
   u_len = 0;
   g_assert_true(_heif_check_plane(4, 3, 256, &u_len, &p_err));
   g_assert_no_error(p_err);
   g_assert_cmpuint(u_len, ==, (gsize)(2 * 256 + 16));

   /* Stride exactly equal to the row width is the valid boundary case, not
    * a rejection. */
   u_len = 0;
   g_assert_true(_heif_check_plane(4, 3, 16, &u_len, &p_err));
   g_assert_no_error(p_err);
   g_assert_cmpuint(u_len, ==, (gsize)(2 * 16 + 16));

   /* The gsize-overflow branch (u_len > G_MAXSIZE) is intentionally NOT
    * exercised here: on every 64-bit build (gsize == 64-bit == the width of
    * u_len's own guint64 accumulator), and with i_h/i_stride bounded by
    * `int`, u_len can never exceed G_MAXSIZE — the comparison is
    * unreachable by construction, not merely hard to trigger. See the
    * comment at that guard in heif.c for the full argument; it is kept
    * live only for 32-bit gsize portability. */
}

static void
test_heif_check_plane_data(void) {
   /* _heif_check_plane_data() rejects a NULL decoder plane pointer and
    * accepts a non-NULL one. Split out of _heif_build_texture() precisely
    * so this branch — previously untested per the task 7u0 review, since
    * triggering a NULL plane through a real decode is not something a
    * crafted HEIF bitstream can reliably do when we explicitly request
    * interleaved RGBA chroma — gets real direct coverage instead of being
    * left silently unverified. */
   GError *p_err = NULL;
   g_assert_false(_heif_check_plane_data(NULL, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED);
   g_clear_error(&p_err);

   guint8 u_dummy = 0;
   g_assert_true(_heif_check_plane_data(&u_dummy, &p_err));
   g_assert_no_error(p_err);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/loader/heif/dims_and_lifetime",
                   test_heif_dims_and_lifetime);
   g_test_add_func("/loader/heif/corrupt", test_heif_corrupt);
   g_test_add_func("/loader/heif/check_plane_guards",
                   test_heif_check_plane_guards);
   g_test_add_func("/loader/heif/check_plane_data", test_heif_check_plane_data);
   return (g_test_run());
}
