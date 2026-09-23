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
                                 guint8 u_mask, const Transform *p_xf,
                                 GError **p_err);

/* Export the enhanced buffer to a file. The saver is chosen from p_out's
 * extension: .jpg/.jpeg -> gegl:jpg-save (quality 95), .png -> gegl:png-save,
 * .webp -> gegl:webp-save (if available). Other extensions fail with
 * G_IO_ERROR_NOT_SUPPORTED. Success is verified by a real stat of the output
 * (not pre-existence). Colour (decision #45): the PNG and JPEG savers embed
 * the buffer's colour space as its ICC profile -- for a source with an
 * embedded profile, that profile byte for byte; WebP cannot carry one, and
 * its saver reads the pixels as sRGB (babl converts a buffer in another
 * space on the way out). An sRGB buffer exports exactly as before. Returns
 * TRUE on success. */
gboolean enhancer_export(GeglBuffer *p_in, const EnhancerPreset *p_preset,
                         GFile *p_out, GError **p_err);

/* Export p_in with the enabled-preset chain (u_mask) and the transform p_xf
 * (nullable) composed, to p_out. */
gboolean enhancer_export_chain(GeglBuffer *p_in, const GPtrArray *p_presets,
                               guint8 u_mask, const Transform *p_xf,
                               GFile *p_out, GError **p_err);

/* Async export: load p_src, apply the chain + transform, save to p_out --
 * all in a GTask worker, because the full-resolution decode + GEGL chain +
 * encode takes seconds on a 40 MP photo and used to freeze the UI
 * (AGENTS.md: decode runs in GTask threads). p_presets and p_xf (nullable)
 * are snapshotted. Finish returns TRUE on a real write. */
void     enhancer_export_chain_async(GFile *p_src, const GPtrArray *p_presets,
                                     guint8 u_mask, const Transform *p_xf,
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

/* Whether enhancer_load() will decode p_file colour-managed, as far as the
 * file's headers tell: a local PNG / JPEG (a JPEG only in a build with
 * the `jpeg` feature), GEGL's loader op installed, an embedded profile
 * babl parses to a space other than sRGB, for the image's number of
 * colour components. The completeness checks are NOT run -- a file whose
 * data turns out broken still falls back to the loader path -- so this is
 * the cheap answer for the info card's "managed on enhance/export" note,
 * not a promise. Thread-safe (the info card asks from a worker). */
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
                                guint8 u_mask, const Transform *p_xf,
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
 * unrefs); NULL WITHOUT an error when the file no longer decodes managed
 * (rewritten meanwhile: the plain original is then the right compare);
 * NULL with p_err set when the file does not load at all or the task was
 * cancelled. */
GdkTexture *enhancer_managed_original_finish(GAsyncResult *p_res,
                                             GError      **p_err);

/* Generate the max-512px original followed by up to eight independent preset
 * previews. The returned array owns its GdkTexture entries; index 0 is the
 * original and index i + 1 corresponds to preset i. An unsupported individual
 * preset is represented by NULL. */
void       enhancer_preview_thumbnails_async(GFile              *p_file,
                                             const GPtrArray    *p_presets,
                                             GCancellable       *p_cancel,
                                             GAsyncReadyCallback p_cb,
                                             gpointer            p_data);
GPtrArray *enhancer_preview_thumbnails_finish(GAsyncResult *p_res,
                                              GError      **p_err);

/* Test seam: treat the GEGL op c_op (e.g. "gegl:png-load") as not
 * installed, so a unit test reaches the fallbacks a GEGL without it takes;
 * NULL restores the real answer. Not thread-safe: set it only while no
 * enhancer work is in flight. */
void enhancer_test_set_missing_op(const char *c_op);

G_END_DECLS

#endif /* GGAZE_HAVE_GEGL */

#endif /* GGAZE_ENHANCER_GEGL_H */