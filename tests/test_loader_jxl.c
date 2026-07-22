/*:*
 * ggaze — JPEG XL loader backend unit test (libjxl lane)
 *
 * Loads a committed JXL fixture via loader_load() and asserts the resulting
 * GdkTexture dimensions, then exercises the safety paths the backend hardened
 * for task nu0:
 *   - corrupt/truncated JXL must return a recoverable GError, not crash;
 *   - a hand-crafted codestream whose SizeHeader declares enormous xsize/
 *     ysize must be rejected (GError) before any allocation — the decoder
 *     rule: bad files never crash, never abort on OOM.
 * The oversized codestream is built in-test with a LSB-first bit writer so no
 * binary fixture is required for it. Only compiled/linked when the meson
 * feature `jxl` is enabled (jxl_dep.found()). No display needed.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "loader/loader.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <unistd.h>

/* --- LSB-first bit writer for crafting a JXL codestream header ------------ */

typedef struct {
   guint8 *p_buf;
   gsize   u_cap;
   gsize   u_bit;
} BitWriter;

static void
bw_put(BitWriter *p_w, guint64 u_val, guint u_nbits) {
   for (guint i = 0; i < u_nbits; i++) {
      gsize u_byte = p_w->u_bit / 8;
      gsize u_off  = p_w->u_bit % 8;
      g_assert_cmpuint(u_byte, <, p_w->u_cap);
      if ((u_val >> i) & 1u) {
         p_w->p_buf[u_byte] |= (guint8)(1u << u_off);
      }
      p_w->u_bit++;
   }
}

/* Build a JXL codestream whose SizeHeader declares huge xsize/ysize (1<<28
 * each), followed by an all-default ImageMetadata and garbage padding. The
 * decoder emits BASIC_INFO with the huge dims; the backend must reject them
 * before allocating. Returns the stream length and writes into *pp_buf (the
 * caller must g_free it). */
static gsize
_build_oversized_jxl(guint8 **pp_buf) {
   gsize     u_cap = 64;
   guint8   *p_buf = g_malloc0(u_cap);
   BitWriter st_w  = {p_buf, u_cap, 0};
   /* Codestream signature: 0xFF 0x0A. */
   bw_put(&st_w, 0xff, 8);
   bw_put(&st_w, 0x0a, 8);
   /* SizeHeader: small = 0 (1 bit). */
   bw_put(&st_w, 0, 1);
   /* ysize_minus_1 via U32(BitsOffset(9,1),BitsOffset(13,1),BitsOffset(18,1),
    * BitsOffset(30,1)); selector 3 -> 30 bits, ysize = value + 1. */
   bw_put(&st_w, 3, 2);
   bw_put(&st_w, (1u << 28) - 1u, 30);
   /* ratio = 0 -> explicit xsize follows. */
   bw_put(&st_w, 0, 3);
   /* xsize_minus_1, same distribution. */
   bw_put(&st_w, 3, 2);
   bw_put(&st_w, (1u << 28) - 1u, 30);
   /* ImageMetadata: all_default = 1 (defaults, no preview/animation). */
   bw_put(&st_w, 1, 1);
   /* Garbage padding so the stream is not prematurely empty. */
   gsize u_head = (st_w.u_bit + 7) / 8;
   for (gsize i = u_head; i < u_head + 8; i++) {
      g_assert_cmpuint(i, <, u_cap);
      p_buf[i] = 0xff;
   }
   *pp_buf = p_buf;
   return (u_head + 8);
}

/* Write p_len bytes to a unique temp file and return its path (caller frees).*/
static gchar *
_write_tmp(const guint8 *p_data, gsize u_len) {
   gchar  *c_tmp = NULL;
   GError *p_sub = NULL;
   gint    i_fd  = g_file_open_tmp("ggaze-jxl-XXXXXX", &c_tmp, &p_sub);
   g_assert_no_error(p_sub);
   g_assert_cmpint(i_fd, >=, 0);
   g_assert_cmpint((glong)write(i_fd, p_data, u_len), ==, (glong)u_len);
   close(i_fd);
   return (c_tmp);
}

static void
test_jxl_dims(void) {
   /* tiny.jxl is a 4x3 JXL. */
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar *c_path = g_build_filename(c_dir, "tiny.jxl", NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);
   GError     *p_err = NULL;
   GdkTexture *p_tex = loader_load(p_file, NULL, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_tex);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 4);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, 3);
   /* Touch the pixel memory through a downloaded snapshot so ASan catches a
    * UAF if the texture is backed by freed memory. */
   GdkTextureDownloader *p_dl     = gdk_texture_downloader_new(p_tex);
   gsize                 u_stride = 0;
   GBytes *p_bytes = gdk_texture_downloader_download_bytes(p_dl, &u_stride);
   g_assert_nonnull(p_bytes);
   g_assert_cmpuint(u_stride, >=, (guint)4 * 4u);
   g_assert_cmpuint(g_bytes_get_size(p_bytes), >=, (guint)4 * 4u * 3u);
   g_bytes_unref(p_bytes);
   gdk_texture_downloader_free(p_dl);
   g_object_unref(p_tex);
   g_object_unref(p_file);
}

static void
test_jxl_corrupt(void) {
   /* Truncated tiny.jxl: signature + a few bytes, no image data -> error. */
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   gchar *c_path = g_build_filename(c_dir, "tiny.jxl", NULL);
   gchar *c_buf  = NULL;
   gsize  u_len  = 0;
   g_assert_true(g_file_get_contents(c_path, &c_buf, &u_len, NULL));
   g_free(c_path);
   gsize  u_cut = (u_len < 8) ? u_len : 8;
   gchar *c_tmp = _write_tmp((const guint8 *)c_buf, u_cut);
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
test_jxl_oversized(void) {
   /* Codestream declaring 1<<28 x 1<<28 pixels must be rejected with a
    * recoverable GError, never an abort or huge allocation. */
   guint8 *p_data = NULL;
   gsize   u_len  = _build_oversized_jxl(&p_data);
   gchar  *c_tmp  = _write_tmp(p_data, u_len);
   g_free(p_data);
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
   g_test_add_func("/loader/jxl/dims", test_jxl_dims);
   g_test_add_func("/loader/jxl/corrupt", test_jxl_corrupt);
   g_test_add_func("/loader/jxl/oversized", test_jxl_oversized);
   return (g_test_run());
}