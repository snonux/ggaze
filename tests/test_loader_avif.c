/*:*
 * ggaze — AVIF loader backend unit test (libavif lane)
 *
 * Loads a committed AVIF fixture via loader_load() and asserts the resulting
 * GdkTexture dimensions. Critically, the texture must OWN its pixel memory:
 * the AVIF backend used to wrap libavif's pixel buffer with a non-owning
 * g_bytes_new_static and then freed that buffer, leaving GdkMemoryTexture
 * backed by freed memory (use-after-free). This test drops the texture after
 * touching it so ASan catches any UAF. Only compiled/linked when the meson
 * feature `avif` is enabled (avif_dep.found()). No display needed.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

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
test_avif_dims(void) {
   /* tiny.avif is a 4x3 AVIF. */
   GdkTexture *p_tex = load_fixture("tiny.avif");
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 4);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, 3);
   /* Touch a few bytes through a downloaded snapshot so the pixel memory is
    * actually read; if the texture backed freed memory, ASan flags it here. */
   GdkTextureDownloader *p_dl     = gdk_texture_downloader_new(p_tex);
   gsize                 u_stride = 0;
   GBytes *p_bytes = gdk_texture_downloader_download_bytes(p_dl, &u_stride);
   g_assert_nonnull(p_bytes);
   g_assert_cmpuint(u_stride, >=, (guint)4 * 4u);
   g_assert_cmpuint(g_bytes_get_size(p_bytes), >=, (guint)4 * 4u * 3u);
   g_bytes_unref(p_bytes);
   gdk_texture_downloader_free(p_dl);
   g_object_unref(p_tex);
}

static void
test_avif_corrupt(void) {
   /* Truncated AVIF: ftyp box only, no image data -> decode error. */
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar *c_path = g_build_filename(c_dir, "tiny.avif", NULL);
   gchar *c_buf  = NULL;
   gsize  u_len  = 0;
   g_assert_true(g_file_get_contents(c_path, &c_buf, &u_len, NULL));
   g_free(c_path);
   /* Keep only the first 20 bytes (ftyp + part of meta) -> parse/decode fail.
    */
   gsize u_cut = (u_len < 20) ? u_len : 20;

   gchar  *c_tmp = NULL;
   GError *p_sub = NULL;
   gint    i_fd  = g_file_open_tmp("ggaze-avif-XXXXXX", &c_tmp, &p_sub);
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

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/loader/avif/dims", test_avif_dims);
   g_test_add_func("/loader/avif/corrupt", test_avif_corrupt);
   return (g_test_run());
}