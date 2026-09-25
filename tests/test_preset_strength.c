/*:*
 * ggaze — preset-strength unit tests (plain C, no display)
 *
 * The tunable-number placeholder of an enhance preset (8i2): parsing
 * "{s:DEFAULT:MIN..MAX[:STEP]}", substituting a value into the graph,
 * clamping, stepping and snapping to canonical values, the locale-free
 * number text (also under a comma-decimal LC_NUMERIC when the machine has
 * one), and every malformed placeholder rejected with its message.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "preset-strength.h"

#include <gio/gio.h>
#include <glib.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>

/* Parse c_graph, asserting it is tunable, and return its range. */
static PresetStrength
parse_ok(const char *c_graph) {
   PresetStrength t_s       = {0};
   gboolean       b_tunable = FALSE;
   GError        *p_err     = NULL;
   g_assert_true(preset_strength_parse(c_graph, &b_tunable, &t_s, &p_err));
   g_assert_no_error(p_err);
   g_assert_true(b_tunable);
   return (t_s);
}

/* substitute(c_graph, d_value) == c_want. */
static void
assert_subst(const char *c_graph, gdouble d_value, const char *c_want) {
   GError *p_err = NULL;
   char   *c_got = preset_strength_substitute(c_graph, d_value, &p_err);
   g_assert_no_error(p_err);
   g_assert_cmpstr(c_got, ==, c_want);
   g_free(c_got);
}

/* A graph without a placeholder -- every preset before 8i2 -- is well
 * formed, not tunable, and substitutes to itself. */
static void
test_plain_graph(void) {
   gboolean       b_tunable = TRUE;
   PresetStrength t_s       = {.d_default = 42.0};
   g_assert_true(preset_strength_parse("gegl:saturation scale=1.3", &b_tunable,
                                       &t_s, NULL));
   g_assert_false(b_tunable);
   g_assert_cmpfloat(t_s.d_default, ==, 42.0); /* untouched */
   g_assert_true(
      preset_strength_parse("gegl:stretch-contrast", NULL, NULL, NULL));
   assert_subst("gegl:saturation scale=1.3", 7.0, "gegl:saturation scale=1.3");
}

/* The two forms: an explicit step, and the default twentieth of the
 * range; the decimals cover the default, the range and the step. */
static void
test_parse_fields(void) {
   PresetStrength t_s = parse_ok("gegl:exposure exposure={s:0.5:-2..2:0.1}");
   g_assert_cmpfloat(t_s.d_default, ==, 0.5);
   g_assert_cmpfloat(t_s.d_min, ==, -2.0);
   g_assert_cmpfloat(t_s.d_max, ==, 2.0);
   g_assert_cmpfloat(t_s.d_step, ==, 0.1);
   g_assert_cmpuint(t_s.u_decimals, ==, 1);
   t_s = parse_ok("gegl:saturation scale={s:1.4:0..2}");
   g_assert_cmpfloat_with_epsilon(t_s.d_step, 0.1, 1e-12);
   g_assert_cmpuint(t_s.u_decimals, ==, 1);
   t_s = parse_ok("a={s:1.3:0..2:0.05}");
   g_assert_cmpuint(t_s.u_decimals, ==, 2);
   t_s = parse_ok("gegl:noise-reduction iterations={s:4:1..16:1}");
   g_assert_cmpuint(t_s.u_decimals, ==, 0);
   t_s = parse_ok("x={s:0.123456:0..1:0.5}");
   g_assert_cmpuint(t_s.u_decimals, ==, 6); /* the most there may be */
   t_s = parse_ok("x={s:0:-1e15..1e15:1e14}");
   g_assert_cmpfloat(t_s.d_max, ==, 1e15); /* the largest there may be */
   /* NULL outs are fine. */
   g_assert_true(preset_strength_parse("a={s:1:0..2}", NULL, NULL, NULL));
}

/* Substitution writes the default exactly as the graph had it before 8i2
 * (no trailing zeros), clamps, and a NaN means the default. */
static void
test_substitute(void) {
   const char *c_g = "gegl:exposure exposure={s:0.5:-2..2:0.1} gegl:x";
   assert_subst(c_g, NAN, "gegl:exposure exposure=0.5 gegl:x");
   assert_subst(c_g, 0.6, "gegl:exposure exposure=0.6 gegl:x");
   assert_subst(c_g, -0.3, "gegl:exposure exposure=-0.3 gegl:x");
   assert_subst(c_g, 9.0, "gegl:exposure exposure=2 gegl:x");
   assert_subst(c_g, -9.0, "gegl:exposure exposure=-2 gegl:x");
   assert_subst(c_g, -0.0001, "gegl:exposure exposure=0 gegl:x"); /* not -0 */
   assert_subst("c={s:1.3:0..2:0.05}", NAN, "c=1.3");
   assert_subst("c={s:1.3:0..2:0.05}", 1.35, "c=1.35");
   assert_subst("{s:4:1..16:1}", NAN, "4"); /* the whole graph */
   /* A malformed one fails with the error. */
   GError *p_err = NULL;
   g_assert_null(preset_strength_substitute("a={s:x:0..1}", 0.5, &p_err));
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
   g_clear_error(&p_err);
}

/* Stepping is exact and reversible (canonical values), stops at the ends,
 * and a slider value snaps onto the grid through the default. */
static void
test_nudge_clamp_snap(void) {
   PresetStrength t_s = parse_ok("e={s:0.5:-2..2:0.1}");
   gdouble        d_v = t_s.d_default;
   for (gint i = 0; i < 7; i++) {
      d_v = preset_strength_nudge(&t_s, d_v, 1);
   }
   g_assert_cmpfloat(d_v, ==, 1.2);
   for (gint i = 0; i < 7; i++) {
      d_v = preset_strength_nudge(&t_s, d_v, -1);
   }
   g_assert_cmpfloat(d_v, ==, 0.5); /* == the parsed default, exactly */
   g_assert_cmpfloat(preset_strength_nudge(&t_s, 1.95, 5), ==, 2.0);
   g_assert_cmpfloat(preset_strength_nudge(&t_s, -2.0, -1), ==, -2.0);
   g_assert_cmpfloat(preset_strength_nudge(&t_s, 0.5, -8), ==, -0.3);
   g_assert_cmpfloat(preset_strength_clamp(&t_s, NAN), ==, 0.5);
   g_assert_cmpfloat(preset_strength_clamp(&t_s, 0.54321), ==, 0.5);
   g_assert_cmpfloat(preset_strength_snap(&t_s, 0.66), ==, 0.7);
   g_assert_cmpfloat(preset_strength_snap(&t_s, 7.0), ==, 2.0);
   g_assert_cmpfloat(preset_strength_snap(&t_s, NAN), ==, 0.5);
   /* The grid runs through the default, not through MIN. */
   PresetStrength t_c = parse_ok("c={s:1.3:0..2:0.25}");
   g_assert_cmpfloat(preset_strength_snap(&t_c, 1.4), ==, 1.3);
   g_assert_cmpfloat(preset_strength_snap(&t_c, 1.5), ==, 1.55);
}

/* A key steps on the very grid the slider snaps to (the one through the
 * default), so both agree on every value between the ends: from the
 * maximum of {s:0.3:0..1:0.25} (1, off the grid) one step down is the grid
 * point 0.8 the slider snaps to -- not 0.75 -- and from the minimum 0 one
 * step up is 0.05, the first grid point, none skipped. */
static void
test_nudge_on_the_slider_grid(void) {
   PresetStrength t_s   = parse_ok("s={s:0.3:0..1:0.25}");
   gdouble        d_max = preset_strength_nudge(&t_s, 0.3, 5);
   g_assert_cmpfloat(d_max, ==, 1.0);
   gdouble d_down = preset_strength_nudge(&t_s, d_max, -1);
   g_assert_cmpfloat(d_down, ==, preset_strength_snap(&t_s, 0.8));
   g_assert_cmpfloat(d_down, ==, 0.8);
   g_assert_cmpfloat(preset_strength_nudge(&t_s, d_down, -1), ==, 0.55);
   g_assert_cmpfloat(preset_strength_nudge(&t_s, d_down, 1), ==, 1.0);
   g_assert_cmpfloat(preset_strength_nudge(&t_s, 0.0, 1), ==, 0.05);
   g_assert_cmpfloat(preset_strength_nudge(&t_s, 0.05, -1), ==, 0.0);
   /* A zero step count only snaps. */
   g_assert_cmpfloat(preset_strength_nudge(&t_s, 0.7, 0), ==, 0.8);
}

/* The card / title label: signed when the range reaches below zero. */
static void
test_label(void) {
   PresetStrength t_e = parse_ok("e={s:0.5:-2..2:0.1}");
   PresetStrength t_c = parse_ok("c={s:1.3:0..2:0.05}");
   const struct {
      const PresetStrength *p_s;
      gdouble               d_v;
      const char           *c_want;
   } CASES[] = {
      {&t_e, 0.6, "+0.6"}, {&t_e, -0.3, "-0.3"}, {&t_e, 0.0, "0"},
      {&t_c, 1.3, "1.3"},  {&t_c, 0.0, "0"},     {&t_c, 2.0, "2"},
   };
   for (gsize u = 0; u < G_N_ELEMENTS(CASES); u++) {
      char *c_got = preset_strength_label(CASES[u].p_s, CASES[u].d_v);
      g_assert_cmpstr(c_got, ==, CASES[u].c_want);
      g_free(c_got);
   }
}

/* Every malformed placeholder, and what its message must name. */
static const struct {
   const char *c_graph;
   const char *c_why;
} MALFORMED[] = {
   {"a={s}", "is not {s:DEFAULT"},
   {"a={s:1}", "is not {s:DEFAULT"},
   {"a={s:1:0..2:0.1:9}", "is not {s:DEFAULT"},
   {"a={t:1:0..2}", "is not {s:DEFAULT"},
   {"a={s:1:0..2", "no '}' closes"},
   {"a=s:1:0..2}", "a '}' without a '{'"},
   {"a=} b={s:1:0..2}", "a '}' without a '{'"},
   {"a={s:1:0..2} b={s:1:0..2}", "only one tunable number"},
   {"a={s:{s:1:0..2}}", "only one tunable number"},
   {"a={s:x:0..2}", "default 'x' is not a number"},
   {"a={s:1,5:0..2}", "default '1,5' is not a number"},
   {"a={s: 1:0..2}", "default ' 1' is not a number"},
   {"a={s:nan:0..2}", "is not a number"},
   {"a={s:1:0-2}", "range '0-2' is not MIN..MAX"},
   {"a={s:1:..2}", "range '..2' is not MIN..MAX"},
   {"a={s:1:0..inf}", "is not MIN..MAX"},
   {"a={s:1:2..0}", "the range is empty"},
   {"a={s:1:1..1}", "the range is empty"},
   {"a={s:3:0..2}", "the default lies outside"},
   {"a={s:1:0..2:0}", "the step must be above 0"},
   {"a={s:1:0..2:-1}", "the step must be above 0"},
   {"a={s:1:0..2:3}", "the step must be above 0"},
   {"a={s:1:0..2:y}", "step 'y' is not a number"},
   /* Numbers 6 decimals cannot write, or too large to write at all. */
   {"a={s:1e-7:0..1e-6:1e-7}", "needs more than 6 decimals"},
   {"a={s:0.1234567:0..1:0.5}", "needs more than 6 decimals"},
   {"a={s:0:0..1:1e-300}", "needs more than 6 decimals (the step"},
   {"a={s:0:0..0.00001}", "needs more than 6 decimals (the step"},
   {"a={s:0:0..1e40}", "is too large"},
   /* a double cannot step 1e12 by 1e-6: capped by the step count */
   {"a={s:0:0..1e15:0.000001}", "more than 1e6 steps"},
   {"a={s:0:-1e16..1}", "is too large"},
};

/* Every malformed placeholder is refused, and the message names what is
 * wrong. */
static void
test_malformed(void) {
   for (gsize u = 0; u < G_N_ELEMENTS(MALFORMED); u++) {
      GError  *p_err     = NULL;
      gboolean b_tunable = TRUE;
      if (preset_strength_parse(MALFORMED[u].c_graph, &b_tunable, NULL,
                                &p_err)) {
         g_error("\"%s\" parsed", MALFORMED[u].c_graph);
      }
      g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
      if (g_strstr_len(p_err->message, -1, MALFORMED[u].c_why) == NULL) {
         g_error("\"%s\": \"%s\"", MALFORMED[u].c_graph, p_err->message);
      }
      g_clear_error(&p_err);
      g_assert_null(
         preset_strength_substitute(MALFORMED[u].c_graph, 1.0, &p_err));
      g_assert_nonnull(p_err);
      g_clear_error(&p_err);
   }
}

/* A comma-decimal LC_NUMERIC changes nothing: the graph is read and
 * written in the C locale's "0.5". Skipped where no such locale is
 * installed. */
static void
test_locale_independent(void) {
   const char *c_loc = setlocale(LC_NUMERIC, "de_DE.UTF-8");
   if (c_loc == NULL) {
      c_loc = setlocale(LC_NUMERIC, "de_DE.utf8");
   }
   if (c_loc == NULL) {
      g_test_skip("no de_DE locale installed");
      return;
   }
   char c_probe[16];
   g_snprintf(c_probe, sizeof(c_probe), "%.1f", 0.5);
   g_assert_cmpstr(c_probe, ==, "0,5"); /* the locale really is in effect */
   PresetStrength t_s = parse_ok("e={s:0.5:-2..2:0.1}");
   g_assert_cmpfloat(t_s.d_default, ==, 0.5);
   assert_subst("e={s:0.5:-2..2:0.1}", 0.7, "e=0.7");
   char *c_label = preset_strength_label(&t_s, -1.2);
   g_assert_cmpstr(c_label, ==, "-1.2");
   g_free(c_label);
   GError *p_err = NULL;
   g_assert_false(preset_strength_parse("e={s:0,5:-2..2}", NULL, NULL, &p_err));
   g_clear_error(&p_err);
   setlocale(LC_NUMERIC, "C");
}

int
main(int argc, char **argv) {
   g_test_init(&argc, &argv, NULL);
   g_test_add_func("/preset_strength/plain_graph", test_plain_graph);
   g_test_add_func("/preset_strength/parse_fields", test_parse_fields);
   g_test_add_func("/preset_strength/substitute", test_substitute);
   g_test_add_func("/preset_strength/nudge_clamp_snap", test_nudge_clamp_snap);
   g_test_add_func("/preset_strength/nudge_on_the_slider_grid",
                   test_nudge_on_the_slider_grid);
   g_test_add_func("/preset_strength/label", test_label);
   g_test_add_func("/preset_strength/malformed", test_malformed);
   g_test_add_func("/preset_strength/locale_independent",
                   test_locale_independent);
   return (g_test_run());
}
