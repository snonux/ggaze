#ifndef GGAZE_ENHANCER_PRIVATE_H
#define GGAZE_ENHANCER_PRIVATE_H

/*:*
 * ggaze — what enhancer.c shares with enhancer-preview.c (GEGL builds)
 *
 * The live preview's source and render (enhancer-preview.c, 8l2) are
 * built from the same decode, chain and conversion the full-resolution
 * render and the export use (enhancer.c), so they cannot drift apart: one
 * load path (the managed decode or ggaze's loader), one chain builder, one
 * preset snapshot. Nothing outside the two files includes this header.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "enhancer-gegl.h"

#if GGAZE_HAVE_GEGL

G_BEGIN_DECLS

/* A deep copy of the first GGAZE_ENHANCE_MAX_PRESETS presets (a worker's
 * snapshot, safe against a concurrent enhancer_set_presets). */
GPtrArray *enhancer_priv_presets_copy(const GPtrArray *p_src);

/* *p_dst = *p_xf, or the identity for NULL. */
void enhancer_priv_snapshot_transform(Transform *p_dst, const Transform *p_xf);

/* The chain (presets in u_mask, then p_xf, which must be in p_in's pixels)
 * on p_in, whose pixel lengths are scaled by d_px_scale (1: the image at
 * full resolution). A new buffer, or NULL with p_err. */
GeglBuffer *enhancer_priv_run_chain(GeglBuffer      *p_in,
                                    const GPtrArray *p_presets, guint32 u_mask,
                                    const Transform *p_xf, gdouble d_px_scale,
                                    GError **p_err);

/* The managed decode of p_file (enhancer_load's upgrade path), or NULL
 * when it declines -- never an error of its own. */
GeglBuffer *enhancer_priv_load_managed(GFile *p_file, GCancellable *p_cancel);

/* ggaze's loader decode of p_file as an sRGB buffer (counted for
 * enhancer_test_loader_decodes), or NULL with p_err. */
GeglBuffer *enhancer_priv_load_via_loader(GFile *p_file, GCancellable *p_cancel,
                                          GError **p_err);

/* A decoded texture's pixels as the sRGB buffer the loader path makes, at
 * d_scale (1: a copy; below: scaled, never copied at full size). */
GeglBuffer *enhancer_priv_buffer_from_texture(GdkTexture *p_tex,
                                              gdouble     d_scale);

/* p_in scaled by d_scale (< 1) into a new buffer of its format. */
GeglBuffer *enhancer_priv_downscale(GeglBuffer *p_in, gdouble d_scale);

G_END_DECLS

#endif /* GGAZE_HAVE_GEGL */

#endif /* GGAZE_ENHANCER_PRIVATE_H */
