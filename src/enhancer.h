#ifndef GGAZE_ENHANCER_H
#define GGAZE_ENHANCER_H

/*:*
 * ggaze — enhance preset metadata (GEGL-agnostic)
 *
 * The preset list + the Enhancer engine's lifecycle/preset-accessors carry no
 * GEGL or GtkWidget dependency, so a non-GEGL consumer (e.g. a preferences UI
 * that only edits the preset list) can include just this header. The GEGL
 * buffer/texture operations live in enhancer-gegl.h (guarded on
 * GGAZE_HAVE_GEGL).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "ggaze-config.h"

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
   char *c_name;
   char *c_graph; /* GEGL graph string (for user presets) or NULL (built-in) */
   int   i_builtin; /* 1 if built-in (programmatic), 0 if user (graph text) */
} EnhancerPreset;

typedef struct Enhancer Enhancer;

Enhancer *enhancer_new(void);
void      enhancer_delete(Enhancer *p_e);

void enhancer_set_presets(Enhancer *p_e, const GPtrArray *p_presets);
const GPtrArray *enhancer_get_presets(Enhancer *p_e);

G_END_DECLS

#endif /* GGAZE_ENHANCER_H */