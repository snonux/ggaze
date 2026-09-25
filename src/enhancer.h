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

#include "preset-strength.h"
#include "settings-pair.h"

G_BEGIN_DECLS

/* The most presets the UI can address: the layered mask is a guint8 and the
 * chooser binds 1-8. Entries past this are kept in the list (Preferences
 * shows them) but cannot be toggled. */
#define GGAZE_ENHANCE_MAX_PRESETS 8

/* One preset. Built-ins and user presets alike are GEGL graph strings --
 * "op:name prop=value ..." chains, see enhancer.c -- and either may mark
 * one number as tunable with a {s:DEFAULT:MIN..MAX[:STEP]} placeholder
 * (preset-strength.h, 8i2): b_tunable and t_strength are what parsing the
 * graph said when the preset was made. A graph whose placeholder is
 * malformed is kept, not tunable, and fails with the parse message when
 * rendered. */
typedef struct {
   char          *c_name;
   char          *c_graph;    /* the GEGL graph string (never NULL) */
   int            i_builtin;  /* 1 if one of BUILTINS[], 0 if the user's */
   gboolean       b_tunable;  /* the graph has a well-formed placeholder */
   PresetStrength t_strength; /* its range (valid iff b_tunable) */
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
 * NULL when none is. Caller frees. enhancer_describe_state with every
 * strength at its default. */
char *enhancer_describe_mask(const GPtrArray *p_presets, guint8 u_mask);

/* Like enhancer_describe_mask, but a tunable preset whose strength
 * (pd_strength[i], GGAZE_ENHANCE_MAX_PRESETS of them; NULL: all at the
 * default) is not its default is named with it, "Brightness +0.6" -- so
 * the title tells two strengths of one preset apart, and at the defaults
 * reads exactly as before 8i2. Caller frees. */
char *enhancer_describe_state(const GPtrArray *p_presets, guint8 u_mask,
                              const gdouble *pd_strength);

/* Each addressable preset's default strength into pd_out[0 ..
 * GGAZE_ENHANCE_MAX_PRESETS - 1] -- 0 for a preset without a tunable
 * number, or a slot with no preset. */
void enhancer_default_strengths(const GPtrArray *p_presets, gdouble *pd_out);

/* A deep copy of p_presets whose tunable presets (the first
 * GGAZE_ENHANCE_MAX_PRESETS) have their placeholder replaced by
 * pd_strength[i] (clamped) -- what the render and the export run, so the
 * GEGL chain never sees a placeholder and the worker gets a snapshot no
 * later strength change can race. pd_strength NULL: a plain copy (the
 * chain then writes each default in). A malformed graph is copied as it
 * is. Caller unrefs. */
GPtrArray *enhancer_presets_resolve(const GPtrArray *p_presets,
                                    const gdouble   *pd_strength);

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