/*:*
 * ggaze — GdkPixbuf -> GdkTexture helpers
 *
 * See pixbuf-util.h. Pure functions over GdkPixbuf; no display needed.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "pixbuf-util.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

#include "animation.h"

GdkPixbuf *
pixbuf_util_upright(GdkPixbuf *p_pix) {
   g_return_val_if_fail(GDK_IS_PIXBUF(p_pix), NULL);
   /* apply_embedded_orientation returns NULL only on allocation failure;
    * an image without an orientation option comes back as a new ref. */
   GdkPixbuf *p_oriented = gdk_pixbuf_apply_embedded_orientation(p_pix);
   if (p_oriented != NULL) {
      return (p_oriented);
   }
   return (GDK_PIXBUF(g_object_ref(p_pix)));
}

GdkTexture *
pixbuf_util_to_texture(GdkPixbuf *p_pix) {
   g_return_val_if_fail(GDK_IS_PIXBUF(p_pix), NULL);
   int i_w = gdk_pixbuf_get_width(p_pix);
   int i_h = gdk_pixbuf_get_height(p_pix);
   if (i_w <= 0 || i_h <= 0) {
      return (NULL);
   }
   /* GdkPixbuf stores non-premultiplied R8G8B8A8 when it has alpha; add an
    * alpha channel otherwise. gdk_pixbuf_add_alpha uses g_try_malloc and
    * returns NULL when the copy cannot be allocated. */
   GdkPixbuf *p_rgba = gdk_pixbuf_get_has_alpha(p_pix)
                          ? GDK_PIXBUF(g_object_ref(p_pix))
                          : gdk_pixbuf_add_alpha(p_pix, FALSE, 0, 0, 0);
   if (p_rgba == NULL) {
      return (NULL);
   }
   int     i_rowstride = gdk_pixbuf_get_rowstride(p_rgba);
   guchar *p_pixels    = gdk_pixbuf_get_pixels(p_rgba);
   /* The last row need not be padded to the full rowstride. */
   gsize   u_len   = (gsize)(i_h - 1) * (gsize)i_rowstride + (gsize)i_w * 4u;
   GBytes *p_bytes = g_bytes_new_with_free_func(
      p_pixels, u_len, (GDestroyNotify)g_object_unref, p_rgba);
   GdkTexture *p_tex = gdk_memory_texture_new(i_w, i_h, GDK_MEMORY_R8G8B8A8,
                                              p_bytes, (gsize)i_rowstride);
   g_bytes_unref(p_bytes);
   return (p_tex);
}

GdkTexture *
pixbuf_util_to_upright_texture(GdkPixbuf *p_pix) {
   GdkPixbuf *p_up = pixbuf_util_upright(p_pix);
   if (p_up == NULL) {
      return (NULL);
   }
   GdkTexture *p_tex = pixbuf_util_to_texture(p_up);
   g_object_unref(p_up);
   return (p_tex);
}

/* Feed p_buf to a fresh GdkPixbufLoader and close it: the one decode both
 * pixbuf_util_decode_bytes() and pixbuf_util_decode_animation_bytes()
 * read their result from. Returns the closed loader (caller unrefs), or
 * NULL with p_err set when the write failed.
 *
 * A loader must be closed before it is finalized or GdkPixbuf logs a
 * warning per corrupt file (fatal under G_DEBUG=fatal-warnings), so both
 * exits close it; on the write-failure exit the close error is irrelevant
 * (the write error is the one reported), and a close that fails on
 * truncated data may still leave a usable pixbuf. */
static GdkPixbufLoader *
_closed_loader_over_bytes(const guchar *p_buf, gsize u_len, GError **p_err) {
   GdkPixbufLoader *p_loader = gdk_pixbuf_loader_new();
   GError          *p_sub    = NULL;
   if (!gdk_pixbuf_loader_write(p_loader, p_buf, u_len, &p_sub)) {
      g_propagate_error(p_err, p_sub);
      gdk_pixbuf_loader_close(p_loader, NULL);
      g_object_unref(p_loader);
      return (NULL);
   }
   if (!gdk_pixbuf_loader_close(p_loader, &p_sub)) {
      g_clear_error(&p_sub);
   }
   return (p_loader);
}

GdkPixbuf *
pixbuf_util_decode_bytes(const guchar *p_buf, gsize u_len, GError **p_err) {
   GdkPixbufLoader *p_loader = _closed_loader_over_bytes(p_buf, u_len, p_err);
   if (p_loader == NULL) {
      return (NULL);
   }
   GdkPixbuf *p_pix = gdk_pixbuf_loader_get_pixbuf(p_loader);
   if (p_pix == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "could not decode image (GdkPixbuf produced no pixbuf)");
   } else {
      g_object_ref(p_pix);
   }
   g_object_unref(p_loader);
   return (p_pix);
}

GdkPixbufAnimation *
pixbuf_util_decode_animation_bytes(const guchar *p_buf, gsize u_len,
                                   GError **p_err) {
   GdkPixbufLoader *p_loader = _closed_loader_over_bytes(p_buf, u_len, p_err);
   if (p_loader == NULL) {
      return (NULL);
   }
   /* gdk-pixbuf 2.44 deprecates its whole animation API in favour of
    * glycin's, which CI's fedora:40 (2.42) does not have and ggaze does
    * not depend on; the deprecated calls are the portable ones, so their
    * warnings are silenced at each site rather than for the file. */
   G_GNUC_BEGIN_IGNORE_DEPRECATIONS
   GdkPixbufAnimation *p_anim = gdk_pixbuf_loader_get_animation(p_loader);
   G_GNUC_END_IGNORE_DEPRECATIONS
   if (p_anim == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "could not decode animation (GdkPixbuf produced no frames)");
   } else {
      g_object_ref(p_anim);
   }
   g_object_unref(p_loader);
   return (p_anim);
}

/* --- animation frames (yb2) -------------------------------------------------
 *
 * gdk-pixbuf 2.44 deprecates its whole animation API in favour of glycin's
 * (see pixbuf_util_decode_animation_bytes above), so this section silences
 * the warnings for the calls it has to make. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

/* A texture that owns the pixels of the frame p_it shows now. The iterator's
 * pixbuf is the decoder's, and gdk-pixbuf 2.42's GIF loader keeps only the
 * compressed data and composites every frame on demand into ONE buffer it
 * hands out for all of them: wrapped as is, the "first frame" would turn
 * into whichever frame was composited last (the bug review finding 1 of
 * yb2 reproduced against 2.42.12). An opaque pixbuf needs no copy here:
 * pixbuf_util_to_texture adds its alpha channel into a fresh buffer. */
static GdkTexture *
_frame_texture(GdkPixbufAnimationIter *p_it) {
   GdkPixbuf *p_shared = gdk_pixbuf_animation_iter_get_pixbuf(p_it);
   if (p_shared == NULL) {
      return (NULL);
   }
   GdkPixbuf *p_own = gdk_pixbuf_get_has_alpha(p_shared)
                         ? gdk_pixbuf_copy(p_shared)
                         : GDK_PIXBUF(g_object_ref(p_shared));
   if (p_own == NULL) {
      return (NULL); /* the copy could not be allocated */
   }
   GdkTexture *p_tex = pixbuf_util_to_texture(p_own);
   g_object_unref(p_own);
   return (p_tex);
}

/* Move p_it to i_t_us on its (synthetic) clock. */
static void
_iter_seek(GdkPixbufAnimationIter *p_it, gint64 i_t_us) {
   GTimeVal st_t = {(glong)(i_t_us / G_USEC_PER_SEC),
                    (glong)(i_t_us % G_USEC_PER_SEC)};
   gdk_pixbuf_animation_iter_advance(p_it, &st_t);
}

/* What gdk_pixbuf_animation_iter_get_delay_time() means differs between
 * implementations, measured: gdk-pixbuf 2.42's own GIF loader reports the
 * time LEFT in the current frame (100, then 99 one millisecond later),
 * while webp-pixbuf-loader 0.2.7 and the glycin bridge of 2.44 report the
 * frame's whole delay however far into it the clock is. TRUE for the
 * first kind; asked 1 ms into frame 0 of p_it, freshly made at i_t0_us
 * and whose frame 0 reported i_first there. A first frame shorter than
 * 2 ms cannot tell the two apart and is taken as the second kind. */
static gboolean
_iter_counts_down(GdkPixbufAnimationIter *p_it, gint64 i_t0_us, gint i_first) {
   if (i_first < 2) {
      return (FALSE);
   }
   _iter_seek(p_it, i_t0_us + 1000);
   return (gdk_pixbuf_animation_iter_get_delay_time(p_it) == i_first - 1);
}

/* The whole delay of the frame p_it shows, sampled 1 ms into it (see
 * _append_frames): a counting-down iterator is 1 ms short there. -1 (the
 * animation ends on this frame) stays -1. */
static gint
_frame_delay(GdkPixbufAnimationIter *p_it, gboolean b_countdown) {
   gint i_delay = gdk_pixbuf_animation_iter_get_delay_time(p_it);
   return ((b_countdown && i_delay >= 0) ? i_delay + 1 : i_delay);
}

/* Step p_it through frames 1..u_frames-1 into p_anim. The iterator runs on
 * a synthetic clock rather than the wall clock: frame k is sampled 1 ms
 * into its slot, at the sum of the delays before it plus 1 ms, because
 * glycin's iterator treats a slot's start as still belonging to the
 * previous frame and 2.42's as the new one's -- sampling exactly at a
 * boundary returned the previous frame on glycin (measured: frame 0
 * twice, frame 3 lost). Sampled there, a counting-down iterator
 * (b_countdown, _iter_counts_down) reports its frame 1 ms short, which is
 * added back so every frame keeps the delay the file gave it. A frame the
 * decoder says holds for ever (delay -1: the animation ends on it) is the
 * last one taken. FALSE with p_err set on cancel or an unwrappable
 * frame.
 *
 * Known limit: a 1 ms frame (only a WebP can say that; a GIF's delays are
 * 10 ms units and 2.42 raises them to 20 ms) is shorter than any sampling
 * point every iterator agrees on. Measured at 1 ms slots: glycin's
 * iterator (microsecond clock) and webp-pixbuf-loader 0.2.7's
 * (millisecond clock) give the slot's start to the previous frame,
 * GdkPixbufSimpleAnim's and 2.42's GIF iterator (millisecond clock) give
 * it to the new one, so no fixed offset lands inside a 1 ms slot for all
 * of them. Such frames may be taken twice or skipped -- a stutter in an
 * animation that plays at GGAZE_ANIM_MIN_DELAY_MS anyway, never a picture
 * that is not one of the file's frames. */
static gboolean
_append_frames(GdkPixbufAnimationIter *p_it, GgazeAnimation *p_anim,
               guint u_frames, gint64 i_t0_us, gboolean b_countdown,
               GCancellable *p_cancel, GError **p_err) {
   gint64 i_slot_ms = 0;
   gint   i_delay   = _frame_delay(p_it, b_countdown); /* frame 0's */
   for (guint u = 1; u < u_frames && i_delay >= 0; u++) {
      if (g_cancellable_set_error_if_cancelled(p_cancel, p_err)) {
         return (FALSE);
      }
      /* MAX 1: a frame reported as 0 ms still moves the clock on. */
      i_slot_ms += MAX(i_delay, 1);
      _iter_seek(p_it, i_t0_us + (i_slot_ms + 1) * 1000);
      GdkTexture *p_frame = _frame_texture(p_it);
      if (p_frame == NULL) {
         g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "could not build texture from animation frame %u", u);
         return (FALSE);
      }
      i_delay = _frame_delay(p_it, b_countdown);
      animation_append_frame(p_anim, p_frame, i_delay);
      g_object_unref(p_frame);
   }
   return (TRUE);
}

/* The still a static "animation" stands for, as a texture of its own. */
static GdkTexture *
_static_texture(GdkPixbufAnimation *p_anim, GError **p_err) {
   GdkPixbuf  *p_pix = gdk_pixbuf_animation_get_static_image(p_anim);
   GdkPixbuf  *p_own = (p_pix != NULL) ? gdk_pixbuf_copy(p_pix) : NULL;
   GdkTexture *p_tex = (p_own != NULL) ? pixbuf_util_to_texture(p_own) : NULL;
   g_clear_object(&p_own);
   if (p_tex == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "could not build texture from the animation's image");
   }
   return (p_tex);
}

GdkTexture *
pixbuf_util_animation_to_texture(GdkPixbufAnimation   *p_anim,
                                 const GgazeAnimProbe *p_probe,
                                 GCancellable *p_cancel, GError **p_err) {
   g_return_val_if_fail(GDK_IS_PIXBUF_ANIMATION(p_anim), NULL);
   g_return_val_if_fail(p_probe != NULL, NULL);
   gint64                  i_t0 = g_get_real_time();
   GTimeVal                st_t = {(glong)(i_t0 / G_USEC_PER_SEC),
                                   (glong)(i_t0 % G_USEC_PER_SEC)};
   GdkPixbufAnimationIter *p_it =
      gdk_pixbuf_animation_is_static_image(p_anim)
         ? NULL
         : gdk_pixbuf_animation_get_iter(p_anim, &st_t);
   if (p_it == NULL) {
      return (_static_texture(p_anim, p_err));
   }
   GdkTexture     *p_first  = _frame_texture(p_it);
   gint            i_first  = gdk_pixbuf_animation_iter_get_delay_time(p_it);
   GgazeAnimation *p_frames = animation_new(i_first);
   gboolean        b_down   = _iter_counts_down(p_it, i_t0, i_first);
   animation_set_plays(p_frames, p_probe->u_plays);
   if (p_first == NULL || !_append_frames(p_it, p_frames, p_probe->u_frames,
                                          i_t0, b_down, p_cancel, p_err)) {
      if (p_first == NULL) {
         g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "could not build texture from the first frame");
      }
      animation_delete(p_frames);
      g_clear_object(&p_first);
   } else if (animation_get_n_frames(p_frames) < 2) {
      animation_delete(p_frames); /* ended on its first frame: a still */
   } else {
      animation_attach(p_first, p_frames);
   }
   g_object_unref(p_it);
   return (p_first);
}

G_GNUC_END_IGNORE_DEPRECATIONS
