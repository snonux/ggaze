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
 * the async render's managed original, and -- when ./sample-images is
 * there -- every profiled camera file.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "enhancer.h"
#include "enhancer-gegl.h"
#include "icc.h"
#include "loader/intact.h"
#include "loader/loader.h"
#include "mem_file.h"
#include "transform.h"

#include <glib.h>
#include <glib/gstdio.h>
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

/* --- managed decodes ---------------------------------------------------- */

/* Tagged with the profile's space, previewed managed: PNG and JPEG. */
static void
test_managed_png_and_jpeg(void) {
   g_assert_false(fixture_pixel_is("swapped.png", 0, 0, 0, 0, 255));
   g_assert_false(fixture_pixel_is("swapped.jpg", 0, 0, 0, 0, 255));
}

/* swapped-rot6.jpg: 16x8 stored, left half red / right half green under
 * the swapped profile, Orientation 6. Upright 8x16 with the stored left
 * half on top, and managed: blue on top, green below. */
static void
test_managed_orientation(void) {
   GeglBuffer *p_buf = load_fixture("swapped-rot6.jpg");
   g_assert_false(is_srgb(p_buf));
   g_assert_cmpint(gegl_buffer_get_width(p_buf), ==, 8);
   g_assert_cmpint(gegl_buffer_get_height(p_buf), ==, 16);
   guint8 c_px[4];
   preview_pixel(p_buf, 0, 0, c_px);
   assert_rgb("rot6 top", c_px, 0, 0, 255, 15);
   preview_pixel(p_buf, 7, 15, c_px);
   assert_rgb("rot6 bottom", c_px, 0, 255, 0, 15);
   g_object_unref(p_buf);
}

/* CMYK and grey profiles are managed too, but the chain runs in sRGB
 * (their pixels have no meaning in an RGB format of their space). */
static void
test_cmyk_and_grey_run_in_srgb(void) {
   g_assert_true(fixture_pixel_is("cmyk-icc.jpg", 0, 0, 0, 0, 255));
   /* linear grey 128/255 is sRGB ~188 */
   g_assert_true(fixture_pixel_is("grey-icc.png", 0, 0, 188, 188, 188));
   g_assert_true(fixture_pixel_is("grey-icc.jpg", 0, 0, 188, 188, 188));
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
      assert_rgb(C_NAMES[u], c_px, 0, 0, 255, 15); /* managed */
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
   g_assert_false(is_srgb(p_buf));
   g_object_unref(p_buf);
   drop_temp(p_file);
   g_byte_array_unref(p_a);
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
   g_assert_false(fixture_is_srgb("swapped.jpg"));
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
   g_assert_true(intact_png(p_file, NULL, &p_err));
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
   assert_rgb(c_name, c_px, 0, 0, 255, 15);
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

/* --- the async render's managed original --------------------------------- */

typedef struct {
   GMainLoop  *p_loop;
   GdkTexture *p_tex;
   GdkTexture *p_orig;
} RenderWait;

static void
render_done(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   RenderWait *p_w = p_data;
   p_w->p_tex =
      enhancer_apply_chain_finish(p_res, NULL, NULL, &p_w->p_orig, NULL);
   g_main_loop_quit(p_w->p_loop);
}

/* Render c_name with Contrast, asking for the original when b_want; the
 * managed original (NULL when not managed / not asked) is returned. */
static GdkTexture *
render_original(const char *c_name, gboolean b_want) {
   Enhancer  *p_e    = enhancer_new();
   GFile     *p_file = fixture(c_name);
   RenderWait t_w    = {g_main_loop_new(NULL, FALSE), NULL, NULL};
   enhancer_apply_chain_async(p_file, enhancer_get_presets(p_e), 1u << 2, NULL,
                              b_want, NULL, render_done, &t_w);
   g_main_loop_run(t_w.p_loop);
   g_assert_nonnull(t_w.p_tex);
   g_object_unref(t_w.p_tex);
   g_main_loop_unref(t_w.p_loop);
   g_object_unref(p_file);
   enhancer_delete(p_e);
   return (t_w.p_orig);
}

/* A managed render hands back its original (the identity chain, blue for
 * swapped.png) when asked; not asked, or not managed, it hands back none. */
static void
test_render_returns_the_managed_original(void) {
   GdkTexture *p_orig = render_original("swapped.png", TRUE);
   g_assert_nonnull(p_orig);
   GdkTextureDownloader *p_dl = gdk_texture_downloader_new(p_orig);
   gdk_texture_downloader_set_format(p_dl, GDK_MEMORY_R8G8B8A8);
   gsize   u_stride = 0;
   GBytes *p_bytes  = gdk_texture_downloader_download_bytes(p_dl, &u_stride);
   assert_rgb("managed original", g_bytes_get_data(p_bytes, NULL), 0, 0, 255,
              15);
   g_bytes_unref(p_bytes);
   gdk_texture_downloader_free(p_dl);
   g_object_unref(p_orig);
   g_assert_null(render_original("swapped.png", FALSE));
   g_assert_null(render_original("plain.jpg", TRUE));
}

/* --- the local camera corpus ---------------------------------------------- */

/* The babl space c_path's embedded profile parses to, or NULL. */
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
   g_bytes_unref(p_icc);
   return (p_space);
}

/* One profiled corpus file: the completeness walk vouches for it and
 * reads a size (a Pixel photo's SOF lies past 64 KiB), and a file with a
 * non-sRGB profile really decodes managed at that size. */
static void
check_corpus_file(GFile *p_file, const Babl *p_space) {
   IntactSize t_size = {0, 0};
   GError    *p_err  = NULL;
   char      *c_path = g_file_get_path(p_file);
   gboolean   b_png  = g_str_has_suffix(c_path, ".png");
   g_assert_true(b_png ? intact_png(p_file, &t_size, &p_err)
                       : intact_jpeg(p_file, &t_size, &p_err));
   g_assert_no_error(p_err);
   g_assert_cmpuint(t_size.u_w, >, 0);
   g_assert_cmpuint(t_size.u_h, >, 0);
   if (p_space != babl_space("sRGB")) {
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
   g_test_message("%u profiled corpus files", u_seen);
}

int
main(int argc, char **argv) {
   gegl_init(&argc, &argv);
   g_test_init(&argc, &argv, NULL);
   c_dir = g_dir_make_tmp("ggaze-enhicc-XXXXXX", NULL);
   g_assert_nonnull(c_dir);
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
   g_test_add_func("/enhancer_icc/extent_check_catches_what_the_walk_cannot",
                   test_extent_check_catches_what_the_walk_cannot);
   g_test_add_func("/enhancer_icc/corrupt_png_data_is_an_error",
                   test_corrupt_png_data_is_an_error);
   g_test_add_func("/enhancer_icc/export_preserves_profile",
                   test_export_preserves_profile);
   g_test_add_func("/enhancer_icc/missing_saver_is_not_supported",
                   test_missing_saver_is_not_supported);
   g_test_add_func("/enhancer_icc/render_returns_the_managed_original",
                   test_render_returns_the_managed_original);
   g_test_add_func("/enhancer_icc/sample_images_profiled_files",
                   test_sample_images_profiled_files);
   int i_rc = g_test_run();
   g_rmdir(c_dir);
   g_free(c_dir);
   gegl_exit();
   return (i_rc);
}
