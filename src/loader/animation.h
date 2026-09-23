#ifndef GGAZE_ANIMATION_H
#define GGAZE_ANIMATION_H

/*:*
 * ggaze — animated GIF / WebP support (plain C)
 *
 * The parts of animation playback that need no widget, so they can be unit
 * tested without a display (task yb2, M5):
 *
 *   - animation_probe(): a byte-level walk of a GIF's blocks or a WebP's
 *     RIFF chunks that counts the frames and reads the canvas size and the
 *     loop count, without a decoder. It is what decides whether the pixbuf backend asks
 *     gdk-pixbuf for a GdkPixbufAnimation at all, so the still path stays
 *     byte-for-byte what it was for every file that is not a multi-frame
 *     GIF or WebP, and "is this animated" is testable on bytes alone.
 *   - animation_within_budget(): the memory and playback bound. Every
 *     frame of a played animation is held as its own texture (see
 *     GgazeAnimation below), so an animation costs frames x canvas x 4
 *     bytes, bounded by GGAZE_ANIM_MAX_PIXELS (32 Mi pixels, 128 MiB; the
 *     memory arithmetic is at that constant); on top of that the canvas
 *     and the frame count have caps of their own
 *     (GGAZE_ANIM_MAX_CANVAS_PIXELS, GGAZE_ANIM_MAX_FRAMES). Beyond any of
 *     them only the first frame is decoded and shown, as a still.
 *   - animation_frame_delay_ms(): the clamp on what a frame's delay may be.
 *     Both decoders ggaze meets already turn a 0 ms GIF delay into 100 ms
 *     before ggaze sees it (gdk-pixbuf 2.42's io-gif.c and the glycin
 *     bridge of 2.44, measured), and 2.42's GIF loader raises anything
 *     under 20 ms to 20 ms itself; what still arrives below 20 ms -- a
 *     10 ms GIF delay on glycin, a 10 ms WebP frame on either -- is
 *     raised to 20 ms here, the rate browsers clamp to, so a crafted file
 *     cannot make the viewer redraw on every vblank.
 *   - GgazeAnimPlayback / animation_playback_advance(): which frame is on
 *     screen at a given time -- the schedule, the stall resync and the
 *     loop count -- as plain C over a caller-supplied clock, so the
 *     viewer only drives it from its frame clock and the rules are unit
 *     tested with a fake one.
 *   - GgazeAnimation: the frames themselves, decoded ONCE in the loader's
 *     worker thread (pixbuf_util_animation_to_texture) into one immutable
 *     GdkTexture per frame plus each frame's delay. The viewer only picks
 *     which of them to draw, so playing costs the main thread no decode,
 *     no composition and no pixel copy, and each frame is uploaded once
 *     rather than once per loop. The textures own their pixels: gdk-pixbuf
 *     2.42's GIF loader composites every frame into ONE reusable buffer,
 *     so a texture wrapping what an iterator returned would change under
 *     every consumer at the next advance (review finding 1 of yb2).
 *   - animation_attach() / animation_lookup(): how the animation travels
 *     with its first frame. The whole pipeline -- the texture LRU, the
 *     prefetch, last-write-wins, the enhance controller, the histogram, the
 *     clipboard, the tools -- deals in one GdkTexture per file, and that
 *     texture IS the first frame; the GgazeAnimation rides on it as GObject
 *     qdata (one quark, defined here) and is looked up by the viewer alone.
 *     So nothing outside the pixbuf backend and the viewer had to learn a
 *     second type, the cache bounds the animation with the texture it
 *     belongs to, and every consumer that is not the viewer operates on the
 *     first frame, by construction.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

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
   guint u_plays; /* how many times the animation plays in all; 0 means
                   * for ever. GIF: 1 without a NETSCAPE2.0 loop
                   * extension (the GIF89a default browsers follow), 0
                   * for a loop count of 0, N + 1 for a count of N (the
                   * count is repetitions after the first play: Chrome and
                   * gdk-pixbuf 2.42 agree). WebP: the ANIM chunk's count
                   * as is (the spec's "number of times to loop", i.e.
                   * plays in all), 0 without an ANIM chunk. */
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
 * frames x canvas = 32 Mi pixels, 128 MiB of RGBA. The arithmetic behind
 * the figure: a played animation holds 4 bytes x frames x canvas for as
 * long as its first frame is referenced; the texture LRU keeps 4 entries
 * (plus the enhance controller's original of the current file), and up to
 * three decodes are in flight (the visible load and two neighbour
 * prefetches), each peaking at about twice what it will hold -- the
 * decoder's own frames and the copies made from them live side by side
 * until the decoder is dropped. So at this cap the worst case is 4 x 128
 * = 512 MiB held plus 3 x 256 MiB at peak, ~1.3 GiB, if every file in
 * sight is a maximal animation. The still cap (GGAZE_IMAGE_MAX_PIXELS,
 * 100 M pixels) would have allowed ~400 MB held and ~0.9 GB peak for a
 * SINGLE animation (a 98 M pixel GIF), which a viewer has no business
 * spending on a GIF. Real animations fit easily: 480 x 270 x 250 frames
 * is 32 M pixels, a 640 x 480 clip plays 109 frames; above the cap only
 * the first frame is shown, as a still. */
#define GGAZE_ANIM_MAX_PIXELS (32u * 1024u * 1024u)

/* The largest canvas that plays (4 Mi pixels, e.g. 2048 x 2048). Each
 * frame is uploaded to the GPU the first time it is drawn, on the main
 * thread, and a frame of this size is 16 MiB; a bigger canvas is shown as
 * its first frame. */
#define GGAZE_ANIM_MAX_CANVAS_PIXELS (4u * 1024u * 1024u)

/* The most frames that play. Far above real animations (a 30 s clip at
 * 30 fps is 900), low enough that a crafted file of 200000 1x1 frames --
 * which the pixel budget alone would admit -- cannot make the worker build
 * a texture object per frame. */
#define GGAZE_ANIM_MAX_FRAMES 1000u

/* TRUE iff p_probe may play: at least one frame and at most
 * GGAZE_ANIM_MAX_FRAMES, each side non-zero and within
 * GGAZE_IMAGE_MAX_SIDE, the canvas within GGAZE_ANIM_MAX_CANVAS_PIXELS
 * and frames x canvas within GGAZE_ANIM_MAX_PIXELS. Checked 64-bit, so a
 * crafted canvas cannot wrap the product. */
gboolean animation_within_budget(const GgazeAnimProbe *p_probe);

/* The shortest delay the viewer schedules between two frames, in ms, the
 * rate browsers clamp fast GIFs to. GIF delays are stored in 10 ms units;
 * a 0 in the file never reaches this clamp (the decoders make it 100 ms,
 * see the top of this file), a 10 ms one can (glycin passes it through,
 * as both decoders do for a 10 ms WebP frame) and is raised to this. */
#define GGAZE_ANIM_MIN_DELAY_MS 20

/* Turn the delay a GdkPixbufAnimationIter reports into what the viewer
 * schedules: -1 stays -1 (the animation has ended on this frame: hold it,
 * schedule nothing), anything shorter than GGAZE_ANIM_MIN_DELAY_MS (in
 * practice a 10 ms delay; a 0 ms one arrives as 100 ms) becomes that
 * minimum, the rest is returned as is. */
gint animation_frame_delay_ms(gint i_reported);

/* The decoded frames of one animation. Frame 0 is NOT stored: it is the
 * texture the animation is attached to (animation_attach), which keeps the
 * two from referencing each other. Built once by the loader's worker, then
 * only read, so a finished GgazeAnimation may be read from any thread. */
typedef struct _GgazeAnimation GgazeAnimation;

/* A new animation whose first frame shows for i_first_delay_ms (as the
 * decoder reported it; animation_get_delay_ms clamps). */
GgazeAnimation *animation_new(gint i_first_delay_ms);
void            animation_delete(GgazeAnimation *p_anim);

/* Append the next frame (refs p_frame, which must own its pixels -- see
 * the top of this file) and the delay it shows for, as reported. */
void animation_append_frame(GgazeAnimation *p_anim, GdkTexture *p_frame,
                            gint i_delay_ms);

/* How many times the animation plays in all (GgazeAnimProbe.u_plays): 0,
 * the default of animation_new, plays for ever. The viewer holds the last
 * frame once the count is reached (animation_playback_advance). */
void  animation_set_plays(GgazeAnimation *p_anim, guint u_plays);
guint animation_get_plays(const GgazeAnimation *p_anim);

/* Frames including the first; 1 until a frame is appended. */
guint animation_get_n_frames(const GgazeAnimation *p_anim);

/* Frame u_idx (transfer none), for 1 <= u_idx < n; NULL for 0 (that is
 * the texture the animation rides on) and for an index out of range. */
GdkTexture *animation_get_frame(const GgazeAnimation *p_anim, guint u_idx);

/* How long frame u_idx shows, through animation_frame_delay_ms: -1 means
 * "hold this frame, the animation has ended", anything else is at least
 * GGAZE_ANIM_MIN_DELAY_MS. -1 for an index out of range. */
gint animation_get_delay_ms(const GgazeAnimation *p_anim, guint u_idx);

/* Where a playing animation is: the frame on screen, how many plays are
 * complete, when (on the caller's clock, microseconds) the next frame is
 * due, and whether it has ended on the frame shown. Zero it with
 * animation_playback_reset() to (re)start from the first frame. */
typedef struct {
   guint    u_frame;  /* the frame on screen; 0 is the carrier texture */
   guint    u_played; /* complete plays so far */
   gint64   i_due_us; /* when the next frame is due; 0 until anchored */
   gboolean b_ended;  /* the frame on screen holds for good */
} GgazeAnimPlayback;

void animation_playback_reset(GgazeAnimPlayback *p_pb);

/* Move p_pb to time i_now_us. The first call after a reset anchors the
 * schedule (frame 0 is due to be replaced one delay from now) and changes
 * nothing. Later calls show the next frame once it is due, one frame per
 * call: the due time moves on by the new frame's delay so the average
 * rate is the file's, except after a stall (the new due time would
 * already be past), where it restarts from i_now_us instead of racing
 * through the frames that were missed. After the last frame another play
 * starts unless the animation's plays are complete, in which case the
 * last frame holds and b_ended is set; a frame whose delay is -1 also
 * ends it, held. TRUE iff u_frame changed. */
gboolean animation_playback_advance(const GgazeAnimation *p_anim,
                                    GgazeAnimPlayback    *p_pb,
                                    gint64                i_now_us);

/* Make p_anim travel with p_tex, which takes ownership of it (transfer
 * full: the texture's finalize deletes it); a later attach replaces and
 * deletes the previous one, NULL detaches. */
void animation_attach(GdkTexture *p_tex, GgazeAnimation *p_anim);

/* The animation attached to p_tex (transfer none), or NULL for a still
 * -- which is every texture the pixbuf backend did not attach to,
 * including a NULL p_tex. */
const GgazeAnimation *animation_lookup(GdkTexture *p_tex);

G_END_DECLS

#endif /* GGAZE_ANIMATION_H */
