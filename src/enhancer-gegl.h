#ifndef GGAZE_ENHANCER_GEGL_H
#define GGAZE_ENHANCER_GEGL_H

/*:*
 * ggaze — GEGL enhance operations (feature-gated)
 *
 * The GeglBuffer/GdkTexture operations on top of the Enhancer preset engine.
 * They are pure functions of their arguments (the preset list is passed in),
 * so none takes an Enhancer instance.
 * Guarded on GGAZE_HAVE_GEGL: the whole header is empty in a non-GEGL build,
 * so it can be included unconditionally alongside enhancer.h without pulling
 * <gegl.h> into a build that does not have GEGL. See enhancer.h for the
 * GEGL-agnostic preset metadata.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "enhancer.h"
#include "preview-scale.h"
#include "transform.h"

#if GGAZE_HAVE_GEGL

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <gegl.h>

G_BEGIN_DECLS

/* Apply one preset (built-in or user graph) to a GeglBuffer. Returns a new
 * buffer, or NULL with p_err set. */
GeglBuffer *enhancer_apply(GeglBuffer *p_in, const EnhancerPreset *p_preset,
                           GError **p_err);

/* Apply the enabled presets (bit i of u_mask -> preset i) in array order,
 * then the geometric transform p_xf (nullable = identity; decision #35
 * order: rotate 90 -> straighten [+ auto-crop] -> crop), composing them into
 * one graph. Returns a new buffer, or NULL if nothing is enabled (empty mask
 * AND identity transform) / on error. Built-in and user-graph presets mix
 * freely. The output size is exactly transform_output_size() of the input
 * (the chain crops the straighten's padded bounding box to the analytic
 * size), which is what lets the crop tool lay out its rectangle before the
 * preview has rendered. */
GeglBuffer *enhancer_apply_chain(GeglBuffer *p_in, const GPtrArray *p_presets,
                                 guint32 u_mask, const Transform *p_xf,
                                 GError **p_err);

/* Export the enhanced buffer to a file. The saver is chosen from p_out's
 * extension: .jpg/.jpeg -> gegl:jpg-save (quality 95), .png -> gegl:png-save,
 * .webp -> gegl:webp-save (if available). Other extensions fail with
 * G_IO_ERROR_NOT_SUPPORTED. Success is verified by a real stat of the output
 * (not pre-existence). Colour (decision #45): the PNG and JPEG savers embed
 * the buffer's colour space as its ICC profile -- for a source with an
 * embedded profile, a profile EQUIVALENT to it (the same curves, primaries
 * within babl's tolerance): its own bytes when babl made the space from
 * it, the bytes of an earlier equivalent profile when babl answered with
 * that one's space (enhancer.c, "export"); WebP cannot carry one, and its
 * saver reads the pixels as sRGB (babl converts a buffer in another space
 * on the way out). An sRGB buffer exports exactly as before. Returns TRUE
 * on success. */
gboolean enhancer_export(GeglBuffer *p_in, const EnhancerPreset *p_preset,
                         GFile *p_out, GError **p_err);

/* Export p_in with the enabled-preset chain (u_mask) and the transform p_xf
 * (nullable) composed, to p_out. */
gboolean enhancer_export_chain(GeglBuffer *p_in, const GPtrArray *p_presets,
                               guint32 u_mask, const Transform *p_xf,
                               GFile *p_out, GError **p_err);

/* Async export: load p_src, apply the chain + transform, save to p_out --
 * all in a GTask worker, because the full-resolution decode + GEGL chain +
 * encode takes seconds on a 40 MP photo and used to freeze the UI
 * (AGENTS.md: decode runs in GTask threads). p_presets and p_xf (nullable)
 * are snapshotted. Finish returns TRUE on a real write. */
void     enhancer_export_chain_async(GFile *p_src, const GPtrArray *p_presets,
                                     guint32 u_mask, const Transform *p_xf,
                                     GFile *p_out, GCancellable *p_cancel,
                                     GAsyncReadyCallback p_cb, gpointer p_data);
gboolean enhancer_export_chain_finish(GAsyncResult *p_res, GError **p_err);

/* Load a file into an upright (EXIF Orientation applied) RGBA8 GeglBuffer
 * whose babl format carries the image's colour space (decision #45). A
 * local PNG or JPEG whose embedded ICC profile babl parses to a space
 * other than sRGB decodes through GEGL's ICC-aware gegl:png-load /
 * gegl:jpg-load, which tag the buffer with that space -- but only when
 * the loader's decode gate and loader/intact.h vouch for the file, and the
 * decode comes out at the header's size. Every other file -- untagged,
 * sRGB-profiled, non-local, or one the managed path does not vouch for
 * (truncated, corrupt image data) -- goes through ggaze's orientation-
 * aware loader and is tagged sRGB, exactly as before xb2: the managed
 * path never refuses a file the loader reads, and never loads one it
 * refuses. Returns a new buffer (caller unrefs) or NULL with p_err set
 * (the loader's error). */
GeglBuffer *enhancer_load(GFile *p_file, GError **p_err);

/* Whether enhancer_load() may decode p_file colour-managed, as far as the
 * file's headers tell: a local PNG / JPEG (a JPEG only in a build with
 * the `jpeg` feature) within the size caps, GEGL's loader op installed,
 * an embedded profile the managed path vouches for (icc_babl_kind, babl,
 * within the per-process profile cap) to a space other than sRGB, for the
 * image's number of colour components. It asks babl through the render's
 * own verdict table, so a new profile the card meets first takes its slot
 * (GGAZE_ENHANCER_MAX_PROFILES) here, once, and the enhance of that file
 * then finds the verdict kept. The completeness checks
 * are NOT run (they read the whole file: ~150 ms on average for a camera
 * file) -- a file whose data turns out broken, a truncated scan say, still
 * falls back to the loader path -- so this is the cheap answer behind the
 * info card's "may be managed on enhance/export", not a promise.
 * Thread-safe (the info card asks from a worker). */
gboolean enhancer_would_manage(GFile *p_file);

/* Convert a GeglBuffer to a GdkTexture for preview: sRGB RGBA8 bytes, so a
 * buffer in another space is colour-converted here (babl), which is what
 * makes a wide-gamut preview look right. Returns a new GdkTexture (caller
 * unrefs) or NULL with p_err set. Needs no display. */
GdkTexture *enhancer_buffer_to_texture(GeglBuffer *p_buf, GError **p_err);

/* Async: load p_file, apply the enabled-preset chain (u_mask) and the
 * transform p_xf (nullable), and convert the result to a GdkTexture, all off
 * the calling thread (a GTask worker) so a caller with a main loop (e.g. the
 * window) is never blocked by GEGL's CPU-heavy processing (tu0). p_presets
 * and p_xf are snapshotted internally before the worker starts, so a
 * concurrent enhancer_set_presets() (Preferences apply) or a tool nudge
 * cannot race it. p_cancel may be NULL. Since GEGL processing itself cannot
 * be interrupted mid-flight, cancellation only skips work that has not
 * started yet (and cuts the managed path's whole-file checks short, which
 * then decline) -- a caller that needs last-write-wins semantics (e.g. a
 * newer apply superseding this one) must still check that on its own
 * before using the finished result. */
void enhancer_apply_chain_async(GFile *p_file, const GPtrArray *p_presets,
                                guint32 u_mask, const Transform *p_xf,
                                GCancellable       *p_cancel,
                                GAsyncReadyCallback p_cb, gpointer p_data);

/* Finish enhancer_apply_chain_async(). Returns a new GdkTexture (caller
 * unrefs) or NULL with p_err set. *p_orig_w / *p_orig_h (each nullable)
 * receive the ORIGINAL image's upright size, the number the transform's base
 * size is computed from (transform_base_size); the controller records it so
 * the crop tool and a later quarter turn know the image they work on without
 * a second decode. *pb_managed (nullable) says whether the decode was
 * colour-managed: the file's embedded profile applied by GEGL's loader, in
 * whatever working space (a CMYK or grey file's chain runs in sRGB, yet its
 * pixels are the profile's). The plain view shows an unmanaged decode, so
 * a before/after compare of a managed render against it would show a
 * colour shift no preset caused: the controller then asks for the managed
 * original (below) for hold-Space. */
GdkTexture *enhancer_apply_chain_finish(GAsyncResult *p_res, gint *p_orig_w,
                                        gint *p_orig_h, gboolean *pb_managed,
                                        GError **p_err);

/* Async: the ORIGINAL of p_file through the same colour-managed decode as
 * the render (enhancer_load's managed path: the identity chain,
 * untransformed, converted to sRGB for display), in a GTask worker. Asked
 * for lazily -- on the first hold-Space of a managed render -- because it
 * is a second full-size texture (w x h x 4 bytes) the caller keeps outside
 * the texture cache's cap. p_cancel may be NULL (it cuts the whole-file
 * checks short; a cancelled task finishes with G_IO_ERROR_CANCELLED). */
void enhancer_managed_original_async(GFile *p_file, GCancellable *p_cancel,
                                     GAsyncReadyCallback p_cb, gpointer p_data);

/* Finish enhancer_managed_original_async(): a new GdkTexture (caller
 * unrefs); NULL WITHOUT an error whenever the managed path declines the
 * file -- rewritten meanwhile into one with nothing to manage, gone, past
 * the profile cap: the plain original is then the right compare, and the
 * worker never decodes one itself (the caller already shows it); NULL
 * with G_IO_ERROR_CANCELLED when p_cancel fired (nothing decoded after
 * that), or with p_err set when the managed buffer cannot be converted. */
GdkTexture *enhancer_managed_original_finish(GAsyncResult *p_res,
                                             GError      **p_err);

/* --- the live preview (8l2, decision #53) ---------------------------------
 *
 * The edit panel's preview does not run the chain on the full-resolution
 * image (~10 s per key on a 64 MP photo): it renders from a SOURCE, the
 * image decoded once and scaled down to what the screen shows
 * (preview-scale.h), kept by the controller for as long as the image is
 * the same, so a toggle or a strength step costs one chain on ~2 MP. The
 * export alone runs at full resolution (enhancer_export_chain_async),
 * exactly as before. */

/* A decoded, scaled-down image the preview renders from. Owned by whoever
 * finished its build; the workers that use it hold references to its
 * buffers, not to it, so it may be deleted while a render is in flight. */
typedef struct EnhancerSource EnhancerSource;

/* Build p_file's source for the viewport p_view (copied), in a worker. The
 * decode is enhancer_load's: the managed path when it applies (the whole
 * image decoded through GEGL, then scaled), else ggaze's loader -- but a
 * file the loader path decodes is NOT decoded again when p_decoded (may
 * be NULL) is given: that is the loader's decode of p_file the viewer
 * shows, and the source is scaled straight out of its pixels (~0.3 s for
 * 64 MP where a decode takes ~0.7 s more). Cancellation skips what has not
 * started (G_IO_ERROR_CANCELLED). */
void enhancer_source_new_async(GFile *p_file, GdkTexture *p_decoded,
                               const PreviewView *p_view,
                               GCancellable *p_cancel, GAsyncReadyCallback p_cb,
                               gpointer p_data);
/* The source (caller deletes), or NULL with p_err set (the load's error). */
EnhancerSource *enhancer_source_new_finish(GAsyncResult *p_res, GError **p_err);
void            enhancer_source_delete(EnhancerSource *p_src);

/* The file it was built from (borrowed). */
GFile *enhancer_source_get_file(const EnhancerSource *p_src);
/* The original's upright size. */
void enhancer_source_get_orig_size(const EnhancerSource *p_src, gint *p_w,
                                   gint *p_h);
/* Source pixels per image pixel, (0, 1]: 1 is the image itself. */
gdouble enhancer_source_get_scale(const EnhancerSource *p_src);
/* Whether the decode was colour-managed (enhancer_apply_chain_finish's
 * *pb_managed). */
gboolean enhancer_source_is_managed(const EnhancerSource *p_src);
/* A scaled source's own pixels as a texture standing for the original's
 * size (logical-size.h) -- what hold-Space compares a preview against, so
 * the compare is like with like: the same resolution, the same (managed or
 * not) decode. NULL for a source at scale 1: the original on screen IS
 * that then. Borrowed. */
GdkTexture *enhancer_source_get_original(const EnhancerSource *p_src);
/* TRUE iff p_src was built from p_file and is fine enough for p_view
 * (preview_scale_covers): a larger window or a finer device asks for a new
 * one, a smaller one does not. */
gboolean enhancer_source_serves(const EnhancerSource *p_src, GFile *p_file,
                                const PreviewView *p_view);

/* Render the chain (presets in u_mask -- resolved, enhancer_presets_resolve
 * -- then p_xf, nullable, in the ORIGINAL's pixels) on p_src, in a worker:
 * the transform is scaled onto the source (transform_scale) and every
 * preset's pixel lengths with it (preview-scale.h), and the texture comes
 * back standing for the size the export would have (transform_output_size
 * of the original, logical-size.h). p_presets and p_xf are snapshotted.
 * Cancellation skips a render that has not started. */
void enhancer_source_render_async(const EnhancerSource *p_src,
                                  const GPtrArray *p_presets, guint32 u_mask,
                                  const Transform *p_xf, GCancellable *p_cancel,
                                  GAsyncReadyCallback p_cb, gpointer p_data);
/* The rendered texture (caller unrefs), or NULL with p_err set. */
GdkTexture *enhancer_source_render_finish(GAsyncResult *p_res, GError **p_err);

/* The card thumbnails, from p_src's own thumbnail-sized copy
 * (PREVIEW_SCALE_THUMB_SIDE, made with the source -- no decode): the
 * original followed by one independent preview per preset (pixel lengths
 * scaled like the preview's). The returned array owns its GdkTexture
 * entries; index 0 is the original and index i + 1 preset i; a preset that
 * cannot be applied is NULL. The worker checks p_cancel between presets
 * (a newer preview render cancels a batch in flight). */
void       enhancer_preview_thumbnails_async(const EnhancerSource *p_src,
                                             const GPtrArray      *p_presets,
                                             GCancellable         *p_cancel,
                                             GAsyncReadyCallback   p_cb,
                                             gpointer              p_data);
GPtrArray *enhancer_preview_thumbnails_finish(GAsyncResult *p_res,
                                              GError      **p_err);

/* Test seam: every source render sleeps u_ms first (0: none), so a test
 * can hold a preview pending past the controller's indicator delay. Not
 * thread-safe: set it only while no render is in flight. */
void enhancer_test_set_render_delay(guint u_ms);

/* Test seam: while b_fail is set every source render fails (with
 * G_IO_ERROR_FAILED) instead of running -- the one way left to make a
 * preview render fail on purpose, now that it no longer reads the file.
 * Same threading rule as the delay. */
void enhancer_test_set_render_fail(gboolean b_fail);

/* Test seam: treat the GEGL op c_op (e.g. "gegl:png-load") as not
 * installed, so a unit test reaches the fallbacks a GEGL without it takes;
 * NULL restores the real answer. Not thread-safe: set it only while no
 * enhancer work is in flight. */
void enhancer_test_set_missing_op(const char *c_op);

/* Test seam: p_hook(p_data) runs at the start of every managed-path
 * attempt (the render, enhancer_load, the managed original), on the
 * calling thread -- a test cancels its GCancellable there to cancel "while
 * the check runs" deterministically. NULL removes it. Not thread-safe: set
 * it only while no enhancer work is in flight. */
typedef void (*EnhancerTestHook)(gpointer p_data);
void enhancer_test_set_load_hook(EnhancerTestHook p_hook, gpointer p_data);

/* Test seam: how many loader-path (non-managed) decodes this process has
 * started, so a test can tell "declined and decoded anyway" from
 * "declined, nothing decoded". */
guint enhancer_test_loader_decodes(void);

/* Distinct embedded profiles the managed path hands to babl per process
 * that may grow babl's tables (enhancer.c, "which profiles babl sees"):
 * babl's space and tone-curve tables hold 100 entries each, are never
 * freed, and a full space table crashes the next babl_space_from_icc().
 * Past the cap a new profile is declined (its file decodes on the loader
 * path, sRGB). A profile babl provably adds nothing for (an sRGB one, one
 * equivalent to a profile already counted) costs no slot, and one babl
 * would decline outright is never handed to it. */
#define GGAZE_ENHANCER_MAX_PROFILES 16u

/* The longest name babl gives a format: it names each format of a space
 * "<encoding>-<space name>" in a 256-byte buffer, cut at 254 characters
 * (babl-format.c, format_new_from_format_with_space). Two formats whose
 * names are cut to the same bytes are then one to babl's name lookups,
 * and its fish search between them spun forever in an uncancellable GEGL
 * decode (xb2 review 5: a 238-character space name from a 'para' curve at
 * -32767). So a managed space's name may be at most this, less the dash
 * and the longest encoding babl has registered
 * (enhancer_max_space_name). icc.c's parameter bounds keep real curves
 * well inside: a Rec. 709 'para' curve on all three channels, with the
 * tests' primaries, names its space in ~220 characters -- a few more in
 * babl 0.1.112, which puts two spaces after a curve's gamma, than in
 * 0.1.128; the exact length also depends on which curves babl already
 * holds (a formula curve is named by the first profile that made it). */
#define GGAZE_ENHANCER_FORMAT_NAME_MAX 254u

/* The longest space name a managed space may have: the test seam's limit
 * when one is set, else GGAZE_ENHANCER_FORMAT_NAME_MAX - 1 - the longest
 * encoding in babl's format table (24, "CIE LCH(ab) alpha double", with
 * babl's and GEGL's own formats: 229) as enhancer_babl_ready() read it. */
guint enhancer_max_space_name(void);

/* Read what the managed path needs from babl's tables once, now: call it
 * right after gegl_init(), on the same thread, before any GEGL work (app.c
 * does; so do the suites that init GEGL). babl's format table is walked
 * without babl's lock, which is safe only while no GEGL worker can add
 * formats to it. Idempotent. Should it never be called, the first verdict
 * reads the table instead, from whichever thread asks. */
void enhancer_babl_ready(void);

/* babl's walk over its format table (babl-internal.h). Every babl this
 * builds with exports it (0.1.112 to 0.1.128 at least) but its public
 * header does not declare it; p_each returns 0 to go on. The one
 * declaration, for enhancer.c and the suites. */
void babl_format_class_for_each(int (*p_each)(Babl *, void *), void *p_data);

/* How many slot-free verdicts (above) are kept, the oldest dropped first:
 * with the slots' own, the verdict table never holds more than
 * GGAZE_ENHANCER_MAX_PROFILES + this many profiles. */
#define GGAZE_ENHANCER_MAX_FREE_VERDICTS 64u

/* Test seam: the space the managed path would apply for the profile p_icc
 * (icc_babl_kind, the CMYK LCMS check, babl, not sRGB), through the same
 * per-process verdict table and cap as a file's profile; NULL when it
 * would not manage it. Lets a test convert pixels through that space. */
const Babl *enhancer_test_profile_space(GBytes *p_icc);

/* Test seam: enhancer_test_profile_space(p_icc) != NULL. */
gboolean enhancer_test_profile_is_managed(GBytes *p_icc);

/* Test seam: profiles counted against GGAZE_ENHANCER_MAX_PROFILES so far. */
guint enhancer_test_profile_slots(void);

/* Test seam: verdicts the table keeps right now (slots and slot-free). */
guint enhancer_test_profile_verdicts(void);

/* Test seam: the space-name limit the verdicts apply from now on (0: back
 * to babl's own, enhancer_max_space_name), so a test can put the limit at the
 * length babl's own name for a space has -- which differs between babl
 * versions -- and check both sides of it. Verdicts already kept stay. */
void enhancer_test_set_max_space_name(guint u_max);

G_END_DECLS

#endif /* GGAZE_HAVE_GEGL */

#endif /* GGAZE_ENHANCER_GEGL_H */