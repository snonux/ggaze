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
 * embedded profile, that profile byte for byte; WebP cannot carry one, so
 * a non-sRGB buffer is converted to sRGB before that saver. An sRGB buffer
 * exports exactly as before. Returns TRUE on success. */
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
 * whose babl format carries the image's colour space (decision #45): a PNG
 * or JPEG with an embedded ICC profile decodes through GEGL's ICC-aware
 * gegl:png-load / gegl:jpg-load, which tag the buffer with the profile's
 * space (sRGB when babl cannot use it), behind the loader's own decode gate
 * (empty / truncated / oversized refusals, G_IO_ERROR_INVALID_DATA) and
 * with the orientation applied here; every other file (untagged PNG / JPEG
 * included) goes through ggaze's orientation-aware loader and is tagged
 * sRGB, exactly as before. Returns a new
 * buffer (caller unrefs) or NULL with p_err set. */
GeglBuffer *enhancer_load(GFile *p_file, GError **p_err);

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
 * started yet -- a caller that needs last-write-wins semantics (e.g. a newer
 * apply superseding this one) must still check that on its own before using
 * the finished result. */
void enhancer_apply_chain_async(GFile *p_file, const GPtrArray *p_presets,
                                guint8 u_mask, const Transform *p_xf,
                                GCancellable       *p_cancel,
                                GAsyncReadyCallback p_cb, gpointer p_data);

/* Finish enhancer_apply_chain_async(). Returns a new GdkTexture (caller
 * unrefs) or NULL with p_err set. *p_orig_w / *p_orig_h (each nullable)
 * receive the ORIGINAL image's upright size, the number the transform's base
 * size is computed from (transform_base_size); the controller records it so
 * the crop tool and a later quarter turn know the image they work on without
 * a second decode. */
GdkTexture *enhancer_apply_chain_finish(GAsyncResult *p_res, gint *p_orig_w,
                                        gint *p_orig_h, GError **p_err);

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

G_END_DECLS

#endif /* GGAZE_HAVE_GEGL */

#endif /* GGAZE_ENHANCER_GEGL_H */