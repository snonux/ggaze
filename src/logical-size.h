#ifndef GGAZE_LOGICAL_SIZE_H
#define GGAZE_LOGICAL_SIZE_H

/*:*
 * ggaze — the image size a texture stands for (no GTK widgets)
 *
 * A texture normally depicts its image pixel for pixel. The live enhance
 * preview does not (8l2, decision #53): it is rendered from a scaled-down
 * source, so a 9248x6936 photo's preview texture is ~1500x1100 -- yet the
 * picture on screen IS the 9248x6936 image. Everything that measures the
 * picture must keep measuring the image: fit and 100 % zoom, the crop
 * rectangle and the straighten horizon (which tool-ctrl.c lays out and
 * reads in image pixels through the viewer's geometry), the base size a
 * crop lives on. So the render attaches the size of the image it depicts
 * to the texture, and the viewer lays the texture out at that size
 * (viewer.c: it draws the texture stretched over the image's rectangle);
 * nothing else needs to know the texture is smaller.
 *
 * The size rides on the texture as object data (GObject qdata, set once by
 * whoever makes the texture, before it is shared), so it cannot be
 * separated from the pixels it describes: a texture handed around the
 * window's choke point, cached, or compared by identity carries it along.
 * A texture without one -- every decoded original -- is its own size.
 *
 * Plain C over GdkTexture (no widget, no display): unit-tested in
 * tests/test_logical_size.c.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk/gdk.h>
#include <glib.h>

G_BEGIN_DECLS

/* Say that p_tex depicts an i_w x i_h image (each >= 1). Set it before the
 * texture is shared: the value is read without a lock by every consumer
 * afterwards. Setting the texture's own size removes the note. */
void logical_size_set(GdkTexture *p_tex, gint i_w, gint i_h);

/* The size of the image p_tex depicts: the one set above, else the
 * texture's own. Either out-parameter may be NULL. */
void logical_size_get(GdkTexture *p_tex, gint *p_w, gint *p_h);

/* TRUE iff p_tex depicts an image of another size than it holds (a
 * scaled-down preview). */
gboolean logical_size_is_scaled(GdkTexture *p_tex);

G_END_DECLS

#endif /* GGAZE_LOGICAL_SIZE_H */
