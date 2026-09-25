#ifndef GGAZE_PRESET_STRENGTH_H
#define GGAZE_PRESET_STRENGTH_H

/*:*
 * ggaze — the tunable number of an enhance preset (plain C, no GTK/GEGL)
 *
 * An enhance preset is a GEGL graph string ("gegl:exposure exposure=0.5",
 * enhancer.c). Part 3 of the edit-mode redesign (8i2) lets ONE number in
 * it be tuned from the edit panel (h / l on the selected card, or its
 * slider): the graph marks that number with a placeholder that also
 * declares its default and range,
 *
 *     {s:DEFAULT:MIN..MAX}         step (MAX - MIN) / 20
 *     {s:DEFAULT:MIN..MAX:STEP}
 *
 * e.g. "gegl:exposure exposure={s:0.5:-2..2:0.1}". The range lives in the
 * graph string itself because the enhance-presets setting is an a(ss)
 * list of (name, graph): a separate field would change the schema type
 * (and orphan every stored list), and a second key would have to be kept
 * in step with every rename, reorder and removal. Inline, one preset stays
 * one string, and a graph without a placeholder -- every preset written
 * before 8i2 -- parses exactly as it always did: on / off only.
 *
 * Numbers are C-locale decimals ("0.5", never "0,5") both ways: parsed
 * with g_ascii_strtod and written with g_ascii_formatd, so a de_DE
 * LC_NUMERIC can neither break a stored preset nor put a comma into the
 * graph GEGL reads. A value is written with the fewest decimals that
 * represent the default, the range and the step exactly, trailing zeros
 * dropped -- so the default of "{s:1.3:0..2:0.05}" is written "1.3" and a
 * built-in preset at its default renders from the very string it had
 * before 8i2.
 *
 * Every value this module hands out is CANONICAL: clamped into the range
 * and equal to what parsing its own written text gives back. So two
 * strengths compare equal with == exactly when they render the same graph
 * (the saved / dirty rule, edit_snapshot_equal), and nudging up and back
 * down lands on the very number it started from.
 *
 * Malformed placeholders are rejected with a message saying what is
 * wrong (G_IO_ERROR_INVALID_ARGUMENT): a second placeholder, a stray
 * brace, an unknown name, a missing field, a number that is not one, a
 * number beyond +-1e15 or needing more than 6 decimals (the default, an
 * end, the step -- given or implied -- could not be written exactly), an
 * empty or inverted range, a default outside it, a step that is not
 * positive. Preferences shows the message while the preset is typed;
 * a render of a malformed graph fails with it.
 *
 * Compiled in every build (Preferences validates with it; the minimal
 * build simply never renders), unit-tested in tests/test_preset_strength.c.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <glib.h>

G_BEGIN_DECLS

/* The declared range of a tunable number. */
typedef struct {
   gdouble d_default;
   gdouble d_min;
   gdouble d_max;
   gdouble d_step;     /* one h / l press; > 0 */
   guint   u_decimals; /* decimals a value is written with (<= 6) */
} PresetStrength;

/* Check c_graph's placeholder. TRUE when the graph is well formed: then
 * *pb_tunable (nullable) says whether it has a placeholder, and p_out
 * (nullable) receives its range when it does. FALSE with p_err set when a
 * placeholder is malformed. */
gboolean preset_strength_parse(const char *c_graph, gboolean *pb_tunable,
                               PresetStrength *p_out, GError **p_err);

/* d_value clamped into the range and made canonical (see the header). A
 * NaN is the default. */
gdouble preset_strength_clamp(const PresetStrength *p_s, gdouble d_value);

/* d_value moved by i_steps steps (negative: down) on the grid through
 * the default that preset_strength_snap uses (from a value off that grid,
 * an end of the range, the first step lands on the nearest grid point
 * that way), clamped, canonical. */
gdouble preset_strength_nudge(const PresetStrength *p_s, gdouble d_value,
                              gint i_steps);

/* d_value moved to the nearest point of the step grid through the default
 * (so the default is always reachable), clamped, canonical: where a
 * dragged slider lands. */
gdouble preset_strength_snap(const PresetStrength *p_s, gdouble d_value);

/* The graph text of d_value: C locale, the range's decimals, no trailing
 * zeros, never "-0" ("0.5", "-0.3", "4"). Caller frees. */
char *preset_strength_format(const PresetStrength *p_s, gdouble d_value);

/* How the panel card and the title show d_value: preset_strength_format,
 * with an explicit "+" on a positive value when the range reaches below
 * zero, so an offset reads as one ("+0.6", "-0.3", "0"; "1.3" for a scale
 * that cannot go negative). Caller frees. */
char *preset_strength_label(const PresetStrength *p_s, gdouble d_value);

/* c_graph with its placeholder replaced by d_value (clamped; NaN: the
 * default) -- the graph GEGL runs. A graph without a placeholder comes
 * back unchanged. NULL with p_err set for a malformed placeholder. Caller
 * frees. */
char *preset_strength_substitute(const char *c_graph, gdouble d_value,
                                 GError **p_err);

G_END_DECLS

#endif /* GGAZE_PRESET_STRENGTH_H */
