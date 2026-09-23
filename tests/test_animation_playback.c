/*:*
 * ggaze — animation playback schedule unit test (no display)
 *
 * animation_playback_advance() (loader/animation.h) is the whole of the
 * viewer's playback rule set as plain C over a caller-supplied clock, so
 * it is driven here with a fake one, in microseconds: the first call only
 * anchors the schedule; a frame changes exactly when it is due and one
 * frame per call; the due time moves by each frame's own (clamped) delay,
 * which keeps the file's average rate although calls land late; after a
 * stall it restarts from the call's time instead of racing through the
 * frames it missed (review finding 5 of yb2); a frame of delay -1 ends
 * the playback on it; and the file's play count (finding 1: a GIF
 * without a NETSCAPE2.0 loop extension plays ONCE) holds the last frame
 * after that many plays, 0 looping for ever. A reset starts over.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "loader/animation.h"

#include <gdk/gdk.h>
#include <glib.h>

/* The fake clock's origin: any non-zero time (0 is "not anchored"). */
#define T0_US ((gint64)5000000)

/* A 1x1 opaque texture standing in for a decoded frame. */
static GdkTexture *
mk_frame(void) {
   static const guint8 u_px[4] = {0, 0, 0, 255};
   GBytes             *p_b     = g_bytes_new_static(u_px, 4);
   GdkTexture *p_t = gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8, p_b, 4);
   g_bytes_unref(p_b);
   return (p_t);
}

/* An animation of u_n frames whose delays are i_delays[0..u_n-1] (frame 0
 * is the carrier, so only the other frames are stored), playing u_plays
 * times. */
static GgazeAnimation *
mk_anim(const gint *i_delays, guint u_n, guint u_plays) {
   GgazeAnimation *p_anim = animation_new(i_delays[0]);
   for (guint u = 1; u < u_n; u++) {
      GdkTexture *p_f = mk_frame();
      animation_append_frame(p_anim, p_f, i_delays[u]);
      g_object_unref(p_f);
   }
   animation_set_plays(p_anim, u_plays);
   return (p_anim);
}

/* Advance at T0_US + i_ms and say whether the frame changed. */
static gboolean
at_ms(const GgazeAnimation *p_anim, GgazeAnimPlayback *p_pb, gint64 i_ms) {
   return (animation_playback_advance(p_anim, p_pb, T0_US + i_ms * 1000));
}

/* The first call anchors: nothing changes, frame 0 is due to be replaced
 * one delay later. Before that nothing moves; at it frame 1 shows. */
static void
test_anchor_and_due(void) {
   static const gint i_d[3] = {100, 50, 30};
   GgazeAnimation   *p_anim = mk_anim(i_d, 3, 0);
   GgazeAnimPlayback st_pb;
   animation_playback_reset(&st_pb);
   g_assert_false(at_ms(p_anim, &st_pb, 0));
   g_assert_cmpuint(st_pb.u_frame, ==, 0);
   g_assert_cmpint(st_pb.i_due_us, ==, T0_US + 100000);
   g_assert_false(at_ms(p_anim, &st_pb, 99));
   g_assert_cmpuint(st_pb.u_frame, ==, 0);
   g_assert_true(at_ms(p_anim, &st_pb, 100));
   g_assert_cmpuint(st_pb.u_frame, ==, 1);
   g_assert_false(at_ms(p_anim, &st_pb, 149));
   g_assert_true(at_ms(p_anim, &st_pb, 150));
   g_assert_cmpuint(st_pb.u_frame, ==, 2);
   animation_delete(p_anim);
}

/* Calls that land late (a tick is 16.7 ms) keep the file's average rate:
 * the next due is the previous due plus the delay, not the call's time
 * plus the delay. After the last frame a looping animation (plays 0)
 * starts over at frame 0, counting the plays. */
static void
test_average_rate_and_loop(void) {
   static const gint i_d[2] = {100, 100};
   GgazeAnimation   *p_anim = mk_anim(i_d, 2, 0);
   GgazeAnimPlayback st_pb;
   animation_playback_reset(&st_pb);
   at_ms(p_anim, &st_pb, 0);
   g_assert_true(at_ms(p_anim, &st_pb, 110)); /* 10 ms late */
   g_assert_cmpint(st_pb.i_due_us, ==, T0_US + 200000);
   g_assert_true(at_ms(p_anim, &st_pb, 205)); /* wraps */
   g_assert_cmpuint(st_pb.u_frame, ==, 0);
   g_assert_cmpuint(st_pb.u_played, ==, 1);
   for (guint u = 3; u < 50; u++) {
      g_assert_true(at_ms(p_anim, &st_pb, (gint64)u * 100 + 5));
   }
   g_assert_false(st_pb.b_ended);
   g_assert_cmpuint(st_pb.u_played, ==, 24);
   animation_delete(p_anim);
}

/* A stall (a busy main loop, a frame clock that paused) of several
 * seconds shows the NEXT frame -- one per call, not the frames missed --
 * and schedules the one after a full delay from the call. */
static void
test_stall_resyncs(void) {
   static const gint i_d[4] = {100, 100, 100, 100};
   GgazeAnimation   *p_anim = mk_anim(i_d, 4, 0);
   GgazeAnimPlayback st_pb;
   animation_playback_reset(&st_pb);
   at_ms(p_anim, &st_pb, 0);
   g_assert_true(at_ms(p_anim, &st_pb, 5000));
   g_assert_cmpuint(st_pb.u_frame, ==, 1);
   g_assert_cmpint(st_pb.i_due_us, ==, T0_US + 5100000);
   g_assert_false(at_ms(p_anim, &st_pb, 5099));
   g_assert_true(at_ms(p_anim, &st_pb, 5100));
   g_assert_cmpuint(st_pb.u_frame, ==, 2);
   animation_delete(p_anim);
}

/* Plays 1 (a GIF without a loop extension, a WebP whose ANIM count is 1):
 * the last frame shows for good after the first play; no later call
 * changes anything. Plays 2: one more round, then the same. */
static void
test_play_count_holds_last_frame(void) {
   static const gint i_d[3] = {100, 100, 100};
   for (guint u_plays = 1; u_plays <= 2; u_plays++) {
      GgazeAnimation   *p_anim = mk_anim(i_d, 3, u_plays);
      GgazeAnimPlayback st_pb;
      animation_playback_reset(&st_pb);
      at_ms(p_anim, &st_pb, 0);
      gint64 i_ms = 0;
      for (guint u = 0; u < 3 * u_plays - 1; u++) {
         i_ms += 100;
         g_assert_true(at_ms(p_anim, &st_pb, i_ms));
      }
      g_assert_cmpuint(st_pb.u_frame, ==, 2);
      g_assert_false(st_pb.b_ended);
      g_assert_false(at_ms(p_anim, &st_pb, i_ms + 100)); /* plays done */
      g_assert_true(st_pb.b_ended);
      g_assert_cmpuint(st_pb.u_frame, ==, 2);
      g_assert_cmpuint(st_pb.u_played, ==, u_plays);
      g_assert_false(at_ms(p_anim, &st_pb, i_ms + 10000));
      g_assert_cmpuint(st_pb.u_frame, ==, 2);
      /* A reset (set_texture, remap, hold release) plays afresh. */
      animation_playback_reset(&st_pb);
      g_assert_false(st_pb.b_ended);
      g_assert_cmpuint(st_pb.u_frame, ==, 0);
      animation_delete(p_anim);
   }
}

/* A frame the decoder says holds for ever (-1) ends the playback on it
 * once shown; a first frame of -1 ends it at the anchor. */
static void
test_hold_for_ever_delay(void) {
   static const gint i_d[3] = {100, -1, 100};
   GgazeAnimation   *p_anim = mk_anim(i_d, 3, 0);
   GgazeAnimPlayback st_pb;
   animation_playback_reset(&st_pb);
   at_ms(p_anim, &st_pb, 0);
   g_assert_true(at_ms(p_anim, &st_pb, 100));
   g_assert_cmpuint(st_pb.u_frame, ==, 1);
   g_assert_true(st_pb.b_ended);
   g_assert_false(at_ms(p_anim, &st_pb, 10000));
   g_assert_cmpuint(st_pb.u_frame, ==, 1);
   animation_delete(p_anim);

   static const gint i_first[2] = {-1, 100};
   p_anim                       = mk_anim(i_first, 2, 0);
   animation_playback_reset(&st_pb);
   g_assert_false(at_ms(p_anim, &st_pb, 0));
   g_assert_true(st_pb.b_ended);
   g_assert_false(at_ms(p_anim, &st_pb, 10000));
   g_assert_cmpuint(st_pb.u_frame, ==, 0);
   animation_delete(p_anim);
}

/* The schedule runs on the clamped delays (GGAZE_ANIM_MIN_DELAY_MS): a
 * 10 ms frame -- what glycin hands over for a 10 ms GIF delay -- is due
 * 20 ms after the one before it, never 10. */
static void
test_schedule_uses_clamped_delay(void) {
   static const gint i_d[3] = {10, 10, 10};
   GgazeAnimation   *p_anim = mk_anim(i_d, 3, 0);
   GgazeAnimPlayback st_pb;
   animation_playback_reset(&st_pb);
   at_ms(p_anim, &st_pb, 0);
   g_assert_cmpint(st_pb.i_due_us, ==, T0_US + GGAZE_ANIM_MIN_DELAY_MS * 1000);
   g_assert_false(at_ms(p_anim, &st_pb, 10));
   g_assert_true(at_ms(p_anim, &st_pb, GGAZE_ANIM_MIN_DELAY_MS));
   animation_delete(p_anim);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);

   g_test_add_func("/animation/playback/anchor_and_due", test_anchor_and_due);
   g_test_add_func("/animation/playback/average_rate_and_loop",
                   test_average_rate_and_loop);
   g_test_add_func("/animation/playback/stall_resyncs", test_stall_resyncs);
   g_test_add_func("/animation/playback/play_count_holds_last_frame",
                   test_play_count_holds_last_frame);
   g_test_add_func("/animation/playback/hold_for_ever_delay",
                   test_hold_for_ever_delay);
   g_test_add_func("/animation/playback/schedule_uses_clamped_delay",
                   test_schedule_uses_clamped_delay);
   return (g_test_run());
}
