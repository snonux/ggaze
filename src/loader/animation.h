#ifndef GGAZE_ANIMATION_H
#define GGAZE_ANIMATION_H

/*:*
 * ggaze — animated GIF / WebP support (plain C)
 *
 * The parts of animation playback that need no widget, so they can be unit
 * tested without a display (task yb2, M5):
 *
 *   - animation_probe(): a byte-level walk of a GIF's blocks or a WebP's
 *     RIFF chunks that counts the frames and reads the canvas size, without
 *     a decoder. It is what decides whether the pixbuf backend asks
 *     gdk-pixbuf for a GdkPixbufAnimation at all, so the still path stays
 *     byte-for-byte what it was for every file that is not a multi-frame
 *     GIF or WebP, and "is this animated" is testable on bytes alone.
 *   - animation_within_budget(): the memory bound. A decoder holds every
 *     frame of an animation it has played once (gdk-pixbuf's GIF loader
 *     decodes them all up front, glycin fetches lazily and then caches), so
 *     an animation costs frames x canvas x 4 bytes. That is bounded to the
 *     same pixel count a single still may reach (GGAZE_IMAGE_MAX_PIXELS);
 *     beyond it only the first frame is decoded and shown.
 *   - animation_frame_delay_ms(): the clamp on what a frame's delay may be
 *     (a 0 ms GIF delay would spin the main loop; browsers clamp to ~20 ms
 *     for the same reason).
 *   - animation_attach() / animation_lookup(): how the animation travels
 *     with its first frame. The whole pipeline -- the texture LRU, the
 *     prefetch, last-write-wins, the enhance controller, the histogram, the
 *     clipboard, the tools -- deals in one GdkTexture per file, and that
 *     texture IS the first frame; the GdkPixbufAnimation rides on it as
 *     GObject qdata (one quark, defined here) and is looked up by the
 *     viewer alone. So nothing outside the pixbuf backend and the viewer
 *     had to learn a second type, the cache bounds the animation with the
 *     texture it belongs to, and every consumer that is not the viewer
 *     operates on the first frame, by construction.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <glib.h>

#include "detect.h"

G_BEGIN_DECLS

/* What animation_probe() read from the container, decoder-free. */
typedef struct {
   guint u_frames; /* image descriptors (GIF) or ANMF chunks (WebP) whose
                    * header is complete; 1 for a still WebP; 0 for a
                    * file that is neither format or too short to say */
   guint u_width;  /* the canvas (GIF logical screen / WebP VP8X canvas);
                    * 0 when unknown */
   guint u_height;
} GgazeAnimProbe;

/* Walk p_buf (the whole file, or as much of it as the caller has) and fill
 * *p_out. TRUE iff the bytes are a GIF or WebP carrying at least two
 * frames -- the only case the pixbuf backend decodes as an animation.
 * Never reads past u_len: a truncated file yields the frames whose headers
 * were complete before the cut (the decoder decides what it can show of
 * them), and garbage after a valid prefix ends the walk where it starts.
 * A GIF's frame count needs every block skipped in turn (each is only
 * length-prefixed), so this is O(file size), not a header peek. */
gboolean animation_probe(const guint8 *p_buf, gsize u_len,
                         GgazeAnimProbe *p_out);

/* The most pixels an animation may hold once every frame is decoded:
 * frames x canvas. The same figure as the still cap, so a playing
 * animation costs no more than the largest still the viewer admits
 * (400 MB RGBA); with the texture LRU's four entries that is the same
 * worst case the cache already allows for stills. */
#define GGAZE_ANIM_MAX_PIXELS GGAZE_IMAGE_MAX_PIXELS

/* TRUE iff p_probe's frames x canvas is within GGAZE_ANIM_MAX_PIXELS and
 * each side within GGAZE_IMAGE_MAX_SIDE (a zero side or zero frames is
 * FALSE: nothing to play). Checked 64-bit, so a crafted canvas cannot
 * wrap the product. */
gboolean animation_within_budget(const GgazeAnimProbe *p_probe);

/* The shortest delay the viewer schedules between two frames, in ms. GIF
 * delays are stored in 10 ms units and 0 is common in the wild (encoders
 * write it for "as fast as possible"); browsers play such frames at
 * roughly this rate rather than as fast as the machine can go. */
#define GGAZE_ANIM_MIN_DELAY_MS 20

/* Turn the delay a GdkPixbufAnimationIter reports into what the viewer
 * schedules: -1 stays -1 (the animation has ended on this frame: hold it,
 * schedule nothing), anything shorter than GGAZE_ANIM_MIN_DELAY_MS becomes
 * that minimum, the rest is returned as is. */
gint animation_frame_delay_ms(gint i_reported);

/* Make p_anim travel with p_tex (refs it; a later attach replaces, NULL
 * detaches). The texture's finalize drops the ref. */
void animation_attach(GdkTexture *p_tex, GdkPixbufAnimation *p_anim);

/* The animation attached to p_tex (transfer none), or NULL for a still
 * -- which is every texture the pixbuf backend did not attach to,
 * including a NULL p_tex. */
GdkPixbufAnimation *animation_lookup(GdkTexture *p_tex);

G_END_DECLS

#endif /* GGAZE_ANIMATION_H */
