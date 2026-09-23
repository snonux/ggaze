/*:*
 * ggaze — animation helpers unit test (no display)
 *
 * The plain-C half of animated GIF/WebP playback (task yb2): the
 * decoder-free frame count and canvas probe over real fixtures, over
 * hand-built containers (a local colour table, an extension cut short, a
 * VP8X flag with no frames, a chunk size past the end) and over EVERY
 * prefix of the animated fixtures (a truncated file must never crash the
 * walk and must count monotonically); the loop count (NETSCAPE2.0 /
 * ANIMEXTS1.0 in a GIF, the ANIM chunk in a WebP, the play-once default,
 * cut and foreign extensions); the playback budget at each of its
 * boundaries (pixels, frame count, canvas) and a real file one frame over
 * the frame cap; the frame-delay clamp; the frame store and the texture
 * <-> animation channel, including that the frames die with their first
 * frame. pixbuf_util's animation decode and its frame walk
 * (pixbuf_util_animation_to_texture: every frame its own texture, the
 * first one intact after the rest were taken, the count, a cancel, a
 * static decode, 0 ms delays arriving as 100 ms, 10 ms ones clamped to
 * 20, the play count carried, a sub-2 ms first frame) are covered here
 * too, since this suite is where their callers' expectations are pinned.
 * The playback schedule has a suite of its own
 * (test_animation_playback.c).
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
   GgazeAnimProbe st_p = {99, 99, 99, 0};
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
   /* And with a global colour table too (packed 0x80: 2 entries), spliced
    * in after the 13-byte header. */
   p_gif->data[10]       = 0x80;
   const guint8 u_gct[6] = {0, 0, 0, 255, 255, 255};
   GByteArray  *p_with   = g_byte_array_new();
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

/* --- probe: loop count --------------------------------------------------- */

/* The play count of a fixture: anim.gif / slow.gif carry NETSCAPE2.0 with
 * a count of 0 (for ever), once.gif none at all (plays once, the GIF89a
 * default), anim.webp an ANIM count of 0, once.webp of 1. */
static void
assert_fixture_plays(const char *c_name, guint u_plays) {
   gsize          u_len = 0;
   guint8        *p_buf = read_fixture(c_name, &u_len);
   GgazeAnimProbe st_p;
   g_assert_true(animation_probe(p_buf, u_len, &st_p));
   g_assert_cmpuint(st_p.u_plays, ==, u_plays);
   g_free(p_buf);
}

static void
test_probe_fixture_plays(void) {
   assert_fixture_plays("anim.gif", 0);
   assert_fixture_plays("slow.gif", 0);
   assert_fixture_plays("once.gif", 1);
   assert_fixture_plays("anim.webp", 0);
   assert_fixture_plays("once.webp", 1);
}

/* An application extension: 0x21 0xFF, the 11-byte identifier c_id, a
 * 3-byte sub-block 0x01 + little-endian u_count, the terminator. */
static void
gif_append_app_ext(GByteArray *p_gif, const char *c_id, guint u_count) {
   const guint8 u_head[3] = {0x21, 0xFF, 11};
   const guint8 u_tail[5] = {3, 0x01, (guint8)(u_count & 0xFF),
                             (guint8)(u_count >> 8), 0};
   g_byte_array_append(p_gif, u_head, sizeof(u_head));
   g_byte_array_append(p_gif, (const guint8 *)c_id, 11);
   g_byte_array_append(p_gif, u_tail, sizeof(u_tail));
}

/* Two frames after whatever extensions the caller added. */
static guint
gif_plays(GByteArray *p_gif) {
   gif_append_frame(p_gif, FALSE);
   gif_append_frame(p_gif, FALSE);
   GgazeAnimProbe st_p;
   g_assert_true(animation_probe(p_gif->data, p_gif->len, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, 2);
   g_byte_array_unref(p_gif);
   return (st_p.u_plays);
}

/* A NETSCAPE2.0 (or ANIMEXTS1.0) count of N is N repetitions after the
 * first play, so N + 1 plays; 0 is for ever. Only the first loop
 * extension counts; any other application extension (XMP here) is
 * skipped, and a loop extension cut short is not read (plays once). */
static void
test_probe_gif_loop_count(void) {
   GByteArray *p_gif = gif_header();
   gif_append_app_ext(p_gif, "NETSCAPE2.0", 2);
   g_assert_cmpuint(gif_plays(p_gif), ==, 3);
   p_gif = gif_header();
   gif_append_app_ext(p_gif, "ANIMEXTS1.0", 0);
   g_assert_cmpuint(gif_plays(p_gif), ==, 0);
   p_gif = gif_header();
   gif_append_app_ext(p_gif, "NETSCAPE2.0", 65535);
   g_assert_cmpuint(gif_plays(p_gif), ==, 65536);
   p_gif = gif_header();
   gif_append_app_ext(p_gif, "NETSCAPE2.0", 1);
   gif_append_app_ext(p_gif, "NETSCAPE2.0", 0); /* ignored */
   g_assert_cmpuint(gif_plays(p_gif), ==, 2);
   p_gif = gif_header();
   gif_append_app_ext(p_gif, "XMP DataXMP", 0);
   g_assert_cmpuint(gif_plays(p_gif), ==, 1);

   p_gif = gif_header();
   gif_append_app_ext(p_gif, "NETSCAPE2.0", 0);
   g_byte_array_set_size(p_gif, p_gif->len - 3); /* cut in the count */
   GgazeAnimProbe st_p;
   g_assert_false(animation_probe(p_gif->data, p_gif->len, &st_p));
   g_assert_cmpuint(st_p.u_plays, ==, 1);
   g_byte_array_unref(p_gif);
}

/* The WebP ANIM chunk's count is plays in all as is (0 for ever); an ANIM
 * chunk cut inside its payload ends the walk unread. */
static void
test_probe_webp_loop_count(void) {
   const guint8 u_anim[14] = {'A', 'N', 'I', 'M', 6, 0, 0, 0, 0, 0, 0, 0, 3, 0};
   GByteArray  *p_w        = webp_container(0x02, 0);
   g_byte_array_append(p_w, u_anim, sizeof(u_anim));
   const guint8 u_anmf[16] = {'A', 'N', 'M', 'F', 0, 0, 0, 0,
                              'A', 'N', 'M', 'F', 0, 0, 0, 0};
   g_byte_array_append(p_w, u_anmf, sizeof(u_anmf));
   GgazeAnimProbe st_p;
   g_assert_true(animation_probe(p_w->data, p_w->len, &st_p));
   g_assert_cmpuint(st_p.u_plays, ==, 3);
   g_assert_cmpuint(st_p.u_frames, ==, 2);
   g_byte_array_unref(p_w);

   p_w = webp_container(0x02, 0);
   g_byte_array_append(p_w, u_anim, 12); /* 2 bytes short */
   g_assert_false(animation_probe(p_w->data, p_w->len, &st_p));
   g_assert_cmpuint(st_p.u_plays, ==, 0);
   g_byte_array_unref(p_w);
}

/* One frame over GGAZE_ANIM_MAX_FRAMES, probed in full and refused by the
 * budget: the file the loader shows as a still (test_loader_pixbuf). */
static void
test_probe_manyframes_over_budget(void) {
   gsize          u_len = 0;
   guint8        *p_buf = read_fixture("manyframes.gif", &u_len);
   GgazeAnimProbe st_p;
   g_assert_true(animation_probe(p_buf, u_len, &st_p));
   g_assert_cmpuint(st_p.u_frames, ==, GGAZE_ANIM_MAX_FRAMES + 1);
   g_assert_false(animation_within_budget(&st_p));
   g_free(p_buf);
}

/* --- budget, delay ------------------------------------------------------- */

static void
test_within_budget(void) {
   GgazeAnimProbe st_p = {4, 8, 6, 0};
   g_assert_true(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){0, 8, 6, 0};
   g_assert_false(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){2, 0, 6, 0};
   g_assert_false(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){2, 8, 0, 0};
   g_assert_false(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){1, GGAZE_IMAGE_MAX_SIDE + 1, 1, 0};
   g_assert_false(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){1, 1, GGAZE_IMAGE_MAX_SIDE + 1, 0};
   g_assert_false(animation_within_budget(&st_p));
   /* The boundary: frames x canvas == the cap is in, one pixel more out
    * (8 frames of 2048 x 2048 is 32 Mi pixels exactly). */
   g_assert_cmpuint((guint64)8 * 2048 * 2048, ==, GGAZE_ANIM_MAX_PIXELS);
   st_p = (GgazeAnimProbe){8, 2048, 2048, 0};
   g_assert_true(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){9, 2048, 2048, 0};
   g_assert_false(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){32, 1024, 1024, 0};
   g_assert_true(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){32, 1024, 1025, 0};
   g_assert_false(animation_within_budget(&st_p));
   /* A real-world clip: 640 x 480 plays up to 109 frames. */
   st_p = (GgazeAnimProbe){109, 640, 480, 0};
   g_assert_true(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){110, 640, 480, 0};
   g_assert_false(animation_within_budget(&st_p));
   /* A huge frame count on a legal canvas: refused by the frame cap and,
    * independently, by the 64-bit product, which a 32-bit one would have
    * wrapped to something small. */
   st_p = (GgazeAnimProbe){G_MAXUINT, GGAZE_IMAGE_MAX_SIDE,
                           GGAZE_IMAGE_MAX_SIDE, 0};
   g_assert_false(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){65536, 256, 256, 0}; /* 2^32 exactly */
   g_assert_false(animation_within_budget(&st_p));
}

/* The frame cap on its own: a 1x1 canvas is nothing for the pixel budget,
 * so only GGAZE_ANIM_MAX_FRAMES stands between a crafted 200000-frame GIF
 * and 200000 texture objects (review finding 5 of yb2). */
static void
test_within_budget_frame_cap(void) {
   GgazeAnimProbe st_p = {GGAZE_ANIM_MAX_FRAMES, 1, 1, 0};
   g_assert_true(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){GGAZE_ANIM_MAX_FRAMES + 1, 1, 1, 0};
   g_assert_false(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){200000, 1, 1, 0};
   g_assert_false(animation_within_budget(&st_p));
   /* At the cap the pixel budget still applies: 1000 frames of 33554
    * pixels (16777 x 2) are under 32 Mi pixels, of 33555 (11185 x 3)
    * over -- both canvases well inside the side and canvas caps. */
   g_assert_cmpuint((guint64)GGAZE_ANIM_MAX_FRAMES * 33554, <=,
                    GGAZE_ANIM_MAX_PIXELS);
   g_assert_cmpuint((guint64)GGAZE_ANIM_MAX_FRAMES * 33555, >,
                    GGAZE_ANIM_MAX_PIXELS);
   st_p = (GgazeAnimProbe){GGAZE_ANIM_MAX_FRAMES, 16777, 2, 0};
   g_assert_true(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){GGAZE_ANIM_MAX_FRAMES, 11185, 3, 0};
   g_assert_false(animation_within_budget(&st_p));
}

/* The canvas cap on its own: two frames of 2048 x 2048 (4 Mi pixels, the
 * cap) play, one more row does not, whatever shape reaches the same
 * area plays too, and a canvas over the cap is refused with far fewer
 * frames than the pixel budget would allow. */
static void
test_within_budget_canvas_cap(void) {
   GgazeAnimProbe st_p = {2, 2048, 2048, 0};
   g_assert_cmpuint((guint64)2048 * 2048, ==, GGAZE_ANIM_MAX_CANVAS_PIXELS);
   g_assert_true(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){2, 2048, 2049, 0};
   g_assert_false(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){2, 4096, 1024, 0};
   g_assert_true(animation_within_budget(&st_p));
   st_p = (GgazeAnimProbe){2, 3000, 3000, 0}; /* 18 M of 32 Mi: pixels OK */
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

/* A 1x1 opaque texture of green u_g, standing in for a decoded frame. */
static GdkTexture *
mk_frame(guint8 u_g) {
   guint8 *p_px    = g_malloc(4);
   p_px[0]         = 0;
   p_px[1]         = u_g;
   p_px[2]         = 0;
   p_px[3]         = 255;
   GBytes     *p_b = g_bytes_new_take(p_px, 4);
   GdkTexture *p_t = gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8, p_b, 4);
   g_bytes_unref(p_b);
   return (p_t);
}

/* The frame store: frame 0 is not stored (it is the carrier texture), the
 * delays come back clamped, -1 stays "hold", and every index out of range
 * is NULL / -1 rather than a read past the arrays. */
static void
test_frames_store(void) {
   GgazeAnimation *p_anim = animation_new(0);
   g_assert_cmpuint(animation_get_n_frames(p_anim), ==, 1);
   g_assert_null(animation_get_frame(p_anim, 0));
   g_assert_null(animation_get_frame(p_anim, 1));
   g_assert_cmpint(animation_get_delay_ms(p_anim, 0), ==,
                   GGAZE_ANIM_MIN_DELAY_MS);
   GdkTexture *p_f1 = mk_frame(1);
   GdkTexture *p_f2 = mk_frame(2);
   animation_append_frame(p_anim, p_f1, 100);
   animation_append_frame(p_anim, p_f2, -1);
   g_assert_cmpuint(animation_get_n_frames(p_anim), ==, 3);
   g_assert_true(animation_get_frame(p_anim, 1) == p_f1);
   g_assert_true(animation_get_frame(p_anim, 2) == p_f2);
   g_assert_null(animation_get_frame(p_anim, 3));
   g_assert_cmpint(animation_get_delay_ms(p_anim, 1), ==, 100);
   g_assert_cmpint(animation_get_delay_ms(p_anim, 2), ==, -1);
   g_assert_cmpint(animation_get_delay_ms(p_anim, 3), ==, -1);
   g_object_unref(p_f1);
   g_object_unref(p_f2);
   animation_delete(p_anim);
   animation_delete(NULL); /* like every _delete here */
}

/* No animation on a plain texture; attach makes lookup return it; attach
 * NULL detaches (and deletes it); and the texture's finalize deletes the
 * animation, releasing the frames it held. */
static void
test_attach_and_lookup(void) {
   GdkTexture *p_tex = mk_tex();
   g_assert_null(animation_lookup(p_tex));
   g_assert_null(animation_lookup(NULL));

   GgazeAnimation *p_anim = animation_new(100);
   animation_attach(p_tex, p_anim);
   g_assert_true(animation_lookup(p_tex) == p_anim);
   animation_attach(p_tex, NULL);
   g_assert_null(animation_lookup(p_tex));

   p_anim           = animation_new(100);
   GdkTexture *p_f1 = mk_frame(1);
   animation_append_frame(p_anim, p_f1, 100);
   gpointer p_weak = p_f1;
   g_object_add_weak_pointer(G_OBJECT(p_f1), &p_weak);
   g_object_unref(p_f1); /* the animation holds the last ref now */
   animation_attach(p_tex, p_anim);
   g_assert_nonnull(p_weak);
   g_object_unref(p_tex);
   g_assert_null(p_weak); /* gone with its first frame */
}

/* --- pixbuf_util_animation_to_texture ------------------------------------ */

/* The frame walk over p_pa for a probe of u_frames frames that loops for
 * ever (the 8x6 canvas of the fixtures; the walk reads only the counts). */
static GdkTexture *
to_texture(GdkPixbufAnimation *p_pa, guint u_frames, GCancellable *p_cancel,
           GError **p_err) {
   GgazeAnimProbe st_p = {u_frames, 8, 6, 0};
   return (pixbuf_util_animation_to_texture(p_pa, &st_p, p_cancel, p_err));
}

/* The green channel of p_tex's top-left pixel (gdk_texture_download writes
 * premultiplied BGRA; the fixtures are opaque, so byte 1 is green). */
static guint8
green_of(GdkTexture *p_tex) {
   gsize   u_stride = 4u * (gsize)gdk_texture_get_width(p_tex);
   guint8 *p_px = g_malloc0(u_stride * (gsize)gdk_texture_get_height(p_tex));
   gdk_texture_download(p_tex, p_px, u_stride);
   guint8 u_g = p_px[1];
   g_free(p_px);
   return (u_g);
}

/* anim.gif's four frames, in order, each its own texture: the greens of
 * tests/fixtures/gen.py ANIM_FRAME_RGB, every delay the file's 100 ms,
 * and the first frame -- the texture returned -- still green AFTER every
 * other frame was taken (a texture sharing the decoder's buffer would now
 * show the last one: review finding 1 of yb2). */
static void
test_to_texture_takes_every_frame(void) {
   static const guint8 u_green[4] = {255, 195, 135, 75};
   GdkPixbufAnimation *p_pa       = decode_fixture_animation("anim.gif");
   GError             *p_err      = NULL;
   GdkTexture         *p_tex      = to_texture(p_pa, 4, NULL, &p_err);
   g_assert_no_error(p_err);
   g_object_unref(p_pa);
   const GgazeAnimation *p_anim = animation_lookup(p_tex);
   g_assert_nonnull(p_anim);
   g_assert_cmpuint(animation_get_n_frames(p_anim), ==, 4);
   g_assert_cmpuint(green_of(p_tex), ==, u_green[0]);
   for (guint u = 1; u < 4; u++) {
      GdkTexture *p_f = animation_get_frame(p_anim, u);
      g_assert_cmpint(gdk_texture_get_width(p_f), ==, 8);
      g_assert_cmpuint(ABS((gint)green_of(p_f) - (gint)u_green[u]), <=, 2);
   }
   for (guint u = 0; u < 4; u++) {
      g_assert_cmpint(animation_get_delay_ms(p_anim, u), ==, 100);
   }
   g_assert_cmpuint(green_of(p_tex), ==, u_green[0]);
   g_object_unref(p_tex);
}

/* Fewer frames asked than the file has: that many are taken. One frame is
 * a still (nothing attached): there is nothing to play. */
static void
test_to_texture_frame_count(void) {
   GdkPixbufAnimation *p_pa  = decode_fixture_animation("anim.gif");
   GdkTexture         *p_tex = to_texture(p_pa, 2, NULL, NULL);
   g_assert_cmpuint(animation_get_n_frames(animation_lookup(p_tex)), ==, 2);
   g_object_unref(p_tex);
   p_tex = to_texture(p_pa, 1, NULL, NULL);
   g_assert_nonnull(p_tex);
   g_assert_null(animation_lookup(p_tex));
   g_assert_cmpuint(green_of(p_tex), ==, 255);
   g_object_unref(p_tex);
   g_object_unref(p_pa);
}

/* A cancel that is already set stops the walk at the first frame boundary
 * with CANCELLED and no texture (a superseded load stops early). */
static void
test_to_texture_cancelled(void) {
   GdkPixbufAnimation *p_pa     = decode_fixture_animation("anim.gif");
   GCancellable       *p_cancel = g_cancellable_new();
   g_cancellable_cancel(p_cancel);
   GError     *p_err = NULL;
   GdkTexture *p_tex = to_texture(p_pa, 4, p_cancel, &p_err);
   g_assert_null(p_tex);
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
   g_clear_error(&p_err);
   g_object_unref(p_cancel);
   g_object_unref(p_pa);
}

/* A static "animation" (a still through the animation decode) is that
 * still with nothing attached. A GIF whose delays are all 0 ms plays all
 * four frames at 100 ms: both decoders (gdk-pixbuf 2.42's io-gif.c and
 * the glycin bridge of 2.44, measured) turn a 0 ms GIF delay into 100 ms
 * before ggaze sees it, so it never reaches ggaze's own clamp (which
 * /animation/to_texture_fast_delay_clamped covers). */
static void
test_to_texture_static_and_zero_delay(void) {
   GError             *p_err = NULL;
   GdkPixbufAnimation *p_pa =
      pixbuf_util_decode_animation_bytes(TINY_PNG, sizeof(TINY_PNG), &p_err);
   g_assert_no_error(p_err);
   GdkTexture *p_tex = to_texture(p_pa, 4, NULL, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_tex);
   g_assert_null(animation_lookup(p_tex));
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 1);
   g_object_unref(p_tex);
   g_object_unref(p_pa);

   p_pa  = decode_fixture_animation("zerodelay.gif");
   p_tex = to_texture(p_pa, 4, NULL, &p_err);
   g_assert_no_error(p_err);
   const GgazeAnimation *p_anim = animation_lookup(p_tex);
   g_assert_nonnull(p_anim);
   g_assert_cmpuint(animation_get_n_frames(p_anim), ==, 4);
   for (guint u = 0; u < 4; u++) {
      g_assert_cmpint(animation_get_delay_ms(p_anim, u), ==, 100);
   }
   g_object_unref(p_tex);
   g_object_unref(p_pa);
}

/* The clamp on a delay that really reaches it: fastdelay.gif's frames
 * are 10 ms. Glycin hands 10 over as is (measured on Fedora 44), so the
 * 20 ms stored is ggaze's own clamp at work; gdk-pixbuf 2.42's GIF loader
 * raises it to 20 itself, so there the result is the same. The raw
 * figure is logged so a run shows which of the two happened. */
static void
test_to_texture_fast_delay_clamped(void) {
   GdkPixbufAnimation     *p_pa = decode_fixture_animation("fastdelay.gif");
   GTimeVal                st_t = {1000, 0};
   GdkPixbufAnimationIter *p_it = gdk_pixbuf_animation_get_iter(p_pa, &st_t);
   gint i_raw = gdk_pixbuf_animation_iter_get_delay_time(p_it);
   g_object_unref(p_it);
   g_test_message("decoder reports %d ms for a 10 ms GIF delay", i_raw);
   g_assert_cmpint(i_raw, <=, GGAZE_ANIM_MIN_DELAY_MS);
   GError     *p_err = NULL;
   GdkTexture *p_tex = to_texture(p_pa, 4, NULL, &p_err);
   g_assert_no_error(p_err);
   const GgazeAnimation *p_anim = animation_lookup(p_tex);
   g_assert_nonnull(p_anim);
   g_assert_cmpuint(animation_get_n_frames(p_anim), ==, 4);
   for (guint u = 0; u < 4; u++) {
      g_assert_cmpint(animation_get_delay_ms(p_anim, u), ==,
                      GGAZE_ANIM_MIN_DELAY_MS);
   }
   g_object_unref(p_tex);
   g_object_unref(p_pa);
}

/* The probe's play count rides on the animation the walk attaches: the
 * iterators cannot be asked (glycin's loops for ever whatever the file
 * says), so the container's figure is what the viewer obeys. */
static void
test_to_texture_carries_plays(void) {
   GdkPixbufAnimation *p_pa = decode_fixture_animation("once.gif");
   GgazeAnimProbe      st_p = {4, 8, 6, 1};
   GdkTexture         *p_tex =
      pixbuf_util_animation_to_texture(p_pa, &st_p, NULL, NULL);
   g_assert_cmpuint(animation_get_plays(animation_lookup(p_tex)), ==, 1);
   g_object_unref(p_tex);
   st_p.u_plays = 0;
   p_tex        = pixbuf_util_animation_to_texture(p_pa, &st_p, NULL, NULL);
   g_assert_cmpuint(animation_get_plays(animation_lookup(p_tex)), ==, 0);
   g_object_unref(p_tex);
   g_object_unref(p_pa);
}

/* A GdkPixbufSimpleAnim of four solid frames at f_fps, not looping; frame
 * u is green 255 - 60 u (gdk_pixbuf_fill takes 0xRRGGBBAA), as in the
 * fixtures. */
static GdkPixbufAnimation *
simple_anim(gfloat f_fps) {
   GdkPixbufSimpleAnim *p_sa = gdk_pixbuf_simple_anim_new(8, 6, f_fps);
   for (guint u = 0; u < 4; u++) {
      GdkPixbuf *p_pix = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 8, 6);
      gdk_pixbuf_fill(p_pix, ((guint32)(255 - 60 * u) << 16) | 0xFF);
      gdk_pixbuf_simple_anim_add_frame(p_sa, p_pix);
      g_object_unref(p_pix);
   }
   return (GDK_PIXBUF_ANIMATION(p_sa));
}

/* A first frame under 2 ms (_iter_counts_down's i_first < 2: 1 ms at
 * 1000 fps) cannot tell a counting-down iterator from a whole-delay one
 * and is taken as whole-delay without probing the iterator. What is
 * pinned is what that promises: the first frame is frame 0, intact, an
 * animation is attached, and every delay is either the clamp or -1 (the
 * non-looping SimpleAnim ends on its last frame) -- never 1 ms, never a
 * count-down figure. Which of the 1 ms frames are sampled is the known
 * limit documented at _append_frames (pixbuf-util.c). */
static void
test_to_texture_sub_2ms_first_frame(void) {
   GdkPixbufAnimation *p_pa  = simple_anim(1000.0f);
   GError             *p_err = NULL;
   GdkTexture         *p_tex = to_texture(p_pa, 4, NULL, &p_err);
   g_assert_no_error(p_err);
   g_assert_cmpuint(green_of(p_tex), ==, 255); /* frame 0 */
   const GgazeAnimation *p_anim = animation_lookup(p_tex);
   g_assert_nonnull(p_anim);
   guint u_n = animation_get_n_frames(p_anim);
   g_assert_cmpuint(u_n, >=, 2);
   for (guint u = 0; u < u_n; u++) {
      gint i_d = animation_get_delay_ms(p_anim, u);
      g_assert_true(i_d == GGAZE_ANIM_MIN_DELAY_MS || i_d == -1);
   }
   g_object_unref(p_tex);
   g_object_unref(p_pa);
}

G_GNUC_END_IGNORE_DEPRECATIONS

/* The probe subtests, registered apart so main() stays short
 * (c-best-practices). */
static void
_add_probe_tests(void) {
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
   g_test_add_func("/animation/probe/fixture_plays", test_probe_fixture_plays);
   g_test_add_func("/animation/probe/gif_loop_count",
                   test_probe_gif_loop_count);
   g_test_add_func("/animation/probe/webp_loop_count",
                   test_probe_webp_loop_count);
   g_test_add_func("/animation/probe/manyframes_over_budget",
                   test_probe_manyframes_over_budget);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);

   _add_probe_tests();
   g_test_add_func("/animation/within_budget", test_within_budget);
   g_test_add_func("/animation/within_budget_frame_cap",
                   test_within_budget_frame_cap);
   g_test_add_func("/animation/within_budget_canvas_cap",
                   test_within_budget_canvas_cap);
   g_test_add_func("/animation/frame_delay", test_frame_delay);
   g_test_add_func("/animation/decode_animation_bytes",
                   test_decode_animation_bytes);
   g_test_add_func("/animation/frames_store", test_frames_store);
   g_test_add_func("/animation/attach_and_lookup", test_attach_and_lookup);
   g_test_add_func("/animation/to_texture_takes_every_frame",
                   test_to_texture_takes_every_frame);
   g_test_add_func("/animation/to_texture_frame_count",
                   test_to_texture_frame_count);
   g_test_add_func("/animation/to_texture_cancelled",
                   test_to_texture_cancelled);
   g_test_add_func("/animation/to_texture_static_and_zero_delay",
                   test_to_texture_static_and_zero_delay);
   g_test_add_func("/animation/to_texture_fast_delay_clamped",
                   test_to_texture_fast_delay_clamped);
   g_test_add_func("/animation/to_texture_carries_plays",
                   test_to_texture_carries_plays);
   g_test_add_func("/animation/to_texture_sub_2ms_first_frame",
                   test_to_texture_sub_2ms_first_frame);
   return (g_test_run());
}
