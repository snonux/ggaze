/*:*
 * ggaze — animation helpers unit test (no display)
 *
 * The plain-C half of animated GIF/WebP playback (task yb2): the
 * decoder-free frame count and canvas probe over real fixtures, over
 * hand-built containers (a local colour table, an extension cut short, a
 * VP8X flag with no frames, a chunk size past the end) and over EVERY
 * prefix of the animated fixtures (a truncated file must never crash the
 * walk and must count monotonically); the pixel budget at its boundary;
 * the frame-delay clamp; and the texture <-> animation channel, including
 * that the animation dies with its first frame. pixbuf_util's animation
 * decode is covered here too, since this suite is where its callers'
 * expectations are pinned.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "loader/animation.h"

#include <string.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <glib.h>

#include "loader/pixbuf-util.h"
#include "tiny_images.h"

/* The gdk-pixbuf animation API is deprecated since 2.44 (for glycin's);
 * the source files say why they keep using it. Same here. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

/* --- helpers ------------------------------------------------------------- */

static guint8 *
read_fixture(const char *c_name, gsize *p_len) {
   const gchar *c_dir = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_dir);
   char   *c_path = g_build_filename(c_dir, c_name, NULL);
   gchar  *c_buf  = NULL;
   GError *p_err  = NULL;
   g_assert_true(g_file_get_contents(c_path, &c_buf, p_len, &p_err));
   g_assert_no_error(p_err);
   g_free(c_path);
   return ((guint8 *)c_buf);
}

/* A 1x1 opaque black GdkMemoryTexture (no animation attached). */
static GdkTexture *
mk_tex(void) {
   static const guint8 u_px[4] = {0, 0, 0, 255};
   GBytes             *p_b     = g_bytes_new_static(u_px, 4);
   GdkTexture *p_t = gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8, p_b, 4);
   g_bytes_unref(p_b);
   return (p_t);
}

/* Append a GIF image descriptor for a w x h frame at (0,0) with an
 * optional 2-entry local colour table, a 2-byte data sub-block and the
 * terminator. The pixel data is nonsense: the probe never decodes it. */
static void
gif_append_frame(GByteArray *p_gif, gboolean b_local_ct) {
   const guint8 u_desc[10] = {
      0x2C, 0, 0, 0, 0, 8, 0, 6, 0, (guint8)(b_local_ct ? 0x80 : 0x00)};
   const guint8 u_lct[6]  = {1, 2, 3, 4, 5, 6};
   const guint8 u_data[5] = {0x02, 0x02, 0x44, 0x01, 0x00};
   g_byte_array_append(p_gif, u_desc, sizeof(u_desc));
   if (b_local_ct) {
      g_byte_array_append(p_gif, u_lct, sizeof(u_lct));
   }
   g_byte_array_append(p_gif, u_data, sizeof(u_data));
}

/* "GIF89a" + an 8x6 logical screen with no global colour table. */
static GByteArray *
gif_header(void) {
   const guint8 u_hdr[13] = {'G', 'I', 'F', '8',  '9', 'a', 8,
                             0,   6,   0,   0x00, 0,   0};
   GByteArray  *p_gif     = g_byte_array_new();
   g_byte_array_append(p_gif, u_hdr, sizeof(u_hdr));
   return (p_gif);
}

/* A RIFF/WEBP container with a VP8X chunk (u_flags, 8x6 canvas) followed
 * by u_anmf empty ANMF chunks. Chunk sizes are honest. */
static GByteArray *
webp_container(guint8 u_flags, guint u_anmf) {
   const guint8 u_riff[12]  = {'R', 'I', 'F', 'F', 0,   0,
                               0,   0,   'W', 'E', 'B', 'P'};
   const guint8 u_vp8x[18]  = {'V', 'P', '8', 'X', 10, 0, 0, 0, u_flags,
                               0,   0,   0,   7,   0,  0, 5, 0, 0};
   const guint8 u_anmf_c[8] = {'A', 'N', 'M', 'F', 0, 0, 0, 0};
   GByteArray  *p_webp      = g_byte_array_new();
   g_byte_array_append(p_webp, u_riff, sizeof(u_riff));
   g_byte_array_append(p_webp, u_vp8x, sizeof(u_vp8x));
   for (guint u = 0; u < u_anmf; u++) {
      g_byte_array_append(p_webp, u_anmf_c, sizeof(u_anmf_c));
   }
   return (p_webp);
}

/* --- probe: fixtures ----------------------------------------------------- */

static void
assert_probe_fixture(const char *c_name, guint u_frames) {
   gsize          u_len = 0;
   guint8        *p_buf = read_fixture(c_name, &u_len);
   GgazeAnimProbe st_p;
   g_assert_true(animation_probe(p_buf, u_len, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, u_frames);
   g_assert_cmpuint(st_p.u_width, ==, 8);
   g_assert_cmpuint(st_p.u_height, ==, 6);
   g_free(p_buf);
}

static void
test_probe_anim_gif(void) {
   assert_probe_fixture("anim.gif", 4);
}

static void
test_probe_zerodelay_gif(void) {
   assert_probe_fixture("zerodelay.gif", 4);
}

static void
test_probe_anim_webp(void) {
   assert_probe_fixture("anim.webp", 4);
}

/* A single-frame GIF is a still: one frame, its canvas, FALSE. */
static void
test_probe_tiny_gif_is_still(void) {
   GgazeAnimProbe st_p;
   g_assert_false(animation_probe(TINY_GIF, sizeof(TINY_GIF), &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 1);
   g_assert_cmpuint(st_p.u_width, ==, 1);
   g_assert_cmpuint(st_p.u_height, ==, 1);
}

/* A WebP without a VP8X animation flag is a still whatever it holds; the
 * bare VP8/VP8L files carry no canvas the probe reads. */
static void
test_probe_still_webp(void) {
   GgazeAnimProbe st_p;
   g_assert_false(
      animation_probe(TINY_WEBP_LOSSLESS, sizeof(TINY_WEBP_LOSSLESS), &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 1);
   g_assert_false(
      animation_probe(TINY_WEBP_LOSSY, sizeof(TINY_WEBP_LOSSY), &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 1);
}

/* Every other format is "not an animation" with zero frames, not "one
 * frame": the probe only speaks for the two containers it walks. */
static void
test_probe_other_formats(void) {
   GgazeAnimProbe st_p;
   g_assert_false(animation_probe(TINY_PNG, sizeof(TINY_PNG), &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 0);
   gsize   u_len = 0;
   guint8 *p_jpg = read_fixture("plain.jpg", &u_len);
   g_assert_false(animation_probe(p_jpg, u_len, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 0);
   g_free(p_jpg);
}

/* Empty, NULL and a bare signature: FALSE, and the out struct is zeroed
 * rather than left with whatever the caller had in it. */
static void
test_probe_empty_and_short(void) {
   GgazeAnimProbe st_p = {99, 99, 99};
   g_assert_false(animation_probe(NULL, 0, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 0);
   g_assert_cmpuint(st_p.u_width, ==, 0);
   st_p.u_frames = 99;
   g_assert_false(animation_probe(TINY_GIF, 0, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 0);
   g_assert_false(animation_probe(TINY_GIF, 6, &st_p)); /* "GIF89a" only */
   g_assert_cmpuint(st_p.u_frames, ==, 0);
   g_assert_false(animation_probe(TINY_WEBP_LOSSLESS, 12, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 1);
}

/* Truncation: every prefix of both fixtures walks without reading past
 * the end, counts no more frames than the whole, past u_monotonic_from
 * bytes never fewer than the prefix one byte shorter did, and the full
 * file counts all four. */
static void
assert_every_prefix_is_safe(const char *c_name, gsize u_monotonic_from) {
   gsize   u_len  = 0;
   guint8 *p_buf  = read_fixture(c_name, &u_len);
   guint   u_prev = 0;
   /* A copy sized exactly to the prefix, so an over-read is an ASan
    * finding rather than a silent read of the rest of the file. */
   for (gsize u = 0; u <= u_len; u++) {
      guint8        *p_cut = g_memdup2(p_buf, u);
      GgazeAnimProbe st_p;
      animation_probe(p_cut, u, &st_p);
      g_assert_cmpuint(st_p.u_frames, <=, 4);
      if (u > u_monotonic_from) {
         g_assert_cmpuint(st_p.u_frames, >=, u_prev);
      }
      u_prev = st_p.u_frames;
      g_free(p_cut);
   }
   g_assert_cmpuint(u_prev, ==, 4);
   g_free(p_buf);
}

static void
test_probe_every_gif_prefix_is_safe(void) {
   assert_every_prefix_is_safe("anim.gif", 0);
}

/* A WebP reads as "a still" (one frame) until its VP8X chunk -- 12-byte
 * RIFF header + 18-byte chunk -- says animated, and counts ANMF chunks
 * from zero only then; the count is monotonic from that point. */
static void
test_probe_every_webp_prefix_is_safe(void) {
   assert_every_prefix_is_safe("anim.webp", 30);
}

/* --- probe: hand-built containers --------------------------------------- */

/* Two frames, the first with a local colour table that must be skipped:
 * misreading its 6 bytes as blocks would end the walk at frame one. */
static void
test_probe_gif_local_color_table(void) {
   GByteArray *p_gif = gif_header();
   gif_append_frame(p_gif, TRUE);
   gif_append_frame(p_gif, FALSE);
   g_byte_array_append(p_gif, (const guint8 *)"\x3B", 1);
   GgazeAnimProbe st_p;
   g_assert_true(animation_probe(p_gif->data, p_gif->len, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 2);
   /* And with a global colour table too (packed 0x80: 2 entries). */
   p_gif->data[10] = 0x80;
   g_byte_array_prepend(p_gif, (const guint8 *)"", 0);
   const guint8 u_gct[6] = {0, 0, 0, 255, 255, 255};
   g_byte_array_remove_range(p_gif, 13, 0);
   GByteArray *p_with = g_byte_array_new();
   g_byte_array_append(p_with, p_gif->data, 13);
   g_byte_array_append(p_with, u_gct, sizeof(u_gct));
   g_byte_array_append(p_with, p_gif->data + 13, p_gif->len - 13);
   g_assert_true(animation_probe(p_with->data, p_with->len, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 2);
   g_byte_array_unref(p_with);
   g_byte_array_unref(p_gif);
}

/* Garbage where a block introducer should be ends the walk with what was
 * counted; an extension cut before its terminator likewise; a frame whose
 * descriptor is complete but whose data is cut still counts. */
static void
test_probe_gif_garbage_and_cuts(void) {
   GgazeAnimProbe st_p;
   GByteArray    *p_gif = gif_header();
   g_byte_array_append(p_gif, (const guint8 *)"\x99\x2C", 2);
   g_assert_false(animation_probe(p_gif->data, p_gif->len, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 0);
   g_byte_array_unref(p_gif);

   p_gif = gif_header();
   gif_append_frame(p_gif, FALSE);
   const guint8 u_ext[5] = {0x21, 0xF9, 0x04, 0x00, 0x00}; /* no end */
   g_byte_array_append(p_gif, u_ext, sizeof(u_ext));
   g_assert_false(animation_probe(p_gif->data, p_gif->len, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 1);
   g_byte_array_unref(p_gif);

   p_gif = gif_header();
   gif_append_frame(p_gif, FALSE);
   gif_append_frame(p_gif, FALSE);
   g_byte_array_set_size(p_gif, p_gif->len - 3); /* second frame's data */
   g_assert_true(animation_probe(p_gif->data, p_gif->len, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 2);
   g_byte_array_unref(p_gif);
}

/* The VP8X animation flag alone is not an animation: the frames are the
 * ANMF chunks, and one of them is a still. Without the flag, ANMF chunks
 * do not count at all. */
static void
test_probe_webp_flag_and_frames(void) {
   GgazeAnimProbe st_p;
   GByteArray    *p_w = webp_container(0x02, 0);
   g_assert_false(animation_probe(p_w->data, p_w->len, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 0);
   g_assert_cmpuint(st_p.u_width, ==, 8);
   g_assert_cmpuint(st_p.u_height, ==, 6);
   g_byte_array_unref(p_w);

   p_w = webp_container(0x02, 1);
   g_assert_false(animation_probe(p_w->data, p_w->len, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 1);
   g_byte_array_unref(p_w);

   p_w = webp_container(0x02, 3);
   g_assert_true(animation_probe(p_w->data, p_w->len, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 3);
   g_byte_array_unref(p_w);

   p_w = webp_container(0x00, 3); /* no flag: a still */
   g_assert_false(animation_probe(p_w->data, p_w->len, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 1);
   g_byte_array_unref(p_w);
}

/* A chunk size past the end of the file stops the walk (nothing sound
 * follows), and a VP8X cut inside its payload is not read at all. */
static void
test_probe_webp_bad_sizes(void) {
   GgazeAnimProbe st_p;
   GByteArray    *p_w       = webp_container(0x02, 2);
   const guint8   u_huge[8] = {'X', 'Y', 'Z', 'W', 0xFF, 0xFF, 0xFF, 0x7F};
   g_byte_array_append(p_w, u_huge, sizeof(u_huge));
   const guint8 u_anmf[8] = {'A', 'N', 'M', 'F', 0, 0, 0, 0};
   g_byte_array_append(p_w, u_anmf, sizeof(u_anmf)); /* never reached */
   g_assert_true(animation_probe(p_w->data, p_w->len, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 2);
   g_byte_array_unref(p_w);

   p_w = webp_container(0x02, 0);
   g_byte_array_set_size(p_w, 12 + 8 + 4); /* VP8X payload cut */
   g_assert_false(animation_probe(p_w->data, p_w->len, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 1);
   g_assert_cmpuint(st_p.u_width, ==, 0);
   g_byte_array_unref(p_w);
}

/* --- budget, delay ------------------------------------------------------- */

static void
test_within_budget(void) {
   GgazeAnimProbe st_p = {4, 8, 6};
   g_assert_true(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){0, 8, 6};
   g_assert_false(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){2, 0, 6};
   g_assert_false(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){2, 8, 0};
   g_assert_false(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){1, GGAZE_IMAGE_MAX_SIDE + 1, 1};
   g_assert_false(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){1, 1, GGAZE_IMAGE_MAX_SIDE + 1};
   g_assert_false(animation_within_budget(&st_p));
   /* The boundary: frames x canvas == the cap is in, one pixel more out. */
   st_p = (GgazeAnimProbe){100, 10000, 100};
   g_assert_cmpuint((guint64)100 * 10000 * 100, ==, GGAZE_ANIM_MAX_PIXELS);
   g_assert_true(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){100, 10000, 101};
   g_assert_false(animation_within_budget(&st_p));
   /* A huge frame count on a legal canvas: the 64-bit product decides,
    * a 32-bit one would have wrapped to something small. */
   st_p =
      (GgazeAnimProbe){G_MAXUINT, GGAZE_IMAGE_MAX_SIDE, GGAZE_IMAGE_MAX_SIDE};
   g_assert_false(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){65536, 256, 256}; /* 2^32 exactly */
   g_assert_false(animation_within_budget(&st_p));
}

static void
test_frame_delay(void) {
   g_assert_cmpint(animation_frame_delay_ms(-1), ==, -1);
   g_assert_cmpint(animation_frame_delay_ms(-100), ==, -1);
   g_assert_cmpint(animation_frame_delay_ms(0), ==, GGAZE_ANIM_MIN_DELAY_MS);
   g_assert_cmpint(animation_frame_delay_ms(GGAZE_ANIM_MIN_DELAY_MS - 1), ==,
                   GGAZE_ANIM_MIN_DELAY_MS);
   g_assert_cmpint(animation_frame_delay_ms(GGAZE_ANIM_MIN_DELAY_MS), ==,
                   GGAZE_ANIM_MIN_DELAY_MS);
   g_assert_cmpint(animation_frame_delay_ms(100), ==, 100);
}

/* --- decode + attach/lookup --------------------------------------------- */

/* The fixture through pixbuf_util's animation decode: real frames. */
static GdkPixbufAnimation *
decode_fixture_animation(const char *c_name) {
   gsize               u_len = 0;
   guint8             *p_buf = read_fixture(c_name, &u_len);
   GError             *p_err = NULL;
   GdkPixbufAnimation *p_anim =
      pixbuf_util_decode_animation_bytes(p_buf, u_len, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_anim);
   g_free(p_buf);
   return (p_anim);
}

static void
test_decode_animation_bytes(void) {
   GdkPixbufAnimation *p_anim = decode_fixture_animation("anim.gif");
   g_assert_false(gdk_pixbuf_animation_is_static_image(p_anim));
   g_assert_cmpint(gdk_pixbuf_animation_get_width(p_anim), ==, 8);
   g_assert_cmpint(gdk_pixbuf_animation_get_height(p_anim), ==, 6);
   g_object_unref(p_anim);
   /* A still decodes to a static animation, not to an error. */
   GError *p_err = NULL;
   p_anim =
      pixbuf_util_decode_animation_bytes(TINY_PNG, sizeof(TINY_PNG), &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_anim);
   g_assert_true(gdk_pixbuf_animation_is_static_image(p_anim));
   g_object_unref(p_anim);
   /* Garbage is an error, never a NULL without one. */
   const guint8 u_junk[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
   p_anim = pixbuf_util_decode_animation_bytes(u_junk, sizeof(u_junk), &p_err);
   g_assert_null(p_anim);
   g_assert_nonnull(p_err);
   g_clear_error(&p_err);
}

/* No animation on a plain texture; attach makes lookup return it; attach
 * NULL detaches; and the texture's finalize drops the animation's ref. */
static void
test_attach_and_lookup(void) {
   GdkTexture *p_tex = mk_tex();
   g_assert_null(animation_lookup(p_tex));
   g_assert_null(animation_lookup(NULL));

   GdkPixbufAnimation *p_anim = decode_fixture_animation("anim.gif");
   animation_attach(p_tex, p_anim);
   g_assert_true(animation_lookup(p_tex) == p_anim);
   animation_attach(p_tex, NULL);
   g_assert_null(animation_lookup(p_tex));

   animation_attach(p_tex, p_anim);
   gpointer p_weak = p_anim;
   g_object_add_weak_pointer(G_OBJECT(p_anim), &p_weak);
   g_object_unref(p_anim); /* the texture holds the last ref now */
   g_assert_nonnull(p_weak);
   g_object_unref(p_tex);
   g_assert_null(p_weak); /* gone with its first frame */
}

G_GNUC_END_IGNORE_DEPRECATIONS

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);

   g_test_add_func("/animation/probe/anim_gif", test_probe_anim_gif);
   g_test_add_func("/animation/probe/zerodelay_gif", test_probe_zerodelay_gif);
   g_test_add_func("/animation/probe/anim_webp", test_probe_anim_webp);
   g_test_add_func("/animation/probe/tiny_gif_is_still",
                   test_probe_tiny_gif_is_still);
   g_test_add_func("/animation/probe/still_webp", test_probe_still_webp);
   g_test_add_func("/animation/probe/other_formats", test_probe_other_formats);
   g_test_add_func("/animation/probe/empty_and_short",
                   test_probe_empty_and_short);
   g_test_add_func("/animation/probe/every_gif_prefix_is_safe",
                   test_probe_every_gif_prefix_is_safe);
   g_test_add_func("/animation/probe/every_webp_prefix_is_safe",
                   test_probe_every_webp_prefix_is_safe);
   g_test_add_func("/animation/probe/gif_local_color_table",
                   test_probe_gif_local_color_table);
   g_test_add_func("/animation/probe/gif_garbage_and_cuts",
                   test_probe_gif_garbage_and_cuts);
   g_test_add_func("/animation/probe/webp_flag_and_frames",
                   test_probe_webp_flag_and_frames);
   g_test_add_func("/animation/probe/webp_bad_sizes",
                   test_probe_webp_bad_sizes);
   g_test_add_func("/animation/within_budget", test_within_budget);
   g_test_add_func("/animation/frame_delay", test_frame_delay);
   g_test_add_func("/animation/decode_animation_bytes",
                   test_decode_animation_bytes);
   g_test_add_func("/animation/attach_and_lookup", test_attach_and_lookup);
   return (g_test_run());
}
