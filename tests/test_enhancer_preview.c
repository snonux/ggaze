/*:*
 * ggaze — the live preview's source and render (GEGL, no display)
 *
 * 8l2, decision #53: the edit panel's preview renders from a SOURCE -- the
 * image decoded once and scaled down to the view -- and only the export
 * runs at full resolution. Pinned here:
 *
 *   - a source at full size renders exactly what the full-resolution chain
 *     does (the same pixels), transform and pixel-length preset included;
 *   - a capped source is the image scaled, its render stands for the
 *     export's size (logical-size.h) with the crop scaled onto it, and its
 *     own pixels stand for the original (the hold-Space compare);
 *   - a source built from the viewer's decode reads nothing from the file
 *     (no loader decode) and holds the same pixels as one decoded from it;
 *   - a preset's pixel lengths scale with the source: a Gaussian blur's
 *     edge on a quarter-size source is as wide, in image pixels, as the
 *     full-resolution one -- unscaled it would be four times wider;
 *   - a colour-managed file's scaled source is the managed decode;
 *   - which views a source serves; the card thumbnails come from the
 *     source at thumbnail size; the test seams (delay, failure).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "enhancer-gegl.h"
#include "enhancer.h"
#include "logical-size.h"
#include "preview-scale.h"
#include "transform.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

/* The PNG every subtest renders: 400x300, left half dark, right half
 * light, with a colour ramp down the rows so a crop or a turn shows. */
#define IMG_W 400
#define IMG_H 300

typedef struct {
   char  *c_dir;
   char  *c_path;
   GFile *p_file;
} Fx;

static void
fx_open(Fx *p_fx) {
   GError *p_err = NULL;
   p_fx->c_dir   = g_dir_make_tmp("ggaze-prevsrc-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   p_fx->c_path    = g_build_filename(p_fx->c_dir, "edge.png", NULL);
   GdkPixbuf *p_pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, IMG_W, IMG_H);
   guint8    *p_px = gdk_pixbuf_get_pixels(p_pb);
   gint       i_rs = gdk_pixbuf_get_rowstride(p_pb);
   for (gint y = 0; y < IMG_H; y++) {
      for (gint x = 0; x < IMG_W; x++) {
         guint8 *p = p_px + y * i_rs + x * 3;
         p[0]      = x < IMG_W / 2 ? 10 : 245;
         p[1]      = (guint8)(y * 255 / (IMG_H - 1));
         p[2]      = x < IMG_W / 2 ? 30 : 200;
      }
   }
   g_assert_true(gdk_pixbuf_save(p_pb, p_fx->c_path, "png", &p_err, NULL));
   g_assert_no_error(p_err);
   g_object_unref(p_pb);
   p_fx->p_file = g_file_new_for_path(p_fx->c_path);
}

static void
fx_close(Fx *p_fx) {
   g_object_unref(p_fx->p_file);
   g_remove(p_fx->c_path);
   g_rmdir(p_fx->c_dir);
   g_free(p_fx->c_path);
   g_free(p_fx->c_dir);
}

typedef struct {
   GMainLoop *p_loop;
   gpointer   p_out;
   GError    *p_err;
} Wait;

static void
source_cb(GObject *p_obj, GAsyncResult *p_res, gpointer p_data) {
   (void)p_obj;
   Wait *p_w  = p_data;
   p_w->p_out = enhancer_source_new_finish(p_res, &p_w->p_err);
   g_main_loop_quit(p_w->p_loop);
}

static void
render_cb(GObject *p_obj, GAsyncResult *p_res, gpointer p_data) {
   (void)p_obj;
   Wait *p_w  = p_data;
   p_w->p_out = enhancer_source_render_finish(p_res, &p_w->p_err);
   g_main_loop_quit(p_w->p_loop);
}

static void
thumbs_cb(GObject *p_obj, GAsyncResult *p_res, gpointer p_data) {
   (void)p_obj;
   Wait *p_w  = p_data;
   p_w->p_out = enhancer_preview_thumbnails_finish(p_res, &p_w->p_err);
   g_main_loop_quit(p_w->p_loop);
}

/* p_file's source for a default view capped at i_cap (0: none), from
 * p_decoded when given. */
static EnhancerSource *
source_for(GFile *p_file, GdkTexture *p_decoded, gint i_cap) {
   PreviewView t_view = {0, 0, 1, i_cap};
   Wait        w      = {.p_loop = g_main_loop_new(NULL, FALSE)};
   enhancer_source_new_async(p_file, p_decoded, &t_view, NULL, source_cb, &w);
   g_main_loop_run(w.p_loop);
   g_main_loop_unref(w.p_loop);
   g_assert_no_error(w.p_err);
   g_assert_nonnull(w.p_out);
   return (w.p_out);
}

/* Render u_mask (default strengths) and p_xf on p_src; NULL with *pp_err
 * when it fails. */
static GdkTexture *
render_on(const EnhancerSource *p_src, guint32 u_mask, const Transform *p_xf,
          GError **pp_err) {
   Enhancer  *p_e = enhancer_new();
   GPtrArray *p_presets =
      enhancer_presets_resolve(enhancer_get_presets(p_e), NULL);
   Wait w = {.p_loop = g_main_loop_new(NULL, FALSE)};
   enhancer_source_render_async(p_src, p_presets, u_mask, p_xf, NULL, render_cb,
                                &w);
   g_main_loop_run(w.p_loop);
   g_main_loop_unref(w.p_loop);
   g_ptr_array_unref(p_presets);
   enhancer_delete(p_e);
   if (w.p_err != NULL) {
      g_propagate_error(pp_err, w.p_err);
   }
   return (w.p_out);
}

/* The full-resolution chain the export runs, as a texture. */
static GdkTexture *
full_chain(GFile *p_file, guint32 u_mask, const Transform *p_xf) {
   Enhancer  *p_e = enhancer_new();
   GPtrArray *p_presets =
      enhancer_presets_resolve(enhancer_get_presets(p_e), NULL);
   GError     *p_err = NULL;
   GeglBuffer *p_in  = enhancer_load(p_file, &p_err);
   g_assert_no_error(p_err);
   GeglBuffer *p_out =
      enhancer_apply_chain(p_in, p_presets, u_mask, p_xf, &p_err);
   g_assert_no_error(p_err);
   GdkTexture *p_tex = enhancer_buffer_to_texture(p_out, &p_err);
   g_assert_no_error(p_err);
   g_object_unref(p_out);
   g_object_unref(p_in);
   g_ptr_array_unref(p_presets);
   enhancer_delete(p_e);
   return (p_tex);
}

/* p_tex's pixels as R8G8B8A8 bytes (caller unrefs), *p_stride set. */
static GBytes *
pixels(GdkTexture *p_tex, gsize *p_stride) {
   GdkTextureDownloader *p_dl = gdk_texture_downloader_new(p_tex);
   gdk_texture_downloader_set_format(p_dl, GDK_MEMORY_R8G8B8A8);
   GBytes *p_b = gdk_texture_downloader_download_bytes(p_dl, p_stride);
   gdk_texture_downloader_free(p_dl);
   return (p_b);
}

/* Channel c of pixel (x, y). */
static guint8
px(GdkTexture *p_tex, gint i_x, gint i_y, gint i_c) {
   gsize         u_stride = 0;
   GBytes       *p_b      = pixels(p_tex, &u_stride);
   const guint8 *p_d      = g_bytes_get_data(p_b, NULL);
   guint8        u_v      = p_d[(gsize)i_y * u_stride + (gsize)i_x * 4 + i_c];
   g_bytes_unref(p_b);
   return (u_v);
}

static void
assert_same_pixels(GdkTexture *p_a, GdkTexture *p_b) {
   g_assert_cmpint(gdk_texture_get_width(p_a), ==, gdk_texture_get_width(p_b));
   g_assert_cmpint(gdk_texture_get_height(p_a), ==,
                   gdk_texture_get_height(p_b));
   gsize   u_sa = 0, u_sb = 0;
   GBytes *p_ba = pixels(p_a, &u_sa);
   GBytes *p_bb = pixels(p_b, &u_sb);
   g_assert_cmpuint(u_sa, ==, u_sb);
   g_assert_true(g_bytes_equal(p_ba, p_bb));
   g_bytes_unref(p_ba);
   g_bytes_unref(p_bb);
}

/* Sharpen (bit 6) and Brightness (bit 1), a quarter turn and a crop: at
 * full size the source's render is the export's chain, pixel for pixel,
 * and stands for its own size. */
static void
test_full_size_source_renders_the_export_pixels(void) {
   Fx fx;
   fx_open(&fx);
   EnhancerSource *p_src = source_for(fx.p_file, NULL, 0);
   g_assert_cmpfloat(enhancer_source_get_scale(p_src), ==, 1.0);
   g_assert_null(enhancer_source_get_original(p_src));
   gint i_w, i_h;
   enhancer_source_get_orig_size(p_src, &i_w, &i_h);
   g_assert_cmpint(i_w, ==, IMG_W);
   g_assert_cmpint(i_h, ==, IMG_H);
   Transform t_xf;
   transform_init(&t_xf);
   t_xf.i_quarter     = 1;
   t_xf.b_crop        = TRUE;
   t_xf.t_crop        = (CropRect){20, 30, 200, 250};
   guint32     u_mask = GGAZE_ENHANCE_BIT(1) | GGAZE_ENHANCE_BIT(6);
   GdkTexture *p_r    = render_on(p_src, u_mask, &t_xf, NULL);
   GdkTexture *p_f    = full_chain(fx.p_file, u_mask, &t_xf);
   assert_same_pixels(p_r, p_f);
   g_assert_false(logical_size_is_scaled(p_r));
   g_object_unref(p_r);
   g_object_unref(p_f);
   enhancer_source_delete(p_src);
   fx_close(&fx);
}

/* Capped at 100 px the source is the image at a quarter; its render of a
 * crop is the crop at a quarter yet stands for the export's 200x150; the
 * source's own texture stands for the 400x300 original. */
static void
test_capped_source_is_scaled_and_stands_for_the_image(void) {
   Fx fx;
   fx_open(&fx);
   EnhancerSource *p_src = source_for(fx.p_file, NULL, 100);
   g_assert_cmpfloat_with_epsilon(enhancer_source_get_scale(p_src), 0.25, 1e-9);
   GdkTexture *p_orig = enhancer_source_get_original(p_src);
   g_assert_nonnull(p_orig);
   g_assert_cmpint(gdk_texture_get_width(p_orig), ==, 100);
   gint i_w, i_h;
   logical_size_get(p_orig, &i_w, &i_h);
   g_assert_cmpint(i_w, ==, IMG_W);
   g_assert_cmpint(i_h, ==, IMG_H);
   Transform t_xf;
   transform_init(&t_xf);
   t_xf.b_crop     = TRUE;
   t_xf.t_crop     = (CropRect){100, 60, 200, 150};
   GdkTexture *p_r = render_on(p_src, GGAZE_ENHANCE_BIT(1), &t_xf, NULL);
   g_assert_cmpint(gdk_texture_get_width(p_r), ==, 50);
   g_assert_cmpint(gdk_texture_get_height(p_r), ==, 38); /* 37.5, snapped */
   logical_size_get(p_r, &i_w, &i_h);
   g_assert_cmpint(i_w, ==, 200);
   g_assert_cmpint(i_h, ==, 150);
   /* The crop is where it is on the image: its left column is still the
    * dark half, its right one the light half. */
   g_assert_cmpint(px(p_r, 2, 10, 0), <, 128);
   g_assert_cmpint(px(p_r, 47, 10, 0), >, 128);
   g_object_unref(p_r);
   enhancer_source_delete(p_src);
   fx_close(&fx);
}

/* From the viewer's decode the source reads nothing from the file -- no
 * loader decode -- and holds what a decode of the file gives. */
static void
test_source_from_the_decode_does_not_decode(void) {
   Fx fx;
   fx_open(&fx);
   GError     *p_err = NULL;
   GeglBuffer *p_buf = enhancer_load(fx.p_file, &p_err);
   g_assert_no_error(p_err);
   GdkTexture *p_dec = enhancer_buffer_to_texture(p_buf, &p_err);
   g_assert_no_error(p_err);
   g_object_unref(p_buf);
   guint           u_before = enhancer_test_loader_decodes();
   EnhancerSource *p_a      = source_for(fx.p_file, p_dec, 100);
   g_assert_cmpuint(enhancer_test_loader_decodes(), ==, u_before);
   EnhancerSource *p_b = source_for(fx.p_file, NULL, 100);
   g_assert_cmpuint(enhancer_test_loader_decodes(), ==, u_before + 1);
   assert_same_pixels(enhancer_source_get_original(p_a),
                      enhancer_source_get_original(p_b));
   enhancer_source_delete(p_a);
   enhancer_source_delete(p_b);
   g_object_unref(p_dec);
   fx_close(&fx);
}

/* A user preset's Gaussian blur (8 px) on the quarter-size source is a
 * 2 px blur there: 7 source px left and 6 right of the edge (at 50; the
 * blend runs in linear light, so its sRGB midpoint sits a pixel to the
 * dark side) each half keeps its own value but for a few levels.
 * Unscaled -- 8 source px, four times too wide -- those pixels would be
 * well into the blend. */
static void
test_pixel_lengths_scale_with_the_source(void) {
   Fx fx;
   fx_open(&fx);
   EnhancerSource *p_src = source_for(fx.p_file, NULL, 100);
   EnhancerPreset  t_pr  = {.c_name  = "Blur",
                            .c_graph = "gegl:gaussian-blur std-dev-x=8 "
                                       "std-dev-y=8"};
   GPtrArray      *p_one = g_ptr_array_new();
   g_ptr_array_add(p_one, &t_pr);
   Wait w = {.p_loop = g_main_loop_new(NULL, FALSE)};
   enhancer_source_render_async(p_src, p_one, 1, NULL, NULL, render_cb, &w);
   g_main_loop_run(w.p_loop);
   g_main_loop_unref(w.p_loop);
   g_assert_no_error(w.p_err);
   GdkTexture *p_r = w.p_out;
   g_assert_cmpint(gdk_texture_get_width(p_r), ==, 100);
   g_assert_cmpint(px(p_r, 42, 40, 0), <, 25);
   g_assert_cmpint(px(p_r, 55, 40, 0), >, 235);
   g_object_unref(p_r);
   g_ptr_array_unref(p_one);
   enhancer_source_delete(p_src);
   fx_close(&fx);
}

/* The views a source serves: its own file, as fine or coarser than it
 * was made for; a larger view or another file wants a new one. */
static void
test_source_serves_its_file_and_smaller_views(void) {
   Fx fx;
   fx_open(&fx);
   EnhancerSource *p_src   = source_for(fx.p_file, NULL, 100);
   PreviewView     t_same  = {0, 0, 1, 100};
   PreviewView     t_small = {0, 0, 1, 50};
   PreviewView     t_big   = {0, 0, 1, 200};
   g_assert_true(enhancer_source_serves(p_src, fx.p_file, &t_same));
   g_assert_true(enhancer_source_serves(p_src, fx.p_file, &t_small));
   g_assert_false(enhancer_source_serves(p_src, fx.p_file, &t_big));
   GFile *p_other = g_file_new_for_path("/nonexistent/other.png");
   g_assert_false(enhancer_source_serves(p_src, p_other, &t_same));
   g_assert_false(enhancer_source_serves(p_src, NULL, &t_same));
   g_assert_true(g_file_equal(enhancer_source_get_file(p_src), fx.p_file));
   g_assert_false(enhancer_source_is_managed(p_src));
   g_object_unref(p_other);
   enhancer_source_delete(p_src);
   fx_close(&fx);
}

/* The card thumbnails come from the source at thumbnail size, whatever
 * the image's: the original then one card per preset; a batch cancelled
 * before it starts reports the cancellation. */
static void
test_thumbnails_from_the_source(void) {
   Fx fx;
   fx_open(&fx);
   EnhancerSource *p_src = source_for(fx.p_file, NULL, 0);
   Enhancer       *p_e   = enhancer_new();
   Wait            w     = {.p_loop = g_main_loop_new(NULL, FALSE)};
   enhancer_preview_thumbnails_async(p_src, enhancer_get_presets(p_e), NULL,
                                     thumbs_cb, &w);
   g_main_loop_run(w.p_loop);
   g_assert_no_error(w.p_err);
   GPtrArray *p_tex = w.p_out;
   g_assert_cmpuint(p_tex->len, ==, 1 + GGAZE_ENHANCE_N_BUILTINS);
   GdkTexture *p_orig = g_ptr_array_index(p_tex, 0);
   g_assert_cmpint(gdk_texture_get_width(p_orig), ==, PREVIEW_SCALE_THUMB_SIDE);
   g_assert_cmpint(gdk_texture_get_height(p_orig), ==, 96);
   g_ptr_array_unref(p_tex);
   GCancellable *p_cancel = g_cancellable_new();
   g_cancellable_cancel(p_cancel);
   w.p_out = NULL;
   enhancer_preview_thumbnails_async(p_src, enhancer_get_presets(p_e), p_cancel,
                                     thumbs_cb, &w);
   g_main_loop_run(w.p_loop);
   g_assert_error(w.p_err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
   g_assert_null(w.p_out);
   g_clear_error(&w.p_err);
   g_object_unref(p_cancel);
   g_main_loop_unref(w.p_loop);
   enhancer_delete(p_e);
   enhancer_source_delete(p_src);
   fx_close(&fx);
}

/* A colour-managed file (swapped.png: its profile swaps red and blue, the
 * managed decode is BLUE): the scaled source is the managed decode, so
 * the compare original is blue too -- like with like. */
static void
test_managed_source_is_the_managed_decode(void) {
   const gchar    *c_fx   = g_getenv("GGAZE_FIXTURES_DIR");
   char           *c_path = g_build_filename(c_fx, "swapped.png", NULL);
   GFile          *p_file = g_file_new_for_path(c_path);
   EnhancerSource *p_src  = source_for(p_file, NULL, 3);
   g_assert_true(enhancer_source_is_managed(p_src));
   GdkTexture *p_orig = enhancer_source_get_original(p_src);
   g_assert_nonnull(p_orig);
   g_assert_cmpint(px(p_orig, 1, 0, 0), <, 40);
   g_assert_cmpint(px(p_orig, 1, 0, 2), >, 215);
   enhancer_source_delete(p_src);
   g_object_unref(p_file);
   g_free(c_path);
}

/* A file that cannot be decoded fails the build with the loader's error. */
static void
test_source_of_an_undecodable_file_fails(void) {
   Fx fx;
   fx_open(&fx);
   g_assert_true(g_file_set_contents(fx.c_path, "no image", -1, NULL));
   PreviewView t_view = {0, 0, 1, 0};
   Wait        w      = {.p_loop = g_main_loop_new(NULL, FALSE)};
   enhancer_source_new_async(fx.p_file, NULL, &t_view, NULL, source_cb, &w);
   g_main_loop_run(w.p_loop);
   g_main_loop_unref(w.p_loop);
   g_assert_null(w.p_out);
   g_assert_nonnull(w.p_err);
   g_clear_error(&w.p_err);
   fx_close(&fx);
}

/* The test seams: a delayed render still lands; a failing one reports. */
static void
test_render_seams(void) {
   Fx fx;
   fx_open(&fx);
   EnhancerSource *p_src = source_for(fx.p_file, NULL, 100);
   enhancer_test_set_render_delay(50);
   gint64      i_t0 = g_get_monotonic_time();
   GdkTexture *p_r  = render_on(p_src, GGAZE_ENHANCE_BIT(1), NULL, NULL);
   g_assert_cmpint(g_get_monotonic_time() - i_t0, >=, 50000);
   enhancer_test_set_render_delay(0);
   g_assert_nonnull(p_r);
   g_object_unref(p_r);
   enhancer_test_set_render_fail(TRUE);
   GError *p_err = NULL;
   p_r           = render_on(p_src, GGAZE_ENHANCE_BIT(1), NULL, &p_err);
   enhancer_test_set_render_fail(FALSE);
   g_assert_null(p_r);
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED);
   g_clear_error(&p_err);
   enhancer_source_delete(p_src);
   fx_close(&fx);
}

int
main(int argc, char **argv) {
   gegl_init(&argc, &argv);
   enhancer_babl_ready(); /* as app.c, right after gegl_init() */
   g_test_init(&argc, &argv, NULL);
   g_test_add_func("/enhancer_preview/full_size_source_renders_export_pixels",
                   test_full_size_source_renders_the_export_pixels);
   g_test_add_func("/enhancer_preview/capped_source_stands_for_the_image",
                   test_capped_source_is_scaled_and_stands_for_the_image);
   g_test_add_func("/enhancer_preview/source_from_the_decode_does_not_decode",
                   test_source_from_the_decode_does_not_decode);
   g_test_add_func("/enhancer_preview/pixel_lengths_scale_with_the_source",
                   test_pixel_lengths_scale_with_the_source);
   g_test_add_func("/enhancer_preview/source_serves_its_file_and_smaller_views",
                   test_source_serves_its_file_and_smaller_views);
   g_test_add_func("/enhancer_preview/thumbnails_from_the_source",
                   test_thumbnails_from_the_source);
   g_test_add_func("/enhancer_preview/managed_source_is_the_managed_decode",
                   test_managed_source_is_the_managed_decode);
   g_test_add_func("/enhancer_preview/source_of_an_undecodable_file_fails",
                   test_source_of_an_undecodable_file_fails);
   g_test_add_func("/enhancer_preview/render_seams", test_render_seams);
   return (g_test_run());
}
