/*:*
 * ggaze — colour management on the GEGL enhance path (xb2, decision #45),
 * unit test (gated on GEGL)
 *
 * The fixtures (tests/fixtures/gen.py) store colours under profiles that
 * move them somewhere else -- swapped.png's red and blue primaries are
 * sRGB's swapped, cmyk-icc.jpg's printer profile maps red ink to blue,
 * grey-icc.*'s grey curve is linear -- so every case asks one question
 * with a visible answer: "is the pixel what the PROFILE says (managed) or
 * what the file stores (the loader path)?". Around that: the managed path
 * never gives a verdict of its own (a broken file gets exactly the
 * loader's), an sRGB profile keeps the loader path byte for byte, the
 * export keeps the profile, the missing-op fallbacks, a non-local file,
 * a profile for other colour components, the cut progressive JPEGs that
 * made gegl:jpg-load exit, the lazy managed original, the info card's
 * "would manage" question, and -- when ./sample-images is there -- every
 * profiled camera file. Every JPEG case holds in a GEGL build without
 * libjpeg too, where JPEGs keep the loader path (GGAZE_HAVE_JPEG).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "enhancer.h"
#include "ggaze-config.h"
#include "enhancer-gegl.h"
#include "icc.h"
#include "icc_build.h"
#include "loader/intact.h"
#include "loader/loader.h"
#include "mem_file.h"
#include "transform.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

static char *c_dir; /* temp folder for the hand-built variants */

/* --- helpers -------------------------------------------------------------- */

static GFile *
fixture(const char *c_name) {
   const gchar *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char  *c_path = g_build_filename(c_fx, c_name, NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);
   return (p_file);
}

/* The fixture's bytes, in a GByteArray the caller edits and unrefs. */
static GByteArray *
fixture_bytes(const char *c_name) {
   GFile *p_file = fixture(c_name);
   char  *c_data = NULL;
   gsize  u_len  = 0;
   g_assert_true(
      g_file_load_contents(p_file, NULL, &c_data, &u_len, NULL, NULL));
   g_object_unref(p_file);
   GByteArray *p_a = g_byte_array_new_take((guint8 *)c_data, u_len);
   return (p_a);
}

/* p_a written as c_dir/c_name. */
static GFile *
temp_file(const char *c_name, const GByteArray *p_a) {
   char *c_path = g_build_filename(c_dir, c_name, NULL);
   g_assert_true(
      g_file_set_contents(c_path, (const char *)p_a->data, p_a->len, NULL));
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);
   return (p_file);
}

static void
drop_temp(GFile *p_file) {
   g_file_delete(p_file, NULL, NULL);
   g_object_unref(p_file);
}

static GeglBuffer *
load_ok(GFile *p_file) {
   GError     *p_err = NULL;
   GeglBuffer *p_buf = enhancer_load(p_file, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_buf);
   return (p_buf);
}

static GeglBuffer *
load_fixture(const char *c_name) {
   GFile      *p_file = fixture(c_name);
   GeglBuffer *p_buf  = load_ok(p_file);
   g_object_unref(p_file);
   return (p_buf);
}

static gboolean
is_srgb(GeglBuffer *p_buf) {
   return (babl_format_get_space(gegl_buffer_get_format(p_buf)) ==
           babl_space("sRGB"));
}

/* The sRGB RGBA of the preview texture's pixel (i_x, i_y), read in an
 * EXPLICIT R8G8B8A8 layout (gdk_texture_download() would hand back
 * GDK_MEMORY_DEFAULT, B8G8R8A8 on little-endian hosts). */
static void
preview_pixel(GeglBuffer *p_buf, gint i_x, gint i_y, guint8 *p_rgba) {
   GError     *p_err = NULL;
   GdkTexture *p_tex = enhancer_buffer_to_texture(p_buf, &p_err);
   g_assert_no_error(p_err);
   GdkTextureDownloader *p_dl = gdk_texture_downloader_new(p_tex);
   gdk_texture_downloader_set_format(p_dl, GDK_MEMORY_R8G8B8A8);
   gsize   u_stride   = 0;
   GBytes *p_bytes    = gdk_texture_downloader_download_bytes(p_dl, &u_stride);
   const guint8 *p_px = g_bytes_get_data(p_bytes, NULL);
   memcpy(p_rgba, p_px + (gsize)i_y * u_stride + (gsize)i_x * 4, 4);
   g_bytes_unref(p_bytes);
   gdk_texture_downloader_free(p_dl);
   g_object_unref(p_tex);
}

/* p_rgba is (r, g, b) within i_tol per channel; c_what names it in the
 * log, since the assertion cannot. */
static void
assert_rgb(const char *c_what, const guint8 *p_rgba, int i_r, int i_g, int i_b,
           int i_tol) {
   g_test_message("%s: %u %u %u %u", c_what, p_rgba[0], p_rgba[1], p_rgba[2],
                  p_rgba[3]);
   g_assert_cmpint(ABS((int)p_rgba[0] - i_r), <=, i_tol);
   g_assert_cmpint(ABS((int)p_rgba[1] - i_g), <=, i_tol);
   g_assert_cmpint(ABS((int)p_rgba[2] - i_b), <=, i_tol);
}

/* Pixel (i_x, i_y) of c_name's managed decode is (r, g, b); returns
 * whether the buffer was sRGB-tagged, for the caller to assert on. */
static gboolean
fixture_pixel_is(const char *c_name, gint i_x, gint i_y, int i_r, int i_g,
                 int i_b) {
   GeglBuffer *p_buf = load_fixture(c_name);
   guint8      c_px[4];
   preview_pixel(p_buf, i_x, i_y, c_px);
   assert_rgb(c_name, c_px, i_r, i_g, i_b, 15);
   gboolean b_srgb = is_srgb(p_buf);
   g_object_unref(p_buf);
   return (b_srgb);
}

/* Whether c_name's decode is colour-managed, as the async render reports
 * it (enhancer_apply_chain_finish's *pb_managed): the one observable that
 * also covers CMYK and grey files, whose managed buffers are sRGB-tagged
 * like the loader path's. */
typedef struct {
   GMainLoop *p_loop;
   gboolean   b_managed;
} ManagedWait;

static void
managed_done(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   ManagedWait *p_w   = p_data;
   GError      *p_err = NULL;
   GdkTexture  *p_tex =
      enhancer_apply_chain_finish(p_res, NULL, NULL, &p_w->b_managed, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_tex);
   g_object_unref(p_tex);
   g_main_loop_quit(p_w->p_loop);
}

static gboolean
render_file_managed(GFile *p_file) {
   Enhancer   *p_e = enhancer_new();
   ManagedWait t_w = {g_main_loop_new(NULL, FALSE), FALSE};
   enhancer_apply_chain_async(p_file, enhancer_get_presets(p_e), 1u << 2, NULL,
                              NULL, managed_done, &t_w);
   g_main_loop_run(t_w.p_loop);
   g_main_loop_unref(t_w.p_loop);
   enhancer_delete(p_e);
   return (t_w.b_managed);
}

static gboolean
render_managed(const char *c_name) {
   GFile   *p_file    = fixture(c_name);
   gboolean b_managed = render_file_managed(p_file);
   g_object_unref(p_file);
   return (b_managed);
}

/* A big-endian 32-bit field at p (any alignment). */
static guint32
be32(const guint8 *p) {
   return (((guint32)p[0] << 24) | ((guint32)p[1] << 16) |
           ((guint32)p[2] << 8) | (guint32)p[3]);
}

/* PNG's chunk CRC (ISO 3309), for the hand-edited variants. */
static guint32
png_crc(const guint8 *p, gsize u_len) {
   guint32 u_crc = 0xFFFFFFFFu;
   for (gsize u = 0; u < u_len; u++) {
      u_crc ^= p[u];
      for (int i = 0; i < 8; i++) {
         u_crc = (u_crc & 1) ? 0xEDB88320u ^ (u_crc >> 1) : u_crc >> 1;
      }
   }
   return (u_crc ^ 0xFFFFFFFFu);
}

/* Rewrite the CRC of the PNG chunk whose length field is at u_at. */
static void
png_fix_crc(GByteArray *p_a, gsize u_at) {
   guint32 u_len = be32(p_a->data + u_at);
   guint32 u_crc = GUINT32_TO_BE(png_crc(p_a->data + u_at + 4, u_len + 4));
   memcpy(p_a->data + u_at + 8 + u_len, &u_crc, 4);
}

/* Offset of the length field of p_a's first chunk of type c_type. */
static gsize
png_chunk_at(const GByteArray *p_a, const char *c_type) {
   gsize u_at = 8;
   while (u_at + 8 <= p_a->len) {
      if (memcmp(p_a->data + u_at + 4, c_type, 4) == 0) {
         return (u_at);
      }
      u_at += 12 + be32(p_a->data + u_at);
   }
   g_assert_not_reached();
   return (0);
}

/* Insert u_len bytes of p_data into p_a at u_at (GLib has no
 * g_byte_array_insert). */
static void
bytes_insert(GByteArray *p_a, gsize u_at, const guint8 *p_data, gsize u_len) {
   gsize u_old = p_a->len;
   g_byte_array_set_size(p_a, (guint)(u_old + u_len));
   memmove(p_a->data + u_at + u_len, p_a->data + u_at, u_old - u_at);
   memcpy(p_a->data + u_at, p_data, u_len);
}

/* The JPEG in p_a with u_len bytes of p_seg inserted right after SOI. */
static void
jpeg_insert_after_soi(GByteArray *p_a, const guint8 *p_seg, gsize u_len) {
   bytes_insert(p_a, 2, p_seg, u_len);
}

/* The length of the first segment after SOI in p_a, its marker included. */
static gsize
jpeg_first_segment_len(const GByteArray *p_a) {
   return (2 + (((gsize)p_a->data[4] << 8) | p_a->data[5]));
}

/* --- managed decodes ----------------------------------------------------
 *
 * A JPEG is managed only in a build with the `jpeg` feature: without
 * libjpeg nothing can check it before gegl:jpg-load (which exits the
 * process on a file libjpeg gives up on), so every JPEG keeps the loader
 * path there -- CI's gegl lane once built that way, and these cases must
 * hold in both builds (GGAZE_HAVE_JPEG). What the loader path's pixels
 * look like is the host's business (see below), so the JPEG-less branch
 * asserts the path, not the colour. */

/* Tagged with the profile's space, previewed managed: PNG and JPEG. */
static void
test_managed_png_and_jpeg(void) {
   g_assert_false(fixture_pixel_is("swapped.png", 0, 0, 0, 0, 255));
#if GGAZE_HAVE_JPEG
   g_assert_false(fixture_pixel_is("swapped.jpg", 0, 0, 0, 0, 255));
#else
   GeglBuffer *p_buf = load_fixture("swapped.jpg");
   g_assert_true(is_srgb(p_buf));
   g_object_unref(p_buf);
   g_assert_false(render_managed("swapped.jpg"));
#endif
}

/* swapped-rot6.jpg: 16x8 stored, left half red / right half green under
 * the swapped profile, Orientation 6. Upright 8x16 with the stored left
 * half on top, and managed: blue on top, green below. */
static void
test_managed_orientation(void) {
   GeglBuffer *p_buf = load_fixture("swapped-rot6.jpg");
   g_assert_cmpint(gegl_buffer_get_width(p_buf), ==, 8);
   g_assert_cmpint(gegl_buffer_get_height(p_buf), ==, 16);
#if !GGAZE_HAVE_JPEG
   g_assert_true(is_srgb(p_buf)); /* the loader path, upright all the same */
   g_object_unref(p_buf);
   return;
#endif
   g_assert_false(is_srgb(p_buf));
   guint8 c_px[4];
   preview_pixel(p_buf, 0, 0, c_px);
   assert_rgb("rot6 top", c_px, 0, 0, 255, 15);
   preview_pixel(p_buf, 7, 15, c_px);
   assert_rgb("rot6 bottom", c_px, 0, 255, 0, 15);
   g_object_unref(p_buf);
}

/* CMYK and grey profiles are managed too, but the chain runs in sRGB
 * (their pixels have no meaning in an RGB format of their space) -- and
 * the render reports them managed all the same, which is what makes
 * hold-Space compare against their managed original. */
static void
test_cmyk_and_grey_run_in_srgb(void) {
   /* linear grey 128/255 is sRGB ~188 */
   g_assert_true(fixture_pixel_is("grey-icc.png", 0, 0, 188, 188, 188));
   g_assert_true(render_managed("grey-icc.png"));
#if GGAZE_HAVE_JPEG
   g_assert_true(fixture_pixel_is("cmyk-icc.jpg", 0, 0, 0, 0, 255));
   g_assert_true(fixture_pixel_is("grey-icc.jpg", 0, 0, 188, 188, 188));
#endif
   g_assert_true(render_managed("cmyk-icc.jpg") == GGAZE_HAVE_JPEG);
   g_assert_true(render_managed("grey-icc.jpg") == GGAZE_HAVE_JPEG);
   g_assert_false(render_managed("plain.jpg"));
   g_assert_false(render_managed("srgb-icc.png"));
}

/* The space survives two presets and a transform. */
static void
test_chain_keeps_the_space(void) {
   Enhancer   *p_e  = enhancer_new();
   GeglBuffer *p_in = load_fixture("swapped.png");
   Transform   t;
   transform_init(&t);
   t.i_quarter        = 1;
   guint8      u_mask = (1u << 1) | (1u << 2); /* Brightness + Contrast */
   GError     *p_err  = NULL;
   GeglBuffer *p_out =
      enhancer_apply_chain(p_in, enhancer_get_presets(p_e), u_mask, &t, &p_err);
   g_assert_no_error(p_err);
   g_assert_true(babl_format_get_space(gegl_buffer_get_format(p_out)) ==
                 babl_format_get_space(gegl_buffer_get_format(p_in)));
   g_assert_cmpint(gegl_buffer_get_width(p_out), ==, 3);
   g_assert_cmpint(gegl_buffer_get_height(p_out), ==, 6);
   guint8 c_px[4];
   preview_pixel(p_out, 0, 0, c_px);
   g_test_message("chained: %u %u %u", c_px[0], c_px[1], c_px[2]);
   g_assert_cmpint(c_px[2], >, c_px[0] + 150); /* still blue */
   g_object_unref(p_out);
   g_object_unref(p_in);
   enhancer_delete(p_e);
}

/* A profiled JPEG whose SOF lies past 64 KiB (a phone photo's big EXIF /
 * XMP; a CMYK file's press profile) is managed: the completeness walk
 * reads the size, no bounded peek refuses it. */
static void
test_sof_past_64k_is_managed(void) {
   guint8 *p_pad         = g_malloc0(65537);
   p_pad[0]              = 0xFF;
   p_pad[1]              = 0xEF; /* APP15, 65535 bytes: 65533 of payload */
   p_pad[2]              = 0xFF;
   p_pad[3]              = 0xFF;
   const char *C_NAMES[] = {"swapped.jpg", "cmyk-icc.jpg"};
   for (gsize u = 0; u < G_N_ELEMENTS(C_NAMES); u++) {
      GByteArray *p_a = fixture_bytes(C_NAMES[u]);
      jpeg_insert_after_soi(p_a, p_pad, 65537);
      jpeg_insert_after_soi(p_a, p_pad, 65537);
      GFile      *p_file = temp_file("far.jpg", p_a);
      GeglBuffer *p_buf  = load_ok(p_file);
      guint8      c_px[4];
      preview_pixel(p_buf, 0, 0, c_px);
      if (GGAZE_HAVE_JPEG) {
         assert_rgb(C_NAMES[u], c_px, 0, 0, 255, 15); /* managed */
      }
      g_object_unref(p_buf);
      drop_temp(p_file);
      g_byte_array_unref(p_a);
   }
   g_free(p_pad);
}

/* Stray bytes between two segments, which libjpeg skips with a warning:
 * still managed (and still a profile for the info card, test_icc.c). */
static void
test_padded_jpeg_is_managed(void) {
   GByteArray  *p_a      = fixture_bytes("swapped.jpg");
   const guint8 C_PAD[3] = {0x00, 0x11, 0x22};
   bytes_insert(p_a, jpeg_first_segment_len(p_a) + 2, C_PAD, 3);
   GFile      *p_file = temp_file("pad.jpg", p_a);
   GeglBuffer *p_buf  = load_ok(p_file);
   g_assert_true(is_srgb(p_buf) == !GGAZE_HAVE_JPEG);
   g_object_unref(p_buf);
   drop_temp(p_file);
   g_byte_array_unref(p_a);
}

/* A profile for another number of colour components than the image has
 * -- the linear grey profile on the RGB swapped.jpg and swapped.png --
 * cannot describe the pixels: the managed path declines it (the loader
 * path, sRGB-tagged) instead of reading RGB through a grey curve. */
static void
test_profile_for_other_components_is_declined(void) {
   GByteArray *p_rgb  = fixture_bytes("swapped.jpg");
   GByteArray *p_grey = fixture_bytes("grey-icc.jpg");
   gsize       u_rgb  = jpeg_first_segment_len(p_rgb); /* the APP2s */
   gsize       u_grey = jpeg_first_segment_len(p_grey);
   g_assert_cmpuint(p_rgb->data[3], ==, 0xE2);
   g_assert_cmpuint(p_grey->data[3], ==, 0xE2);
   g_byte_array_remove_range(p_rgb, 2, (guint)u_rgb);
   bytes_insert(p_rgb, 2, p_grey->data + 2, u_grey);
   struct {
      const char *c_name;
      GByteArray *p_a;
   } CASES[2]         = {{"greyprof.jpg", p_rgb}, {"greyprof.png", NULL}};
   GByteArray *p_png  = fixture_bytes("swapped.png");
   GByteArray *p_gpng = fixture_bytes("grey-icc.png");
   gsize       u_at   = png_chunk_at(p_png, "iCCP");
   gsize       u_gat  = png_chunk_at(p_gpng, "iCCP");
   g_byte_array_remove_range(p_png, (guint)u_at, be32(p_png->data + u_at) + 12);
   bytes_insert(p_png, u_at, p_gpng->data + u_gat,
                be32(p_gpng->data + u_gat) + 12);
   CASES[1].p_a = p_png;
   for (gsize u = 0; u < G_N_ELEMENTS(CASES); u++) {
      GFile      *p_file = temp_file(CASES[u].c_name, CASES[u].p_a);
      GeglBuffer *p_buf  = load_ok(p_file);
      g_assert_true(is_srgb(p_buf));
      g_object_unref(p_buf);
      drop_temp(p_file);
   }
   g_byte_array_unref(p_rgb);
   g_byte_array_unref(p_grey);
   g_byte_array_unref(p_png);
   g_byte_array_unref(p_gpng);
}

/* The info card's question (enhancer_would_manage), header-deep: a
 * non-sRGB profile on a local PNG, or JPEG with libjpeg, yes; no profile,
 * an sRGB one, garbage, a profile for other components (built by the test
 * above: a grey profile on an RGB JPEG), a non-local file or a missing
 * loader op, no. */
static void
test_would_manage(void) {
   const struct {
      const char *c_name;
      gboolean    b_want;
   } CASES[] = {{"swapped.png", TRUE},
                {"grey-icc.png", TRUE},
                {"swapped.jpg", GGAZE_HAVE_JPEG},
                {"cmyk-icc.jpg", GGAZE_HAVE_JPEG},
                {"srgb-icc.png", FALSE},
                {"srgb-icc.jpg", FALSE},
                {"plain.jpg", FALSE},
                {"badicc.png", FALSE}};
   for (gsize u = 0; u < G_N_ELEMENTS(CASES); u++) {
      GFile *p_file = fixture(CASES[u].c_name);
      g_assert_true(enhancer_would_manage(p_file) == CASES[u].b_want);
      g_object_unref(p_file);
   }
   GByteArray *p_rgb  = fixture_bytes("swapped.jpg");
   GByteArray *p_grey = fixture_bytes("grey-icc.jpg");
   g_byte_array_remove_range(p_rgb, 2, (guint)jpeg_first_segment_len(p_rgb));
   bytes_insert(p_rgb, 2, p_grey->data + 2, jpeg_first_segment_len(p_grey));
   GFile *p_file = temp_file("greyprof.jpg", p_rgb);
   g_assert_false(enhancer_would_manage(p_file));
   drop_temp(p_file);
   g_byte_array_unref(p_rgb);
   g_byte_array_unref(p_grey);
   GByteArray *p_a = fixture_bytes("swapped.png");
   p_file          = ggtest_mem_file_new(p_a->data, p_a->len);
   g_assert_false(enhancer_would_manage(p_file));
   g_object_unref(p_file);
   g_byte_array_unref(p_a);
   enhancer_test_set_missing_op("gegl:png-load");
   p_file = fixture("swapped.png");
   g_assert_false(enhancer_would_manage(p_file));
   g_object_unref(p_file);
   enhancer_test_set_missing_op(NULL);
}

/* --- the loader path ------------------------------------------------------ */

/* The RGBA u8 bytes of p_buf, for byte-for-byte comparisons. */
static GBytes *
buffer_bytes(GeglBuffer *p_buf) {
   const GeglRectangle *p_r   = gegl_buffer_get_extent(p_buf);
   gsize                u_len = (gsize)p_r->width * p_r->height * 4;
   guint8              *p_px  = g_malloc(u_len);
   gegl_buffer_get(p_buf, p_r, 1.0, babl_format("R'G'B'A u8"), p_px,
                   p_r->width * 4, GEGL_ABYSS_NONE);
   return (g_bytes_new_take(p_px, u_len));
}

/* c_name loads byte for byte like p_stripped (the same file without its
 * profile), sRGB-tagged. */
static void
assert_same_as_stripped(const char *c_name, GByteArray *p_stripped) {
   GeglBuffer *p_tagged = load_fixture(c_name);
   GFile      *p_file   = temp_file("stripped", p_stripped);
   GeglBuffer *p_plain  = load_ok(p_file);
   g_assert_true(is_srgb(p_tagged));
   GBytes *p_a = buffer_bytes(p_tagged);
   GBytes *p_b = buffer_bytes(p_plain);
   g_assert_true(g_bytes_equal(p_a, p_b));
   g_bytes_unref(p_a);
   g_bytes_unref(p_b);
   g_object_unref(p_tagged);
   g_object_unref(p_plain);
   drop_temp(p_file);
}

/* An sRGB profile has nothing to manage: the file keeps the loader path
 * and its pixels are those of the same file with no profile at all. */
static void
test_srgb_profile_is_byte_identical(void) {
   GByteArray *p_png  = fixture_bytes("srgb-icc.png");
   gsize       u_iccp = png_chunk_at(p_png, "iCCP");
   guint32     u_len  = be32(p_png->data + u_iccp);
   g_byte_array_remove_range(p_png, (guint)u_iccp, u_len + 12);
   assert_same_as_stripped("srgb-icc.png", p_png);
   g_byte_array_unref(p_png);
   GByteArray *p_jpg = fixture_bytes("srgb-icc.jpg");
   g_assert_cmpuint(p_jpg->data[3], ==, 0xE2); /* the APP2 follows SOI */
   g_byte_array_remove_range(p_jpg, 2, (guint)jpeg_first_segment_len(p_jpg));
   assert_same_as_stripped("srgb-icc.jpg", p_jpg);
   g_byte_array_unref(p_jpg);
}

/* No profile, or one that is no profile (badicc.png's iCCP inflates to
 * text): the loader path, sRGB, the stored red. */
static void
test_untagged_and_broken_profiles_stay_srgb(void) {
   const char *C_NAMES[] = {"plain.jpg", "small.png"};
   for (gsize u = 0; u < G_N_ELEMENTS(C_NAMES); u++) {
      GeglBuffer *p_buf = load_fixture(C_NAMES[u]);
      g_assert_true(is_srgb(p_buf));
      g_object_unref(p_buf);
   }
   g_assert_true(fixture_pixel_is("badicc.png", 0, 0, 255, 0, 0));
}

/* What the loader path's pixels look like under a profile is the host's
 * business, not this suite's: gdk-pixbuf on a glycin desktop applies the
 * profile itself (swapped.png comes out blue there), fedora:40's native
 * loaders do not (red). So a test of the loader path checks the sRGB tag
 * and the size only -- the tag is what says the enhancer did not manage
 * the file. */

/* A non-local GFile (served from memory: no path for GEGL's loaders)
 * takes the loader path, which reads it like any other. */
static void
test_non_local_file_takes_the_loader(void) {
   GByteArray *p_a    = fixture_bytes("swapped.png");
   GFile      *p_file = ggtest_mem_file_new(p_a->data, p_a->len);
   GeglBuffer *p_buf  = load_ok(p_file);
   g_assert_true(is_srgb(p_buf));
   g_assert_cmpint(gegl_buffer_get_width(p_buf), ==, 6);
   g_assert_cmpint(gegl_buffer_get_height(p_buf), ==, 3);
   g_object_unref(p_buf);
   g_object_unref(p_file);
   g_byte_array_unref(p_a);
}

/* Whether c_name decodes sRGB-tagged (the loader path). */
static gboolean
fixture_is_srgb(const char *c_name) {
   GeglBuffer *p_buf  = load_fixture(c_name);
   gboolean    b_srgb = is_srgb(p_buf);
   g_object_unref(p_buf);
   return (b_srgb);
}

/* A GEGL without the loader op keeps the loader path for that format
 * only. */
static void
test_missing_loader_op_takes_the_loader(void) {
   enhancer_test_set_missing_op("gegl:png-load");
   g_assert_true(fixture_is_srgb("swapped.png"));
   g_assert_true(fixture_is_srgb("swapped.jpg") == !GGAZE_HAVE_JPEG);
   enhancer_test_set_missing_op("gegl:jpg-load");
   g_assert_true(fixture_is_srgb("swapped.jpg"));
   g_assert_false(fixture_is_srgb("swapped.png"));
   enhancer_test_set_missing_op(NULL);
}

/* --- broken files: the loader's verdict, never a new one ----------------- */

/* p_a as c_name through both paths: enhancer_load succeeds iff the loader
 * does (the managed path may decline, never refuse or accept on its own),
 * and a success is the loader path's (sRGB). Returns the success. */
static gboolean
assert_loader_verdict(const char *c_name, const GByteArray *p_a) {
   GFile      *p_file = temp_file(c_name, p_a);
   GError     *p_err  = NULL;
   GError     *p_lerr = NULL;
   GeglBuffer *p_buf  = enhancer_load(p_file, &p_err);
   GdkTexture *p_tex  = loader_load(p_file, NULL, &p_lerr);
   g_test_message("%s: enhancer %s, loader %s", c_name,
                  p_err != NULL ? p_err->message : "loads",
                  p_lerr != NULL ? p_lerr->message : "loads");
   g_assert_true((p_buf != NULL) == (p_tex != NULL));
   g_assert_true((p_err != NULL) == (p_buf == NULL));
   gboolean b_ok = p_buf != NULL;
   if (b_ok) {
      g_assert_true(is_srgb(p_buf));
   }
   g_clear_object(&p_buf);
   g_clear_object(&p_tex);
   g_clear_error(&p_err);
   g_clear_error(&p_lerr);
   drop_temp(p_file);
   return (b_ok);
}

/* Cut copies: GEGL's loaders would spin on them (a meson timeout found
 * this); the loader refuses a cut PNG and decodes a cut JPEG as it always
 * did, and so does the enhancer now. */
static void
test_truncated_files_get_the_loaders_verdict(void) {
   const struct {
      const char *c_fixture;
      guint       u_keep; /* bytes kept, when u_cut is 0 */
      guint       u_cut;  /* bytes cut off the end */
   } CASES[] = {{"swapped.png", 20, 0},
                {"swapped.png", 0, 40},
                {"swapped.jpg", 0, 100},
                {"swapped.jpg", 0, 2}};
   for (gsize u = 0; u < G_N_ELEMENTS(CASES); u++) {
      GByteArray *p_a = fixture_bytes(CASES[u].c_fixture);
      g_byte_array_set_size(p_a, CASES[u].u_cut > 0 ? p_a->len - CASES[u].u_cut
                                                    : CASES[u].u_keep);
      assert_loader_verdict(CASES[u].c_fixture, p_a);
      g_byte_array_unref(p_a);
   }
}

/* A copy of c_fixture with u_len bytes of p_patch at u_at (and, when
 * i_crc_at >= 0, the CRC of the PNG chunk there rewritten to match) through
 * assert_loader_verdict(). */
static gboolean
patched_verdict(const char *c_fixture, gsize u_at, const void *p_patch,
                gsize u_len, gssize i_crc_at) {
   GByteArray *p_a = fixture_bytes(c_fixture);
   g_assert_cmpuint(u_at + u_len, <=, p_a->len);
   memcpy(p_a->data + u_at, p_patch, u_len);
   if (i_crc_at >= 0) {
      png_fix_crc(p_a, (gsize)i_crc_at);
   }
   gboolean b_ok = assert_loader_verdict(c_fixture, p_a);
   g_byte_array_unref(p_a);
   return (b_ok);
}

/* Broken headers: IHDR not first, an empty or oversized PNG, an IHDR
 * whose width no longer matches its CRC or (CRC fixed) its data, a JPEG
 * whose SOF declares no rows. swapped.png is 6x3 (IHDR data at 16),
 * swapped.jpg's SOF0 height sits at byte 701. */
static void
test_broken_headers_get_the_loaders_verdict(void) {
   const guint8 C_ZERO[4]  = {0, 0, 0, 0};
   const guint8 C_HUGE[4]  = {0x00, 0x10, 0x00, 0x00}; /* 1048576 */
   const guint8 C_SEVEN[4] = {0, 0, 0, 7};
   g_assert_false(patched_verdict("swapped.png", 12, "XXXX", 4, -1));
   g_assert_false(patched_verdict("swapped.png", 16, C_ZERO, 4, 8));
   g_assert_false(patched_verdict("swapped.png", 16, C_HUGE, 4, 8));
   g_assert_false(patched_verdict("swapped.png", 16, C_SEVEN, 4, -1));
   patched_verdict("swapped.png", 16, C_SEVEN, 4, 8);
   patched_verdict("swapped.jpg", 701, C_ZERO, 2, -1);
}

/* A second SOF ahead of SOS: libjpeg refuses the header ("two SOF
 * markers"), so GEGL's bounding box is empty while the completeness walk
 * (which reads the first SOF) vouched for the file -- the extent check
 * catches it, and the loader's own verdict stands. */
static void
test_two_sof_jpeg_gets_the_loaders_verdict(void) {
   GByteArray *p_a   = fixture_bytes("swapped.jpg");
   gsize       u_sof = 0;
   for (gsize u = 2; u + 1 < p_a->len; u++) {
      if (p_a->data[u] == 0xFF && p_a->data[u + 1] == 0xC0) {
         u_sof = u;
         break;
      }
   }
   g_assert_cmpuint(u_sof, >, 0);
   gsize u_len =
      2 + (((gsize)p_a->data[u_sof + 2] << 8) | p_a->data[u_sof + 3]);
   guint8 *p_sof = g_memdup2(p_a->data + u_sof, u_len);
   bytes_insert(p_a, u_sof, p_sof, u_len);
   assert_loader_verdict("twosof.jpg", p_a);
   g_free(p_sof);
   g_byte_array_unref(p_a);
}

/* Offset of the u_n-th (from 1) FF u_code marker in p_a. */
static gsize
jpeg_nth_marker(const GByteArray *p_a, guint8 u_code, guint u_n) {
   for (gsize u = 2; u + 1 < p_a->len; u++) {
      if (p_a->data[u] == 0xFF && p_a->data[u + 1] == u_code && --u_n == 0) {
         return (u);
      }
   }
   g_assert_not_reached();
   return (0);
}

/* swapped-prog.jpg (progressive, every scan with entropy data) with a COM
 * segment holding the bytes FF D9 inserted ahead of its third scan, then
 * cut: right after the COM, and inside the third scan's data. Both look
 * complete to a byte scan for EOI, and libjpeg's stdio source hides the
 * cut (it inserts a fake EOI at EOF and decodes on), but gegl:jpg-load's
 * reader starts the file over at EOF and libjpeg then exits the process
 * ("two SOI markers"). The managed path must decline both -- the loader
 * decides, as for any cut JPEG -- while the whole file is managed. */
static void
test_cut_progressive_jpeg_gets_the_loaders_verdict(void) {
   GFile      *p_whole = fixture("swapped-prog.jpg");
   GeglBuffer *p_buf   = load_ok(p_whole);
   g_assert_true(is_srgb(p_buf) == !GGAZE_HAVE_JPEG);
   g_object_unref(p_buf);
   g_object_unref(p_whole);
   const guint8 C_COM[6] = {0xFF, 0xFE, 0x00, 0x04, 0xFF, 0xD9};
   GByteArray  *p_a      = fixture_bytes("swapped-prog.jpg");
   gsize        u_sos    = jpeg_nth_marker(p_a, 0xDA, 3);
   bytes_insert(p_a, u_sos, C_COM, sizeof(C_COM));
   gsize u_full = p_a->len;
   g_byte_array_set_size(p_a, (guint)(u_sos + 6 + 14 + 70)); /* mid-scan */
   g_assert_cmpuint(p_a->len, <, u_full);
   assert_loader_verdict("midscan.jpg", p_a);
   g_byte_array_set_size(p_a, (guint)(u_sos + sizeof(C_COM)));
   assert_loader_verdict("aftercom.jpg", p_a);
   g_byte_array_unref(p_a);
}

/* An unknown critical chunk ahead of IDAT: sound as a container (the walk
 * checks its CRC, not its meaning), but libpng refuses the header, so
 * GEGL's bounding box comes out empty and the extent check -- the last
 * line of defence after the walks -- hands the file to the loader. */
static void
test_extent_check_catches_what_the_walk_cannot(void) {
   GByteArray  *p_a      = fixture_bytes("swapped.png");
   gsize        u_idat   = png_chunk_at(p_a, "IDAT");
   const guint8 C_ZZ[13] = {0, 0, 0, 1, 'G', 'g', 'A', 'z', 'x', 0, 0, 0, 0};
   bytes_insert(p_a, u_idat, C_ZZ, sizeof(C_ZZ));
   png_fix_crc(p_a, u_idat);
   GFile  *p_file = temp_file("critical.png", p_a);
   GError *p_err  = NULL;
   g_assert_true(intact_png(p_file, NULL, NULL, &p_err));
   g_assert_no_error(p_err);
   drop_temp(p_file);
   assert_loader_verdict("critical.png", p_a);
   g_byte_array_unref(p_a);
}

/* Corrupt image data in a profiled PNG -- a flipped byte (bad CRC), the
 * same with the CRC fixed (the deflate stream breaks), and a shortened
 * IDAT (the rows run out) -- made gegl:png-load hand back a silent black
 * buffer. Now it is the loader's error, as before xb2. */
static void
test_corrupt_png_data_is_an_error(void) {
   GByteArray  *p_a      = fixture_bytes("swapped.png");
   gsize        u_idat   = png_chunk_at(p_a, "IDAT");
   const guint8 C_BAD[2] = {0xFF, 0x7E};
   g_assert_false(patched_verdict("swapped.png", u_idat + 10, C_BAD, 2, -1));
   g_assert_false(
      patched_verdict("swapped.png", u_idat + 10, C_BAD, 2, (gssize)u_idat));
   /* short: keep 6 bytes of the IDAT data (zlib header + a partial block) */
   guint32 u_len = be32(p_a->data + u_idat);
   g_byte_array_remove_range(p_a, (guint)u_idat + 8 + 6, u_len - 6);
   guint32 u_six = GUINT32_TO_BE(6);
   memcpy(p_a->data + u_idat, &u_six, 4);
   png_fix_crc(p_a, u_idat);
   g_assert_false(assert_loader_verdict("short.png", p_a));
   g_byte_array_unref(p_a);
}

/* --- export ----------------------------------------------------------------
 */

/* Export p_buf with p_preset to c_dir/c_name: the file carries exactly
 * p_want as its profile and reloads managed (blue). */
static void
export_keeps_profile(GeglBuffer *p_buf, const EnhancerPreset *p_preset,
                     const char *c_name, GBytes *p_want) {
   GError *p_err = NULL;
   char   *c_out = g_build_filename(c_dir, c_name, NULL);
   GFile  *p_out = g_file_new_for_path(c_out);
   g_assert_true(enhancer_export(p_buf, p_preset, p_out, &p_err));
   g_assert_no_error(p_err);
   GBytes *p_got = icc_read_embedded(p_out, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_got);
   g_assert_true(g_bytes_equal(p_want, p_got));
   g_bytes_unref(p_got);
   GeglBuffer *p_re = load_ok(p_out);
   guint8      c_px[4];
   preview_pixel(p_re, 0, 0, c_px);
   if (GGAZE_HAVE_JPEG || g_str_has_suffix(c_name, ".png")) {
      assert_rgb(c_name, c_px, 0, 0, 255, 15); /* reloaded managed */
   }
   g_object_unref(p_re);
   drop_temp(p_out);
   g_free(c_out);
}

/* A WebP carries no profile, so its pixels must be sRGB: the saver reads
 * them as sRGB and babl converts on the way out, no conversion node needed
 * -- which is why the export stays right with gegl:convert-space hidden.
 * The reload (the blue the preview showed) needs the loader's optional
 * WebP decoder. */
static void
export_webp(GeglBuffer *p_buf, const EnhancerPreset *p_preset) {
   GError *p_err = NULL;
   char   *c_out = g_build_filename(c_dir, "out.webp", NULL);
   GFile  *p_out = g_file_new_for_path(c_out);
   g_assert_true(enhancer_export(p_buf, p_preset, p_out, &p_err));
   g_assert_no_error(p_err);
   GeglBuffer *p_re = enhancer_load(p_out, &p_err);
   if (p_re == NULL) {
      g_test_message("no WebP decoder for the reload (%s)", p_err->message);
      g_clear_error(&p_err);
   } else {
      guint8 c_px[4];
      preview_pixel(p_re, 0, 0, c_px);
      assert_rgb("out.webp", c_px, 0, 0, 255, 15);
      g_object_unref(p_re);
   }
   drop_temp(p_out);
   g_free(c_out);
}

static void
test_export_preserves_profile(void) {
   GFile  *p_src  = fixture("swapped.png");
   GBytes *p_want = icc_read_embedded(p_src, NULL);
   g_assert_nonnull(p_want);
   GeglBuffer           *p_buf = load_ok(p_src);
   Enhancer             *p_e   = enhancer_new();
   const EnhancerPreset *p_contrast =
      g_ptr_array_index((GPtrArray *)enhancer_get_presets(p_e), 2);
   export_keeps_profile(p_buf, p_contrast, "out.png", p_want);
   export_keeps_profile(p_buf, p_contrast, "out.jpg", p_want);
   if (gegl_has_operation("gegl:webp-save")) {
      enhancer_test_set_missing_op("gegl:convert-space");
      export_webp(p_buf, p_contrast);
      enhancer_test_set_missing_op(NULL);
   } else {
      g_test_message("gegl:webp-save unavailable; skipping the webp export");
   }
   enhancer_delete(p_e);
   g_object_unref(p_buf);
   g_bytes_unref(p_want);
   g_object_unref(p_src);
}

/* A GEGL without the saver op: the export is refused as unsupported,
 * nothing is written. */
static void
test_missing_saver_is_not_supported(void) {
   GeglBuffer *p_buf = load_fixture("swapped.png");
   char       *c_out = g_build_filename(c_dir, "nosaver.png", NULL);
   GFile      *p_out = g_file_new_for_path(c_out);
   GError     *p_err = NULL;
   Enhancer   *p_e   = enhancer_new();
   enhancer_test_set_missing_op("gegl:png-save");
   g_assert_false(enhancer_export(
      p_buf, g_ptr_array_index((GPtrArray *)enhancer_get_presets(p_e), 2),
      p_out, &p_err));
   enhancer_delete(p_e);
   enhancer_test_set_missing_op(NULL);
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
   g_clear_error(&p_err);
   g_assert_false(g_file_test(c_out, G_FILE_TEST_EXISTS));
   g_object_unref(p_out);
   g_free(c_out);
   g_object_unref(p_buf);
}

/* --- the managed original (lazy, for hold-Space) ------------------------ */

typedef struct {
   GMainLoop  *p_loop;
   GdkTexture *p_tex;
   GError     *p_err;
} OrigWait;

static void
orig_done(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   OrigWait *p_w = p_data;
   p_w->p_tex    = enhancer_managed_original_finish(p_res, &p_w->p_err);
   g_main_loop_quit(p_w->p_loop);
}

/* The managed original of p_file (NULL when none), with *pp_err. */
static GdkTexture *
managed_original(GFile *p_file, GCancellable *p_cancel, GError **pp_err) {
   OrigWait t_w = {g_main_loop_new(NULL, FALSE), NULL, NULL};
   enhancer_managed_original_async(p_file, p_cancel, orig_done, &t_w);
   g_main_loop_run(t_w.p_loop);
   g_main_loop_unref(t_w.p_loop);
   if (t_w.p_err != NULL) {
      g_propagate_error(pp_err, t_w.p_err);
   }
   return (t_w.p_tex);
}

/* c_name's managed original, pixel (0, 0) is (r, g, b). */
static void
assert_original_is(const char *c_name, int i_r, int i_g, int i_b) {
   GFile      *p_file = fixture(c_name);
   GError     *p_err  = NULL;
   GdkTexture *p_tex  = managed_original(p_file, NULL, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_tex);
   GdkTextureDownloader *p_dl = gdk_texture_downloader_new(p_tex);
   gdk_texture_downloader_set_format(p_dl, GDK_MEMORY_R8G8B8A8);
   gsize   u_stride = 0;
   GBytes *p_bytes  = gdk_texture_downloader_download_bytes(p_dl, &u_stride);
   assert_rgb(c_name, g_bytes_get_data(p_bytes, NULL), i_r, i_g, i_b, 15);
   g_bytes_unref(p_bytes);
   gdk_texture_downloader_free(p_dl);
   g_object_unref(p_tex);
   g_object_unref(p_file);
}

/* The managed original is the file through the managed decode, converted
 * for display: blue for swapped.png, ~188 for the linear grey PNG, blue
 * for the CMYK JPEG (with libjpeg) -- CMYK and grey included, although
 * their working space is sRGB. A file with nothing to manage has none (no
 * error), and so has a missing file -- the managed original never falls
 * back to the loader, whose error that would be; a cancelled fetch is
 * CANCELLED. */
static void
test_managed_original(void) {
   assert_original_is("swapped.png", 0, 0, 255);
   assert_original_is("grey-icc.png", 188, 188, 188);
#if GGAZE_HAVE_JPEG
   assert_original_is("cmyk-icc.jpg", 0, 0, 255);
#endif
   GError *p_err  = NULL;
   GFile  *p_file = fixture("plain.jpg");
   g_assert_null(managed_original(p_file, NULL, &p_err));
   g_assert_no_error(p_err);
   g_object_unref(p_file);
   p_file = g_file_new_for_path("/nonexistent/ggaze/x.png");
   g_assert_null(managed_original(p_file, NULL, &p_err));
   g_assert_no_error(p_err); /* declined: never the loader's decode */
   g_object_unref(p_file);
   GCancellable *p_cancel = g_cancellable_new();
   g_cancellable_cancel(p_cancel);
   p_file = fixture("swapped.png");
   g_assert_null(managed_original(p_file, p_cancel, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
   g_clear_error(&p_err);
   g_object_unref(p_file);
   g_object_unref(p_cancel);
}

/* --- profiles babl must never see (xb2 review 3) -------------------------
 *
 * babl_space_from_icc() trusts tag data (a huge 'curv' count read past the
 * buffer or exited the process through babl_fatal), and on a CMYK profile
 * keeps a failed LCMS transform and crashes converting through it. Each
 * case below crashed the process before; now the file takes the loader
 * path (sRGB), the card says nothing is managed, and no managed original
 * exists. The profile is edited inside the JPEG's APP2 segment, so the
 * container stays sound. */

/* Offset of the profile inside p_a's (single) APP2 ICC_PROFILE segment. */
static gsize
jpeg_profile_at(const GByteArray *p_a) {
   for (gsize u = 2; u + 14 <= p_a->len; u++) {
      if (memcmp(p_a->data + u, "ICC_PROFILE", 12) == 0) {
         return (u + 14); /* the name, then the sequence number and count */
      }
   }
   g_assert_not_reached();
   return (0);
}

/* The data offset (in p_a) of tag c_sig of the profile at u_icc. */
static gsize
profile_tag_at(const GByteArray *p_a, gsize u_icc, const char *c_sig) {
   const guint8 *p       = p_a->data + u_icc;
   guint32       u_count = be32(p + 128);
   for (guint32 u = 0; u < u_count; u++) {
      if (memcmp(p + 132 + 12 * u, c_sig, 4) == 0) {
         return (u_icc + be32(p + 132 + 12 * u + 4));
      }
   }
   g_assert_not_reached();
   return (0);
}

static void
put32(guint8 *p, guint32 u) {
   p[0] = (guint8)(u >> 24);
   p[1] = (guint8)(u >> 16);
   p[2] = (guint8)(u >> 8);
   p[3] = (guint8)u;
}

/* p_a as c_name: declined everywhere -- the card, the render, the managed
 * original -- and decoded by the loader, sRGB. */
static void
assert_profile_declined(const char *c_name, const GByteArray *p_a) {
   GFile *p_file = temp_file(c_name, p_a);
   g_assert_false(enhancer_would_manage(p_file));
   GeglBuffer *p_buf = load_ok(p_file);
   g_assert_true(is_srgb(p_buf));
   g_object_unref(p_buf);
   g_assert_false(render_file_managed(p_file));
   GError     *p_err = NULL;
   GdkTexture *p_tex = managed_original(p_file, NULL, &p_err);
   g_assert_no_error(p_err);
   g_assert_null(p_tex);
   drop_temp(p_file);
}

/* swapped.jpg with its gTRC 'curv' count set to u_count. */
static void
check_curv_count(guint32 u_count) {
   GByteArray *p_a   = fixture_bytes("swapped.jpg");
   gsize       u_icc = jpeg_profile_at(p_a);
   put32(p_a->data + profile_tag_at(p_a, u_icc, "gTRC") + 8, u_count);
   assert_profile_declined("hugecurv.jpg", p_a);
   g_byte_array_unref(p_a);
}

static void
test_insane_profile_is_declined(void) {
   check_curv_count(0x01000000u); /* the review's repro */
   check_curv_count(0xffffffffu); /* a negative int to babl */
   /* A 'para' curve cut short, and a tag pointing past the profile. */
   GByteArray *p_a   = fixture_bytes("swapped.jpg");
   gsize       u_icc = jpeg_profile_at(p_a);
   memcpy(p_a->data + profile_tag_at(p_a, u_icc, "rTRC"), "para", 4);
   p_a->data[profile_tag_at(p_a, u_icc, "rTRC") + 9] = 3; /* needs 32 */
   assert_profile_declined("shortpara.jpg", p_a);
   g_byte_array_unref(p_a);
   p_a   = fixture_bytes("swapped.jpg");
   u_icc = jpeg_profile_at(p_a);
   put32(p_a->data + u_icc + 132 + 4, 0x7ffffff0u); /* the first tag */
   assert_profile_declined("tagout.jpg", p_a);
   g_byte_array_unref(p_a);
}

/* cmyk-icc.jpg's profile with one byte of its A2B0 lut8 header changed:
 * LCMS cannot build the transform (babl would keep the NULL one and crash
 * on the first conversion), so the managed path declines. The profile as
 * it is still passes (the test seam, through the same verdict table). */
static void
test_cmyk_profile_lcms_cannot_open_is_declined(void) {
   const struct {
      guint  u_at; /* into the A2B0 tag */
      guint8 u_val;
   } CASES[]         = {{8, 0}, {9, 0}, {10, 0}, {8, 9}};
   GByteArray *p_a   = fixture_bytes("cmyk-icc.jpg");
   gsize       u_icc = jpeg_profile_at(p_a);
   GBytes     *p_ok  = g_bytes_new(p_a->data + u_icc, be32(p_a->data + u_icc));
   g_assert_true(enhancer_test_profile_is_managed(p_ok));
   g_bytes_unref(p_ok);
   g_byte_array_unref(p_a);
   for (gsize u = 0; u < G_N_ELEMENTS(CASES); u++) {
      p_a   = fixture_bytes("cmyk-icc.jpg");
      u_icc = jpeg_profile_at(p_a);
      p_a->data[profile_tag_at(p_a, u_icc, "A2B0") + CASES[u].u_at] =
         CASES[u].u_val;
      GBytes *p_icc = g_bytes_new(p_a->data + u_icc, be32(p_a->data + u_icc));
      g_assert_false(enhancer_test_profile_is_managed(p_icc));
      g_bytes_unref(p_icc);
      assert_profile_declined("badclut.jpg", p_a);
      g_byte_array_unref(p_a);
   }
}

/* --- babl's own crashes and asserts (xb2 review 4) ----------------------
 *
 * Well-formed profiles babl 0.1.128 crashes or aborts on (each reproduced
 * against it): a 'para' whose reserved byte makes babl read it as a huge
 * 'curv', curves too long for the 64 KiB buffer babl writes every new
 * space's profile into, and 'para' break points outside babl's assertion.
 * Each must be declined -- the card, the render, the managed original --
 * before babl sees it, costing no slot. Built as swapped.png with its
 * iCCP replaced (icc_build.h), so every lane's build can decode it. */

/* p_icc zlib-compressed, as an iCCP chunk's data wants it. */
static GBytes *
zlib_bytes(GBytes *p_icc) {
   GConverter *p_z =
      G_CONVERTER(g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB, -1));
   GOutputStream *p_mem = g_memory_output_stream_new_resizable();
   GOutputStream *p_out = g_converter_output_stream_new(p_mem, p_z);
   gsize          u_len = 0;
   const void    *p_d   = g_bytes_get_data(p_icc, &u_len);
   g_assert_true(
      g_output_stream_write_all(p_out, p_d, u_len, NULL, NULL, NULL));
   g_assert_true(g_output_stream_close(p_out, NULL, NULL));
   GBytes *p_zb =
      g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(p_mem));
   g_object_unref(p_out);
   g_object_unref(p_mem);
   g_object_unref(p_z);
   return (p_zb);
}

/* swapped.png with its iCCP chunk carrying p_icc instead. */
static GByteArray *
png_with_profile(GBytes *p_icc) {
   GByteArray *p_a  = fixture_bytes("swapped.png");
   gsize       u_at = png_chunk_at(p_a, "iCCP");
   g_byte_array_remove_range(p_a, (guint)u_at, be32(p_a->data + u_at) + 12);
   GBytes       *p_z    = zlib_bytes(p_icc);
   gsize         u_zlen = 0;
   const guint8 *p_zd   = g_bytes_get_data(p_z, &u_zlen);
   GByteArray   *p_ch   = g_byte_array_new();
   guint8        c_len[4];
   put32(c_len, (guint32)(7 + u_zlen));
   g_byte_array_append(p_ch, c_len, 4);
   g_byte_array_append(p_ch, (const guint8 *)"iCCPggaze\0\0", 11);
   g_byte_array_append(p_ch, p_zd, (guint)u_zlen);
   g_byte_array_append(p_ch, (const guint8 *)"\0\0\0\0", 4); /* CRC */
   bytes_insert(p_a, u_at, p_ch->data, p_ch->len);
   png_fix_crc(p_a, u_at);
   g_byte_array_unref(p_ch);
   g_bytes_unref(p_z);
   return (p_a);
}

/* p_icc (taken) declined everywhere without a slot or a verdict. */
static void
assert_built_profile_declined(const char *c_what, GBytes *p_icc) {
   g_test_message("declined: %s", c_what);
   guint u_slots    = enhancer_test_profile_slots();
   guint u_verdicts = enhancer_test_profile_verdicts();
   g_assert_false(enhancer_test_profile_is_managed(p_icc));
   GByteArray *p_png = png_with_profile(p_icc);
   assert_profile_declined("crasher.png", p_png);
   g_assert_cmpuint(enhancer_test_profile_slots(), ==, u_slots);
   g_assert_cmpuint(enhancer_test_profile_verdicts(), ==, u_verdicts);
   g_byte_array_unref(p_png);
   g_bytes_unref(p_icc);
}

/* An RGB profile whose three curves are p_trc (taken). */
static GBytes *
rgb_of(const char *c_desc, GBytes *p_trc) {
   GBytes *p_icc = icc_build_rgb(c_desc, p_trc, p_trc, p_trc);
   g_bytes_unref(p_trc);
   return (p_icc);
}

/* An RGB profile of three distinct u_n-point curves. */
static GBytes *
rgb_of_three(const char *c_desc, guint32 u_n) {
   GBytes *p_r   = icc_build_curv(u_n, 1.61);
   GBytes *p_g   = icc_build_curv(u_n, 1.71);
   GBytes *p_b   = icc_build_curv(u_n, 1.91);
   GBytes *p_icc = icc_build_rgb(c_desc, p_r, p_g, p_b);
   g_bytes_unref(p_r);
   g_bytes_unref(p_g);
   g_bytes_unref(p_b);
   return (p_icc);
}

/* A type 3 / 4 'para' with break point d and slope c (the sRGB curve's
 * other parameters). */
static GBytes *
para_cd(guint16 u_fn, double f_c, double f_d) {
   const double P[7] = {2.4, 1 / 1.055, 0.055 / 1.055, f_c, f_d, 0.0, 0.0};
   return (icc_build_para(u_fn, P, u_fn == 3 ? 5 : 7));
}

static void
test_babl_crashers_are_declined(void) {
   GBytes     *p_para = para_cd(3, 1 / 12.92, 0.04045);
   GByteArray *p_arr  = g_bytes_unref_to_array(p_para);
   p_arr->data[4]     = 1; /* the review's SIGSEGV */
   p_arr->data[10]    = 0xff;
   p_arr->data[11]    = 0xff;
   assert_built_profile_declined(
      "para reserved byte",
      rgb_of("reserved", g_byte_array_free_to_bytes(p_arr)));
   assert_built_profile_declined("shared 65536-point curve",
                                 rgb_of("65536", icc_build_curv(65536, 1.7)));
   assert_built_profile_declined("32768-point curve",
                                 rgb_of("32768", icc_build_curv(32768, 1.7)));
   assert_built_profile_declined("three 11000-point curves",
                                 rgb_of_three("11000", 11000));
   assert_built_profile_declined("three 20000-point curves",
                                 rgb_of_three("20000", 20000));
   assert_built_profile_declined("para d = 1.0",
                                 rgb_of("d1", para_cd(3, 1 / 12.92, 1.0)));
   assert_built_profile_declined("para d = -0.1",
                                 rgb_of("dneg", para_cd(3, 1 / 12.92, -0.1)));
   assert_built_profile_declined("para c < 0",
                                 rgb_of("cneg", para_cd(3, -0.5, 0.04)));
   assert_built_profile_declined("para 4 c * d = 1.2",
                                 rgb_of("cd", para_cd(4, 30.0, 0.04)));
   const double CIE[3] = {2.2, 1.0, 0.0};
   assert_built_profile_declined("para type 1",
                                 rgb_of("cie", icc_build_para(1, CIE, 3)));
}

/* p_icc (taken) managed: babl takes it, and swapped.png carrying it
 * decodes in its space -- babl built the space and its profile copy (the
 * 64 KiB buffer) without incident. */
static void
assert_built_profile_managed(const char *c_what, GBytes *p_icc) {
   g_test_message("managed: %s", c_what);
   g_assert_true(enhancer_test_profile_is_managed(p_icc));
   GByteArray *p_png  = png_with_profile(p_icc);
   GFile      *p_file = temp_file("builtok.png", p_png);
   g_assert_true(enhancer_would_manage(p_file));
   GeglBuffer *p_buf = load_ok(p_file);
   g_assert_false(is_srgb(p_buf));
   g_object_unref(p_buf);
   drop_temp(p_file);
   g_byte_array_unref(p_png);
   g_bytes_unref(p_icc);
}

/* What real profiles carry still gets through: 4096- and 1024-point
 * curves, three distinct or shared, and 'para' types 0, 3 and 4. */
static void
test_real_size_curves_are_managed(void) {
   assert_built_profile_managed("three 4096-point curves",
                                rgb_of_three("4096", 4096));
   assert_built_profile_managed("shared 1024-point curve",
                                rgb_of("1024", icc_build_curv(1024, 1.83)));
   assert_built_profile_managed("para 4",
                                rgb_of("para4", para_cd(4, 0.5, 0.04)));
   const double G[1] = {1.93};
   assert_built_profile_managed("para 0",
                                rgb_of("para0", icc_build_para(0, G, 1)));
}

/* --- the slot accounting (xb2 review 4) ---------------------------------
 *
 * A slot is what a profile may add to babl's fixed tables. One babl
 * answers with a space it already had costs none -- unless it carries a
 * curve that space does not use, which babl parses and keeps all the
 * same. Profiles babl declines outright never reach it; and the verdicts
 * babl gives without a slot are kept only up to
 * GGAZE_ENHANCER_MAX_FREE_VERDICTS. */

/* Whether p_icc (taken) is managed and cost u_want slots. */
static void
assert_slots(GBytes *p_icc, gboolean b_managed, guint u_want) {
   guint u_was = enhancer_test_profile_slots();
   g_assert_true(enhancer_test_profile_is_managed(p_icc) == b_managed);
   g_assert_cmpuint(enhancer_test_profile_slots(), ==, u_was + u_want);
   g_bytes_unref(p_icc);
}

static void
test_slot_accounting(void) {
   GBytes     *p_c     = icc_build_curv(1, 1.37);
   GBytes     *p_k     = icc_build_curv(1, 1.53);
   IccBuildTag t_k[]   = {{"kTRC", p_k}};
   IccBuildTag t_rgb[] = {{"rTRC", p_k}, {"gTRC", p_k}, {"bTRC", p_k}};
   assert_slots(icc_build_rgb("slot a", p_c, p_c, p_c), TRUE, 1);
   assert_slots(icc_build_rgb("slot b", p_c, p_c, p_c), TRUE, 0);
   assert_slots(icc_build_rgb_with("slot k", p_c, p_c, p_c, t_k, 1), TRUE, 1);
   assert_slots(icc_build_gray("grey a", p_c), TRUE, 1);
   assert_slots(icc_build_gray("grey b", p_c), TRUE, 0);
   assert_slots(icc_build_gray_with("grey rgb", p_c, t_rgb, 3), TRUE, 1);
   GFile  *p_file = fixture("srgb-icc.png"); /* babl's own sRGB: free */
   GBytes *p_srgb = icc_read_embedded(p_file, NULL);
   g_object_unref(p_file);
   GByteArray *p_arr = g_bytes_unref_to_array(p_srgb);
   p_arr->data[p_arr->len - 1] ^= 1; /* bytes of its own, same space */
   assert_slots(g_byte_array_free_to_bytes(p_arr), FALSE, 0);
   g_bytes_unref(p_c);
   g_bytes_unref(p_k);
}

/* A profile of class c_class, space c_space, PCS c_pcs with swapped.png's
 * tags, less bTRC when b_no_blue, plus A2B0 and B2A0 when b_clut. */
static GBytes *
declinable(const char *c_class, const char *c_pcs, gboolean b_no_blue,
           gboolean b_clut) {
   GBytes     *p_c   = icc_build_curv(1, 1.45);
   GBytes     *p_x   = icc_build_xyz(0.4, 0.2, 0.1);
   GBytes     *p_lut = g_bytes_new_static("mft2\0\0\0\0", 8);
   IccBuildTag t[]   = {{"wtpt", p_x},   {"rXYZ", p_x},   {"gXYZ", p_x},
                        {"bXYZ", p_x},   {"rTRC", p_c},   {"gTRC", p_c},
                        {"A2B0", p_lut}, {"B2A0", p_lut}, {"bTRC", p_c}};
   gsize       u_n   = b_clut ? 9 : 6;
   if (!b_clut && !b_no_blue) {
      t[6] = t[8];
      u_n  = 7;
   }
   GBytes *p_icc = icc_build(c_class, "RGB ", c_pcs, t, u_n);
   g_bytes_unref(p_c);
   g_bytes_unref(p_x);
   g_bytes_unref(p_lut);
   return (p_icc);
}

/* babl's early declines never reach it: no slot, no verdict. */
static void
test_babl_declines_cost_nothing(void) {
   GBytes *CASES[] = {
      declinable("prtr", "XYZ ", FALSE, FALSE), /* a class babl refuses */
      declinable("mntr", "Lab ", FALSE, FALSE), /* a Lab PCS */
      declinable("mntr", "XYZ ", FALSE, TRUE),  /* both CLUT directions */
      declinable("mntr", "XYZ ", TRUE, FALSE),  /* no blue curve */
   };
   for (gsize u = 0; u < G_N_ELEMENTS(CASES); u++) {
      guint u_slots    = enhancer_test_profile_slots();
      guint u_verdicts = enhancer_test_profile_verdicts();
      g_assert_true(icc_profile_is_sane(CASES[u]));
      g_assert_false(enhancer_test_profile_is_managed(CASES[u]));
      g_assert_cmpuint(enhancer_test_profile_slots(), ==, u_slots);
      g_assert_cmpuint(enhancer_test_profile_verdicts(), ==, u_verdicts);
      g_bytes_unref(CASES[u]);
   }
   /* The same profile with its class fixed does reach babl. */
   GBytes *p_ok = declinable("mntr", "XYZ ", FALSE, FALSE);
   guint   u_v  = enhancer_test_profile_verdicts();
   (void)enhancer_test_profile_is_managed(p_ok);
   g_assert_cmpuint(enhancer_test_profile_verdicts(), ==, u_v + 1);
   g_bytes_unref(p_ok);
}

/* Slot-free verdicts past GGAZE_ENHANCER_MAX_FREE_VERDICTS drop the oldest:
 * the table stays bounded, no slot is spent, and a dropped profile asked
 * about again gets the same answer, again for free. */
static void
test_verdicts_are_bounded(void) {
   GBytes *p_c     = icc_build_curv(1, 1.37); /* test_slot_accounting's space */
   guint   u_slots = enhancer_test_profile_slots();
   for (guint u = 0; u < 2 * GGAZE_ENHANCER_MAX_FREE_VERDICTS + 8; u++) {
      char c_desc[32];
      g_snprintf(c_desc, sizeof(c_desc), "free %u", u);
      GBytes *p_icc = icc_build_rgb(c_desc, p_c, p_c, p_c);
      g_assert_true(enhancer_test_profile_is_managed(p_icc));
      g_bytes_unref(p_icc);
      g_assert_cmpuint(enhancer_test_profile_verdicts(), <=,
                       GGAZE_ENHANCER_MAX_PROFILES +
                          GGAZE_ENHANCER_MAX_FREE_VERDICTS);
   }
   GBytes *p_first = icc_build_rgb("free 0", p_c, p_c, p_c); /* dropped */
   g_assert_true(enhancer_test_profile_is_managed(p_first));
   g_assert_cmpuint(enhancer_test_profile_slots(), ==, u_slots);
   g_bytes_unref(p_first);
   g_bytes_unref(p_c);
}

/* --- cancellation (xb2 review 3) -----------------------------------------
 *
 * A cancelled load used to fall through to the loader with no cancellable:
 * a whole decode for a result nobody takes. The test seam cancels from
 * inside the load, as the managed path starts -- "while the check runs",
 * deterministically -- and the loader-decode counter says whether the
 * loader ran. */

static void
cancel_hook(gpointer p_data) {
   g_cancellable_cancel(G_CANCELLABLE(p_data));
}

typedef struct {
   GMainLoop *p_loop;
   GError    *p_err;
} ErrWait;

static void
apply_err_done(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   ErrWait    *p_w = p_data;
   GdkTexture *p_tex =
      enhancer_apply_chain_finish(p_res, NULL, NULL, NULL, &p_w->p_err);
   g_assert_null(p_tex);
   g_main_loop_quit(p_w->p_loop);
}

/* A render of c_name cancelled mid-check: CANCELLED, and no decode. */
static void
assert_render_cancelled(const char *c_name) {
   GCancellable *p_cancel = g_cancellable_new();
   Enhancer     *p_e      = enhancer_new();
   GFile        *p_file   = fixture(c_name);
   ErrWait       t_w      = {g_main_loop_new(NULL, FALSE), NULL};
   guint         u_before = enhancer_test_loader_decodes();
   enhancer_test_set_load_hook(cancel_hook, p_cancel);
   enhancer_apply_chain_async(p_file, enhancer_get_presets(p_e), 1u << 2, NULL,
                              p_cancel, apply_err_done, &t_w);
   g_main_loop_run(t_w.p_loop);
   enhancer_test_set_load_hook(NULL, NULL);
   g_assert_error(t_w.p_err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
   g_assert_cmpuint(enhancer_test_loader_decodes(), ==, u_before);
   g_clear_error(&t_w.p_err);
   g_main_loop_unref(t_w.p_loop);
   g_object_unref(p_file);
   enhancer_delete(p_e);
   g_object_unref(p_cancel);
}

/* A render cancelled while the managed path checks (swapped.png: its
 * checks see the cancellation and decline; plain.jpg: nothing to manage,
 * declined at once) returns CANCELLED without the loader's decode; so
 * does the managed original, which also never falls back to a plain
 * decode when it declines for any other reason. The uncancelled render
 * of an untagged file does decode, once. */
static void
test_cancelled_load_decodes_nothing(void) {
   assert_render_cancelled("swapped.png");
   assert_render_cancelled("plain.jpg");
#if GGAZE_HAVE_JPEG
   assert_render_cancelled("swapped.jpg");
#endif
   GCancellable *p_cancel = g_cancellable_new();
   GFile        *p_file   = fixture("swapped.png");
   GError       *p_err    = NULL;
   guint         u_before = enhancer_test_loader_decodes();
   enhancer_test_set_load_hook(cancel_hook, p_cancel);
   g_assert_null(managed_original(p_file, p_cancel, &p_err));
   enhancer_test_set_load_hook(NULL, NULL);
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
   g_clear_error(&p_err);
   g_object_unref(p_file);
   g_object_unref(p_cancel);
   p_file = fixture("plain.jpg");
   g_assert_null(managed_original(p_file, NULL, &p_err));
   g_assert_no_error(p_err);
   g_assert_cmpuint(enhancer_test_loader_decodes(), ==, u_before);
   g_assert_false(render_file_managed(p_file));
   g_assert_cmpuint(enhancer_test_loader_decodes(), ==, u_before + 1);
   g_object_unref(p_file);
}

/* --- the info card's answer is header-deep (xb2 review 3) ---------------- */

/* swapped.jpg (8x8; its SOF0 height at byte 701, width at 703) cut inside
 * its scan: every header is there, so the card says "may be managed", but
 * the render's whole-file checks decline it and the loader decodes the cut
 * file as it always did. With SOF sizes past the shared caps -- one side
 * over GGAZE_IMAGE_MAX_SIDE, or 30000 x 30000 over GGAZE_IMAGE_MAX_PIXELS
 * -- the card says no, as the render would (every build). */
static void
test_would_manage_is_header_deep(void) {
#if GGAZE_HAVE_JPEG
   GByteArray *p_cut = fixture_bytes("swapped.jpg");
   g_byte_array_set_size(p_cut, p_cut->len - 4); /* EOI and some data */
   GFile *p_file = temp_file("cutscan.jpg", p_cut);
   g_assert_true(enhancer_would_manage(p_file));
   g_assert_false(render_file_managed(p_file));
   drop_temp(p_file);
   g_byte_array_unref(p_cut);
#endif
   const guint8 C_SIDE[2] = {0x80, 0x01};             /* 32769 rows */
   const guint8 C_AREA[4] = {0x75, 0x30, 0x75, 0x30}; /* 30000 x 30000 */
   const struct {
      const guint8 *p_patch;
      gsize         u_len;
   } CASES[] = {{C_SIDE, sizeof(C_SIDE)}, {C_AREA, sizeof(C_AREA)}};
   for (gsize u = 0; u < G_N_ELEMENTS(CASES); u++) {
      GByteArray *p_a = fixture_bytes("swapped.jpg");
      memcpy(p_a->data + 701, CASES[u].p_patch, CASES[u].u_len);
      GFile *p_big = temp_file("bigsof.jpg", p_a);
      g_assert_false(enhancer_would_manage(p_big));
      drop_temp(p_big);
      g_byte_array_unref(p_a);
   }
}

/* --- babl's fixed tables: the per-process profile cap --------------------
 *
 * Distinct profiles that may grow babl's space / curve tables stop at
 * GGAZE_ENHANCER_MAX_PROFILES; past that a NEW profile is declined, while
 * one already seen keeps its verdict (so a folder of the same camera's
 * files never runs out). Registered last among the managed cases: it uses
 * the cap up. */

/* swapped.png's profile with its red primary's X nudged by u_step: a
 * distinct, valid, non-sRGB profile per step. */
static GBytes *
nudged_profile(guint u_step) {
   GFile  *p_file = fixture("swapped.png");
   GBytes *p_icc  = icc_read_embedded(p_file, NULL);
   g_object_unref(p_file);
   gsize       u_len = 0;
   const void *p_src = g_bytes_get_data(p_icc, &u_len);
   guint8     *p     = g_memdup2(p_src, u_len);
   g_bytes_unref(p_icc);
   guint32 u_count = be32(p + 128);
   for (guint32 u = 0; u < u_count; u++) {
      if (memcmp(p + 132 + 12 * u, "rXYZ", 4) == 0) {
         guint8 *p_x = p + be32(p + 132 + 12 * u + 4) + 8;
         put32(p_x, be32(p_x) + 1024 * (u_step + 1));
      }
   }
   return (g_bytes_new_take(p, u_len));
}

static void
test_profile_cap(void) {
   GFile  *p_file = fixture("swapped.png");
   GBytes *p_seen = icc_read_embedded(p_file, NULL);
   g_object_unref(p_file);
   g_assert_true(enhancer_test_profile_is_managed(p_seen));
   /* Until a few past the cap; a nudge babl maps onto a space it already
    * has takes no slot, hence the generous bound. */
   guint u_new = 0, u_declined = 0;
   for (guint u = 0; u < 4 * GGAZE_ENHANCER_MAX_PROFILES && u_declined < 4;
        u++) {
      GBytes  *p_icc = nudged_profile(u);
      guint    u_was = enhancer_test_profile_slots();
      gboolean b_ok  = enhancer_test_profile_is_managed(p_icc);
      g_assert_true(b_ok == (u_was < GGAZE_ENHANCER_MAX_PROFILES));
      u_new += b_ok ? 1 : 0;
      u_declined += b_ok ? 0 : 1;
      g_assert_true(enhancer_test_profile_is_managed(p_icc) == b_ok);
      g_bytes_unref(p_icc);
   }
   g_test_message("%u new profiles before the cap", u_new);
   g_assert_cmpuint(u_declined, ==, 4);
   g_assert_cmpuint(enhancer_test_profile_slots(), ==,
                    GGAZE_ENHANCER_MAX_PROFILES);
   g_assert_true(enhancer_test_profile_is_managed(p_seen)); /* kept */
   g_assert_true(render_managed("swapped.png"));
   g_bytes_unref(p_seen);
}

/* --- fuzz: the profile gate and babl together ---------------------------
 *
 * Damage to profiles through the managed path's whole profile gate
 * (icc_babl_kind, the CMYK LCMS check, babl) and on into babl: every
 * profile that gets through makes babl build its space, and pixels are
 * converted through that space both ways (as the preview and the chain /
 * export do) and its profile copied out (as the savers do). No crash, no
 * assertion, no babl_fatal exit, whatever passes. The seeds are the
 * fixture profiles and built ones with the curve kinds the fixtures lack
 * ('para' 0 / 3 / 4, long 'curv' tables, a grey 'para', an RGB profile
 * with a kTRC), and half the edits are aimed INSIDE a tag -- a parameter
 * set to an edge value, a function type, a reserved byte, a curve count --
 * where babl's own crashes were, so most edited profiles stay well-formed
 * enough to reach babl. In subprocesses, because each profile babl takes
 * may take a slot of its fixed tables and the per-process cap then keeps
 * new ones from babl: each round starts with empty tables and runs until
 * its slots are spent. The rounds run with BABL_PATH_LENGTH=1: babl then
 * converts through its reference fish (the space's own curves and matrix,
 * in double -- what is under test) instead of timing candidate paths for
 * every new space, ~1 s each, which made a round take 20 s. Seeded per
 * round, so a failure reproduces (GGAZE_FUZZ_ROUND=<n> and
 * BABL_PATH_LENGTH=1 with -p /enhancer_icc/profile_fuzz/subprocess). */

#define FUZZ_ROUNDS 12
#define FUZZ_RUNS 300

/* One random edit of p_arr: a byte, a 32-bit field set to an edge value,
 * a cut, or the header's size field brought back in line. */
static void
fuzz_mutate(GRand *p_rand, GByteArray *p_arr) {
   static const guint32 C_EDGE[] = {
      0, 1, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0x01000000u, 20, 14};
   guint u_pos = (guint)g_rand_int_range(p_rand, 0, (gint32)p_arr->len);
   switch (g_rand_int_range(p_rand, 0, 4)) {
   case 0:
      p_arr->data[u_pos] = (guint8)g_rand_int_range(p_rand, 0, 256);
      break;
   case 1:
      if (u_pos + 4 <= p_arr->len) {
         put32(p_arr->data + u_pos,
               C_EDGE[g_rand_int_range(p_rand, 0, G_N_ELEMENTS(C_EDGE))]);
      }
      break;
   case 2:
      g_byte_array_set_size(
         p_arr, (guint)g_rand_int_range(p_rand, 132, (gint32)p_arr->len + 1));
      break;
   default:
      put32(p_arr->data, p_arr->len);
      break;
   }
}

/* One edit inside the data of a random tag of p_arr (whose table is known
 * to lie inside it and whose tag is at least 16 bytes): an s15Fixed16
 * parameter set to a value at or across one of babl's edges, the 'para'
 * function type, a reserved byte, or a 'curv' count. */
static void
fuzz_edit_tag(GRand *p_rand, guint8 *p_tag, guint32 u_size) {
   static const double  C_PARAM[] = {0.0,    1.0,       -0.1,    0.9975,
                                     0.9985, 30.0,      2.4,     -2.0,
                                     0.04,   1 / 12.92, 32767.0, -32768.0};
   static const guint32 C_COUNT[] = {0, 1, 2, 4096, 4097, 65536};
   guint                u_slots   = (u_size - 12) / 4;
   switch (g_rand_int_range(p_rand, 0, 4)) {
   case 0: {
      double f_v = C_PARAM[g_rand_int_range(p_rand, 0, G_N_ELEMENTS(C_PARAM))];
      guint  u_k = (guint)g_rand_int_range(p_rand, 0, (gint32)u_slots);
      put32(p_tag + 12 + 4 * u_k, (guint32)(gint32)(f_v * 65536.0));
      break;
   }
   case 1:
      p_tag[9] = (guint8)g_rand_int_range(p_rand, 0, 6);
      break;
   case 2:
      p_tag[g_rand_int_range(p_rand, 4, 8)] =
         (guint8)g_rand_int_range(p_rand, 0, 256);
      break;
   default:
      put32(p_tag + 8,
            C_COUNT[g_rand_int_range(p_rand, 0, G_N_ELEMENTS(C_COUNT))]);
      break;
   }
}

/* fuzz_edit_tag() on a random tag of p_arr, when its table and that tag
 * lie inside it (an earlier edit may have broken them). */
static void
fuzz_mutate_tag(GRand *p_rand, GByteArray *p_arr) {
   guint32 u_count = be32(p_arr->data + 128);
   if (u_count == 0 || u_count > 64 || 132 + 12 * u_count > p_arr->len) {
      return;
   }
   const guint8 *p_e =
      p_arr->data + 132 + 12 * g_rand_int_range(p_rand, 0, (gint32)u_count);
   guint32 u_off  = be32(p_e + 4);
   guint32 u_size = be32(p_e + 8);
   if (u_size >= 16 && u_off <= p_arr->len && u_size <= p_arr->len - u_off) {
      fuzz_edit_tag(p_rand, p_arr->data + u_off, u_size);
   }
}

/* Built seeds (taken into p_seeds): the curve kinds the fixtures lack. */
static void
fuzz_built_seeds(GPtrArray *p_seeds) {
   const double SRGB[5] = {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045};
   const double P4[7]   = {2.2, 0.9, 0.1, 0.5, 0.05, 0.01, 0.0};
   const double G[1]    = {1.8};
   GBytes      *p_p3    = icc_build_para(3, SRGB, 5);
   GBytes      *p_p4    = icc_build_para(4, P4, 7);
   GBytes      *p_p0    = icc_build_para(0, G, 1);
   GBytes      *p_lut   = icc_build_curv(1024, 1.9);
   IccBuildTag  t_k[]   = {{"kTRC", p_p0}};
   g_ptr_array_add(p_seeds, icc_build_rgb("fuzz p3", p_p3, p_p3, p_p3));
   g_ptr_array_add(p_seeds, icc_build_rgb("fuzz mix", p_p4, p_p0, p_lut));
   g_ptr_array_add(p_seeds, icc_build_rgb("fuzz lut", p_lut, p_lut, p_lut));
   g_ptr_array_add(p_seeds, icc_build_gray("fuzz grey", p_p3));
   g_ptr_array_add(p_seeds,
                   icc_build_rgb_with("fuzz k", p_p3, p_p3, p_p3, t_k, 1));
   g_bytes_unref(p_p3);
   g_bytes_unref(p_p4);
   g_bytes_unref(p_p0);
   g_bytes_unref(p_lut);
}

static GPtrArray *
fuzz_seeds(void) {
   const char *C_NAMES[] = {"swapped.png", "srgb-icc.png", "grey-icc.png",
                            "cmyk-icc.jpg"};
   GPtrArray  *p_seeds =
      g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
   for (gsize u = 0; u < G_N_ELEMENTS(C_NAMES); u++) {
      GFile *p_file = fixture(C_NAMES[u]);
      g_ptr_array_add(p_seeds, icc_read_embedded(p_file, NULL));
      g_object_unref(p_file);
   }
   fuzz_built_seeds(p_seeds);
   return (p_seeds);
}

/* Pixels through p_space both ways, and its profile copied out: what the
 * preview (to sRGB), the chain and the savers (into the space -- never a
 * CMYK one, whose chain runs in sRGB) and the export's profile ask of it. */
static void
fuzz_use_space(const Babl *p_space) {
   const char *c_fmt = babl_space_is_gray(p_space)   ? "Y' u8"
                       : babl_space_is_cmyk(p_space) ? "cmyk u8"
                                                     : "R'G'B' u8";
   const Babl *p_in  = babl_format_with_space(c_fmt, p_space);
   const Babl *p_out = babl_format("R'G'B'A u8");
   guint8      c_px[16 * 4], c_rgba[16 * 4];
   for (guint u = 0; u < sizeof(c_px); u++) {
      c_px[u] = (guint8)(u * 17);
   }
   babl_process(babl_fish(p_in, p_out), c_px, c_rgba, 16);
   if (!babl_space_is_cmyk(p_space)) {
      babl_process(babl_fish(p_out, p_in), c_rgba, c_px, 16);
   }
   int i_len = 0;
   g_assert_nonnull(babl_space_get_icc(p_space, &i_len));
   g_assert_cmpint(i_len, >, 0);
}

/* One fuzz case: a seed, edited u_n times. */
static GBytes *
fuzz_case(GRand *p_rand, GBytes *p_seed) {
   gsize       u_len = 0;
   const void *p_d   = g_bytes_get_data(p_seed, &u_len);
   GByteArray *p_arr = g_byte_array_sized_new((guint)u_len);
   g_byte_array_append(p_arr, p_d, (guint)u_len);
   guint u_n = (guint)g_rand_int_range(p_rand, 1, 4);
   for (guint u = 0; u < u_n; u++) {
      if (g_rand_boolean(p_rand)) {
         fuzz_mutate_tag(p_rand, p_arr);
      } else {
         fuzz_mutate(p_rand, p_arr);
      }
   }
   return (g_byte_array_free_to_bytes(p_arr));
}

static void
test_profile_fuzz_subprocess(void) {
   const char *c_round = g_getenv("GGAZE_FUZZ_ROUND");
   GRand      *p_rand =
      g_rand_new_with_seed(0x1cc0u + (c_round != NULL ? atoi(c_round) : 0));
   GPtrArray *p_seeds   = fuzz_seeds();
   guint      u_managed = 0, u_run = 0;
   for (; u_run < FUZZ_RUNS &&
          enhancer_test_profile_slots() < GGAZE_ENHANCER_MAX_PROFILES;
        u_run++) {
      GBytes *p_b =
         fuzz_case(p_rand, g_ptr_array_index(p_seeds, u_run % p_seeds->len));
      const Babl *p_space = enhancer_test_profile_space(p_b);
      if (p_space != NULL) {
         fuzz_use_space(p_space);
         u_managed++;
      }
      g_bytes_unref(p_b);
   }
   g_test_message("%u of %u edited profiles managed, %u babl slots, %u "
                  "verdicts",
                  u_managed, u_run, enhancer_test_profile_slots(),
                  enhancer_test_profile_verdicts());
   /* The point of the round: babl itself was reached. */
   g_assert_cmpuint(enhancer_test_profile_verdicts(), >, 0);
   g_assert_cmpuint(u_managed, >, 0);
   g_ptr_array_unref(p_seeds);
   g_rand_free(p_rand);
}

static void
test_profile_fuzz(void) {
   g_setenv("BABL_PATH_LENGTH", "1", TRUE); /* the subprocesses' babl */
   for (guint u = 0; u < FUZZ_ROUNDS; u++) {
      char c_round[16];
      g_snprintf(c_round, sizeof(c_round), "%u", u);
      g_setenv("GGAZE_FUZZ_ROUND", c_round, TRUE);
      g_test_trap_subprocess("/enhancer_icc/profile_fuzz/subprocess",
                             120 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_DEFAULT);
      g_test_trap_assert_passed();
   }
   g_unsetenv("GGAZE_FUZZ_ROUND");
   g_unsetenv("BABL_PATH_LENGTH");
}

/* --- the local camera corpus ---------------------------------------------- */

/* The babl space c_path's embedded profile parses to, or NULL. A real
 * camera profile babl parses must also pass icc_profile_is_sane() and
 * icc_babl_kind(): the gate in front of babl must not cost a real file its
 * management. (The corpus is trusted local data, so babl is asked
 * directly here.) */
static const Babl *
profile_space(GFile *p_file) {
   GBytes *p_icc = icc_read_embedded(p_file, NULL);
   if (!icc_is_profile(p_icc)) {
      g_clear_pointer(&p_icc, g_bytes_unref);
      return (NULL);
   }
   gsize       u_len  = 0;
   const char *c_data = g_bytes_get_data(p_icc, &u_len);
   const char *c_err  = NULL;
   const Babl *p_space =
      babl_space_from_icc(c_data, (int)u_len, BABL_ICC_INTENT_DEFAULT, &c_err);
   if (p_space != NULL) {
      g_assert_true(icc_profile_is_sane(p_icc));
      g_assert_cmpint(icc_babl_kind(p_icc), !=, ICC_BABL_NONE);
   }
   g_bytes_unref(p_icc);
   return (p_space);
}

/* One profiled corpus file: the completeness walk vouches for it and
 * reads a size (a Pixel photo's SOF lies past 64 KiB), and a file with a
 * non-sRGB profile really decodes managed at that size. */
static void
check_corpus_file(GFile *p_file, const Babl *p_space) {
   IntactSize t_size = {0, 0, 0};
   GError    *p_err  = NULL;
   char      *c_path = g_file_get_path(p_file);
   gboolean   b_png  = g_str_has_suffix(c_path, ".png");
   g_assert_true(b_png ? intact_png(p_file, NULL, &t_size, &p_err)
                       : intact_jpeg(p_file, NULL, &t_size, &p_err));
   g_assert_no_error(p_err);
   g_assert_cmpuint(t_size.u_w, >, 0);
   g_assert_cmpuint(t_size.u_h, >, 0);
   if (p_space != babl_space("sRGB") && (b_png || GGAZE_HAVE_JPEG) &&
       !enhancer_would_manage(p_file)) {
      /* Only past the per-process profile cap (a corpus with more
       * distinct non-sRGB profiles than GGAZE_ENHANCER_MAX_PROFILES). */
      g_assert_cmpuint(enhancer_test_profile_slots(), ==,
                       GGAZE_ENHANCER_MAX_PROFILES);
      g_test_message("%s: past the profile cap", c_path);
   } else if (p_space != babl_space("sRGB") && (b_png || GGAZE_HAVE_JPEG)) {
      g_test_message("%s: managed", c_path);
      GeglBuffer *p_buf = load_ok(p_file);
      g_assert_false(is_srgb(p_buf) && !babl_space_is_cmyk(p_space) &&
                     !babl_space_is_gray(p_space));
      g_object_unref(p_buf);
   }
   g_free(c_path);
}

/* Every profiled PNG / JPEG in ./sample-images (skipped when absent: CI
 * has only tests/fixtures). */
static void
test_sample_images_profiled_files(void) {
   const char *c_sample = g_getenv("GGAZE_SAMPLE_DIR");
   GDir       *p_dir = c_sample != NULL ? g_dir_open(c_sample, 0, NULL) : NULL;
   if (p_dir == NULL) {
      g_test_skip("no ./sample-images corpus");
      return;
   }
   guint       u_seen = 0;
   const char *c_name;
   while ((c_name = g_dir_read_name(p_dir)) != NULL) {
      char       *c_path  = g_build_filename(c_sample, c_name, NULL);
      GFile      *p_file  = g_file_new_for_path(c_path);
      const Babl *p_space = g_file_test(c_path, G_FILE_TEST_IS_REGULAR)
                               ? profile_space(p_file)
                               : NULL;
      if (p_space != NULL) {
         check_corpus_file(p_file, p_space);
         u_seen++;
      }
      g_object_unref(p_file);
      g_free(c_path);
   }
   g_dir_close(p_dir);
   g_test_message("%u profiled corpus files, %u babl slots", u_seen,
                  enhancer_test_profile_slots());
}

/* The managed decode, its fallbacks and the export. */
static void
add_decode_tests(void) {
   g_test_add_func("/enhancer_icc/managed_png_and_jpeg",
                   test_managed_png_and_jpeg);
   g_test_add_func("/enhancer_icc/managed_orientation",
                   test_managed_orientation);
   g_test_add_func("/enhancer_icc/cmyk_and_grey_run_in_srgb",
                   test_cmyk_and_grey_run_in_srgb);
   g_test_add_func("/enhancer_icc/chain_keeps_the_space",
                   test_chain_keeps_the_space);
   g_test_add_func("/enhancer_icc/sof_past_64k_is_managed",
                   test_sof_past_64k_is_managed);
   g_test_add_func("/enhancer_icc/padded_jpeg_is_managed",
                   test_padded_jpeg_is_managed);
   g_test_add_func("/enhancer_icc/profile_for_other_components_is_declined",
                   test_profile_for_other_components_is_declined);
   g_test_add_func("/enhancer_icc/would_manage", test_would_manage);
   g_test_add_func("/enhancer_icc/srgb_profile_is_byte_identical",
                   test_srgb_profile_is_byte_identical);
   g_test_add_func("/enhancer_icc/untagged_and_broken_profiles_stay_srgb",
                   test_untagged_and_broken_profiles_stay_srgb);
   g_test_add_func("/enhancer_icc/non_local_file_takes_the_loader",
                   test_non_local_file_takes_the_loader);
   g_test_add_func("/enhancer_icc/missing_loader_op_takes_the_loader",
                   test_missing_loader_op_takes_the_loader);
   g_test_add_func("/enhancer_icc/truncated_files_get_the_loaders_verdict",
                   test_truncated_files_get_the_loaders_verdict);
   g_test_add_func("/enhancer_icc/broken_headers_get_the_loaders_verdict",
                   test_broken_headers_get_the_loaders_verdict);
   g_test_add_func("/enhancer_icc/two_sof_jpeg_gets_the_loaders_verdict",
                   test_two_sof_jpeg_gets_the_loaders_verdict);
   g_test_add_func(
      "/enhancer_icc/cut_progressive_jpeg_gets_the_loaders_verdict",
      test_cut_progressive_jpeg_gets_the_loaders_verdict);
   g_test_add_func("/enhancer_icc/extent_check_catches_what_the_walk_cannot",
                   test_extent_check_catches_what_the_walk_cannot);
   g_test_add_func("/enhancer_icc/corrupt_png_data_is_an_error",
                   test_corrupt_png_data_is_an_error);
   g_test_add_func("/enhancer_icc/export_preserves_profile",
                   test_export_preserves_profile);
   g_test_add_func("/enhancer_icc/missing_saver_is_not_supported",
                   test_missing_saver_is_not_supported);
}

/* The profile gate, babl's tables, cancellation and the corpus; the
 * profile cap last (it uses the cap up). */
static void
add_profile_tests(void) {
   g_test_add_func("/enhancer_icc/managed_original", test_managed_original);
   g_test_add_func("/enhancer_icc/insane_profile_is_declined",
                   test_insane_profile_is_declined);
   g_test_add_func("/enhancer_icc/cmyk_profile_lcms_cannot_open_is_declined",
                   test_cmyk_profile_lcms_cannot_open_is_declined);
   g_test_add_func("/enhancer_icc/babl_crashers_are_declined",
                   test_babl_crashers_are_declined);
   g_test_add_func("/enhancer_icc/real_size_curves_are_managed",
                   test_real_size_curves_are_managed);
   g_test_add_func("/enhancer_icc/slot_accounting", test_slot_accounting);
   g_test_add_func("/enhancer_icc/babl_declines_cost_nothing",
                   test_babl_declines_cost_nothing);
   g_test_add_func("/enhancer_icc/verdicts_are_bounded",
                   test_verdicts_are_bounded);
   g_test_add_func("/enhancer_icc/cancelled_load_decodes_nothing",
                   test_cancelled_load_decodes_nothing);
   g_test_add_func("/enhancer_icc/would_manage_is_header_deep",
                   test_would_manage_is_header_deep);
   g_test_add_func("/enhancer_icc/sample_images_profiled_files",
                   test_sample_images_profiled_files);
   g_test_add_func("/enhancer_icc/profile_fuzz", test_profile_fuzz);
   g_test_add_func("/enhancer_icc/profile_fuzz/subprocess",
                   test_profile_fuzz_subprocess);
   g_test_add_func("/enhancer_icc/profile_cap", test_profile_cap);
}

int
main(int argc, char **argv) {
   gegl_init(&argc, &argv);
   g_test_init(&argc, &argv, NULL);
   c_dir = g_dir_make_tmp("ggaze-enhicc-XXXXXX", NULL);
   g_assert_nonnull(c_dir);
   add_decode_tests();
   add_profile_tests();
   int i_rc = g_test_run();
   g_rmdir(c_dir);
   g_free(c_dir);
   gegl_exit();
   return (i_rc);
}
