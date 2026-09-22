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

#include <gio/gio.h>
#include <glib.h>

#include "settings-pair.h"

G_BEGIN_DECLS

/* The most presets the UI can address: the layered mask is a guint8 and the
 * chooser binds 1-8. Entries past this are kept in the list (Preferences
 * shows them) but cannot be toggled. */
#define GGAZE_ENHANCE_MAX_PRESETS 8

typedef struct {
   char *c_name;
   char *c_graph; /* GEGL graph string (user presets) or NULL (built-in);
                   * "op:name prop=value ..." chains, see enhancer.c */
   int i_builtin; /* 1 if built-in (programmatic), 0 if user (graph text) */
} EnhancerPreset;

typedef struct Enhancer Enhancer;

/* A fresh engine holding the built-in presets. */
Enhancer *enhancer_new(void);
void      enhancer_delete(Enhancer *p_e);

/* Replace the whole list (deep copy). */
void enhancer_set_presets(Enhancer *p_e, const GPtrArray *p_presets);
const GPtrArray *enhancer_get_presets(Enhancer *p_e);

/* Rebuild the list as the built-ins followed by the user presets from the
 * Preferences a(ss) list (SettingsPair* of name -> graph string). This is
 * the merge policy in one place: calling it again with the same list yields
 * the same result (the window used to append the user presets onto a list
 * that already held them, doubling them on every Preferences change). */
void enhancer_set_user_presets(Enhancer *p_e, const GPtrArray *p_pairs);

/* Comma-joined names of the presets enabled in u_mask (for the title), or
 * NULL when none is. Caller frees. */
char *enhancer_describe_mask(const GPtrArray *p_presets, guint8 u_mask);

/* The non-colliding export destination for p_src in its own folder:
 * "<stem>-enhanced<ext>", then "<stem>-enhanced-<n><ext>", where <ext> is
 * p_src's extension when the saver supports it (.jpg/.jpeg/.png/.webp) and
 * ".jpg" otherwise (docs/gegl.md "defaults to the original format"). Never
 * names an existing file. Caller unrefs. */
GFile *enhancer_export_dest_for(GFile *p_src);

/* TRUE iff the saver can write files with this extension (".jpg", ...). */
gboolean enhancer_ext_supported(const char *c_ext);

G_END_DECLS

#endif /* GGAZE_ENHANCER_H */