#ifndef GGAZE_ENHANCER_GEGL_H
#define GGAZE_ENHANCER_GEGL_H

/*:*
 * ggaze — GEGL enhance operations (feature-gated)
 *
 * The GeglBuffer/GdkTexture operations on top of the Enhancer preset engine.
 * Guarded on GGAZE_HAVE_GEGL: the whole header is empty in a non-GEGL build,
 * so it can be included unconditionally alongside enhancer.h without pulling
 * <gegl.h> into a build that does not have GEGL. See enhancer.h for the
 * GEGL-agnostic preset metadata.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "enhancer.h"

#if GGAZE_HAVE_GEGL

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <gegl.h>

G_BEGIN_DECLS

/* Apply a preset to a GeglBuffer (returns a new buffer, or NULL on error). */
GeglBuffer *enhancer_apply(Enhancer *p_e, GeglBuffer *p_in,
                           const EnhancerPreset *p_preset, GError **p_err);

/* Apply a chain of the enabled built-in presets (bit i of u_mask -> preset i)
 * in array order, composing them. Returns a new buffer, or NULL if none are
 * enabled / on error. */
GeglBuffer *enhancer_apply_chain(Enhancer *p_e, GeglBuffer *p_in,
                                 const GPtrArray *p_presets, guint8 u_mask,
                                 GError **p_err);

/* Export the enhanced buffer to a file. The saver is chosen from p_out's
 * extension: .jpg/.jpeg -> gegl:jpg-save (quality 95), .png -> gegl:png-save,
 * .webp -> gegl:webp-save (if available). Other extensions fail with
 * G_IO_ERROR_NOT_SUPPORTED. Success is verified by a real stat of the output
 * (not pre-existence). Returns TRUE on success. */
gboolean enhancer_export(Enhancer *p_e, GeglBuffer *p_in,
                         const EnhancerPreset *p_preset, GFile *p_out,
                         GError **p_err);

/* Export p_in with the enabled-preset chain (u_mask) composed, to p_out. */
gboolean enhancer_export_chain(Enhancer *p_e, GeglBuffer *p_in,
                               const GPtrArray *p_presets, guint8 u_mask,
                               GFile *p_out, GError **p_err);

/* Load a file into a GeglBuffer, upright (EXIF Orientation applied). Loads
 * through ggaze's own orientation-aware loader (not gegl:load, which does
 * not auto-rotate) and copies the upright RGBA8 pixels into a GeglBuffer.
 * Returns a new buffer (caller unrefs) or NULL with p_err set. */
GeglBuffer *enhancer_load(GFile *p_file, GError **p_err);

/* Convert a GeglBuffer to a GdkTexture for preview (RGBA8 bytes). Returns a
 * new GdkTexture (caller unrefs) or NULL with p_err set. Needs no display. */
GdkTexture *enhancer_buffer_to_texture(GeglBuffer *p_buf, GError **p_err);

/* Async: load p_file, apply the enabled-preset chain (u_mask), and convert
 * the result to a GdkTexture, all off the calling thread (a GTask worker) so
 * a caller with a main loop (e.g. the window) is never blocked by GEGL's
 * CPU-heavy processing (tu0). p_presets is snapshotted internally before the
 * worker starts, so a concurrent enhancer_set_presets() (Preferences apply)
 * cannot race it. p_cancel may be NULL. Since GEGL processing itself cannot
 * be interrupted mid-flight, cancellation only skips work that has not
 * started yet -- a caller that needs last-write-wins semantics (e.g. a newer
 * apply superseding this one) must still check that on its own before using
 * the finished result. */
void enhancer_apply_chain_async(Enhancer *p_e, GFile *p_file,
                                const GPtrArray *p_presets, guint8 u_mask,
                                GCancellable       *p_cancel,
                                GAsyncReadyCallback p_cb, gpointer p_data);

/* Finish enhancer_apply_chain_async(). Returns a new GdkTexture (caller
 * unrefs) or NULL with p_err set. */
GdkTexture *enhancer_apply_chain_finish(GAsyncResult *p_res, GError **p_err);

/* Generate the max-512px original followed by up to eight independent preset
 * previews. The returned array owns its GdkTexture entries; index 0 is the
 * original and index i + 1 corresponds to preset i. An unsupported individual
 * preset is represented by NULL. */
void       enhancer_preview_thumbnails_async(Enhancer *p_e, GFile *p_file,
                                             const GPtrArray    *p_presets,
                                             GCancellable       *p_cancel,
                                             GAsyncReadyCallback p_cb,
                                             gpointer            p_data);
GPtrArray *enhancer_preview_thumbnails_finish(GAsyncResult *p_res,
                                              GError      **p_err);

G_END_DECLS

#endif /* GGAZE_HAVE_GEGL */

#endif /* GGAZE_ENHANCER_GEGL_H */