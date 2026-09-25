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

/* The preset list (ai2): the GGAZE_ENHANCE_N_BUILTINS built-ins, then the
 * user's own from the enhance-presets setting in Preferences order, all of
 * them rows of the edit panel. GGAZE_ENHANCE_MAX_PRESETS rows at most: the
 * layered mask is a guint32 (bit i: row i on), and every per-preset array
 * (the strengths, the snapshots undo keeps, the panel's widgets) is sized
 * by it. So at most GGAZE_ENHANCE_MAX_USER_PRESETS user presets are taken;
 * enhancer_set_user_presets ignores the rest and Preferences says so. 32
 * rows is far more than a panel is useful with -- the list scrolls past
 * the ten or so that fit -- and keeps the mask one machine word.
 *
 * Only the first GGAZE_ENHANCE_DIGIT_PRESETS rows have a digit (1-8, the
 * built-ins as long as they are the first eight); j / k / Enter / h / l
 * reach every row. */
#define GGAZE_ENHANCE_N_BUILTINS 8
#define GGAZE_ENHANCE_MAX_PRESETS 32
#define GGAZE_ENHANCE_MAX_USER_PRESETS                                         \
   (GGAZE_ENHANCE_MAX_PRESETS - GGAZE_ENHANCE_N_BUILTINS)
#define GGAZE_ENHANCE_DIGIT_PRESETS 8

/* The mask bit of row i (0 .. GGAZE_ENHANCE_MAX_PRESETS - 1). */
#define GGAZE_ENHANCE_BIT(i) ((guint32)1u << (guint)(i))

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

/* Replace the whole list (deep copy; rows past GGAZE_ENHANCE_MAX_PRESETS
 * are not taken, as no mask bit could reach them). */
void enhancer_set_presets(Enhancer *p_e, const GPtrArray *p_presets);
const GPtrArray *enhancer_get_presets(Enhancer *p_e);

/* Rebuild the list as the built-ins followed by the user presets from the
 * Preferences a(ss) list (SettingsPair* of name -> graph string), in that
 * order, the first GGAZE_ENHANCE_MAX_USER_PRESETS of them (the rest are
 * ignored: Preferences marks them). This is the merge policy in one
 * place: calling it again with the same list yields the same result (the
 * window used to append the user presets onto a list that already held
 * them, doubling them on every Preferences change). */
void enhancer_set_user_presets(Enhancer *p_e, const GPtrArray *p_pairs);

/* Comma-joined names of the presets enabled in u_mask (for the title), or
 * NULL when none is. Caller frees. enhancer_describe_state with every
 * strength at its default. */
char *enhancer_describe_mask(const GPtrArray *p_presets, guint32 u_mask);

/* Like enhancer_describe_mask, but a tunable preset whose strength
 * (pd_strength[i], GGAZE_ENHANCE_MAX_PRESETS of them; NULL: all at the
 * default) is not its default is named with it, "Brightness +0.6" -- so
 * the title tells two strengths of one preset apart, and at the defaults
 * reads exactly as before 8i2. Caller frees. */
char *enhancer_describe_state(const GPtrArray *p_presets, guint32 u_mask,
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

/* What the presets enabled in u_mask render, as text: each one's graph
 * with its strength written in (pd_strength as for describe_state), one
 * per line, in row order -- the chain the render and the export run. Two
 * states render the same pixels iff their keys are equal, whatever list
 * each is read against, which is how a Preferences change tells whether
 * the edit on screen (or the one saved) is still the same picture. NULL
 * when nothing is enabled. Caller frees. */
char *enhancer_chain_key(const GPtrArray *p_presets, guint32 u_mask,
                         const gdouble *pd_strength);

/* --- carrying an edit across a new preset list (ai2) ---------------------
 *
 * Preferences rewrites the user presets while an edit may have some of
 * them on (add, edit, move, remove -- one change per write, each a new
 * list). An edit refers to presets by ROW, so a change that moves rows
 * must carry the edit along, or a reorder would turn on the preset that
 * slid into an enabled row. enhancer_presets_map says where each old row
 * went; enhancer_state_remap moves a (mask, strengths) state through it. */

/* Where each row of p_old is in p_new: pi_map[i] for every row i <
 * MIN(p_old->len, GGAZE_ENHANCE_MAX_PRESETS), -1 when it is gone. A row is
 * matched, in three passes, to the first row of p_new not matched yet
 * that has (1) the same name and graph -- the preset untouched, wherever
 * it moved; else (2) the same name -- its graph edited; else (3) the same
 * graph -- renamed. So each single change Preferences makes carries every
 * preset it did not remove along; an edit of both name and graph at once
 * reads as a removal and an addition. The built-ins, identical in both
 * lists, keep their rows. TRUE iff the lists are the same (same length,
 * every row the same name and graph): nothing to carry. */
gboolean enhancer_presets_map(const GPtrArray *p_old, const GPtrArray *p_new,
                              gint *pi_map);

/* The state (u_mask, pd_old[]) read against p_old, carried to p_new
 * through pi_map (enhancer_presets_map): each preset keeps its on / off
 * bit and its strength in its new row -- the strength clamped into the
 * new graph's range (an edited placeholder), the new default where the
 * preset had none to keep. A preset that is gone loses both; a new row is
 * off at its default. Writes pd_new[0 .. GGAZE_ENHANCE_MAX_PRESETS - 1]
 * (must not alias pd_old) and returns the new mask. */
guint32 enhancer_state_remap(const GPtrArray *p_old, const GPtrArray *p_new,
                             const gint *pi_map, guint32 u_mask,
                             const gdouble *pd_old, gdouble *pd_new);

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