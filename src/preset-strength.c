/*:*
 * ggaze — the tunable number of an enhance preset (plain C, no GTK/GEGL)
 *
 * See preset-strength.h. The placeholder is found by its braces (GEGL
 * graph strings use none), its fields are split on ':' and the range on
 * "..", and every number goes through g_ascii_strtod / g_ascii_formatd so
 * the process locale never enters the graph text.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "preset-strength.h"

#include <gio/gio.h>
#include <glib.h>
#include <math.h>
#include <string.h>

/* The most decimals a value is written with: finer than any GEGL property
 * a preset tunes needs, coarse enough that a sum of steps never shows its
 * binary rounding error. A range or step needing more is refused (_check):
 * written at 6 decimals it would round to other numbers, or to 0. */
#define _MAX_DECIMALS 6u

/* The largest magnitude a range may reach: its values are then written in
 * at most 16 integer digits plus _MAX_DECIMALS, well inside the
 * G_ASCII_DTOSTR_BUF_SIZE buffer of preset_strength_format. The magnitude
 * alone does not keep a step representable, though: 1e12 + 0.000001 is
 * 1e12 again in a double, so a nudge would stall mid-range. Hence also: */
#define _MAX_MAGNITUDE 1e15

/* The most steps a range may hold. A key press or slider notch per step,
 * so no sane preset gets near it, and with it (max - min) / step stays far
 * below the 2^52 at which a double stops telling a value from value+step. */
#define _MAX_STEPS 1e6

/* The syntax, for the error messages. */
#define _SYNTAX "{s:DEFAULT:MIN..MAX} or {s:DEFAULT:MIN..MAX:STEP}"

/* Set the printf-style message as a G_IO_ERROR_INVALID_ARGUMENT; FALSE. */
#define _FAIL(p_err, ...)                                                      \
   (g_set_error((p_err), G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,              \
                "strength placeholder: " __VA_ARGS__),                         \
    FALSE)

/* Parse c_text (exactly, nothing before or after it) as a finite C-locale
 * number. */
static gboolean
_number(const char *c_text, gdouble *pd_out) {
   if (*c_text == '\0' || g_ascii_isspace(*c_text)) {
      return (FALSE);
   }
   char   *c_end = NULL;
   gdouble d     = g_ascii_strtod(c_text, &c_end);
   if (c_end == NULL || *c_end != '\0' || !isfinite(d)) {
      return (FALSE);
   }
   *pd_out = d;
   return (TRUE);
}

/* The fewest decimals that write d exactly, or _MAX_DECIMALS + 1 when
 * even _MAX_DECIMALS do not. A non-zero number that would be written as
 * 0 is not written exactly, however close to it (1e-300 is no 0). */
static guint
_decimals(gdouble d) {
   gdouble d_scaled = fabs(d);
   for (guint u = 0; u <= _MAX_DECIMALS; u++) {
      gdouble d_round = round(d_scaled);
      if ((d_round != 0.0 || d == 0.0) &&
          fabs(d_scaled - d_round) < 1e-9 * fmax(1.0, d_scaled)) {
         return (u);
      }
      d_scaled *= 10.0;
   }
   return (_MAX_DECIMALS + 1);
}

/* Find the one placeholder: *pc_open at its '{' and *pc_close at its '}',
 * both NULL when the graph has none. FALSE for a stray or second brace. */
static gboolean
_find(const char *c_graph, const char **pc_open, const char **pc_close,
      GError **p_err) {
   const char *c_open  = strchr(c_graph, '{');
   const char *c_brace = strchr(c_graph, '}');
   *pc_open            = NULL;
   *pc_close           = NULL;
   if (c_open == NULL) {
      return (c_brace == NULL ? TRUE : _FAIL(p_err, "a '}' without a '{'"));
   }
   if (c_brace == NULL) {
      return (_FAIL(p_err, "no '}' closes the '{'"));
   }
   if (c_brace < c_open) {
      return (_FAIL(p_err, "a '}' without a '{'"));
   }
   const char *c_next = strchr(c_open + 1, '{');
   if ((c_next != NULL && c_next < c_brace) ||
       strpbrk(c_brace + 1, "{}") != NULL) {
      return (_FAIL(p_err, "only one tunable number per preset"));
   }
   *pc_open  = c_open;
   *pc_close = c_brace;
   return (TRUE);
}

/* "MIN..MAX" into p_s's range. */
static gboolean
_range(const char *c_text, PresetStrength *p_s, GError **p_err) {
   const char *c_dots = strstr(c_text, "..");
   gboolean    b_ok   = c_dots != NULL;
   if (b_ok) {
      char *c_min = g_strndup(c_text, (gsize)(c_dots - c_text));
      b_ok = _number(c_min, &p_s->d_min) && _number(c_dots + 2, &p_s->d_max);
      g_free(c_min);
   }
   if (!b_ok) {
      return (_FAIL(p_err, "range '%s' is not MIN..MAX", c_text));
   }
   return (TRUE);
}

/* The number rules: every number of the range within +-_MAX_MAGNITUDE,
 * and written exactly in at most _MAX_DECIMALS decimals. */
static gboolean
_check_numbers(const PresetStrength *p_s, GError **p_err) {
   const gdouble d_nums[] = {p_s->d_default, p_s->d_min, p_s->d_max,
                             p_s->d_step};
   for (guint u = 0; u < G_N_ELEMENTS(d_nums); u++) {
      if (fabs(d_nums[u]) > _MAX_MAGNITUDE) {
         return (_FAIL(p_err, "%g is too large (at most 1e15 either way)",
                       d_nums[u]));
      }
      if (_decimals(d_nums[u]) > _MAX_DECIMALS) {
         return (_FAIL(p_err, "%g needs more than %u decimals%s", d_nums[u],
                       _MAX_DECIMALS,
                       u == 3 ? " (the step; without one it is a "
                                "twentieth of the range)"
                              : ""));
      }
   }
   if (p_s->d_step > 0.0 &&
       (p_s->d_max - p_s->d_min) / p_s->d_step > _MAX_STEPS) {
      return (_FAIL(p_err, "the range holds more than 1e6 steps of %g",
                    p_s->d_step));
   }
   return (TRUE);
}

/* The rules a parsed range must keep (see the header). */
static gboolean
_check(const PresetStrength *p_s, GError **p_err) {
   if (!_check_numbers(p_s, p_err)) {
      return (FALSE);
   }
   if (!(p_s->d_min < p_s->d_max)) {
      return (_FAIL(p_err, "the range is empty (MIN must be below MAX)"));
   }
   if (p_s->d_default < p_s->d_min || p_s->d_default > p_s->d_max) {
      return (_FAIL(p_err, "the default lies outside MIN..MAX"));
   }
   if (!(p_s->d_step > 0.0) || p_s->d_step > p_s->d_max - p_s->d_min) {
      return (_FAIL(p_err, "the step must be above 0 and within the range"));
   }
   return (TRUE);
}

/* The inside of a placeholder, "s:DEFAULT:MIN..MAX[:STEP]", into p_s. */
static gboolean
_fields(const char *c_inner, PresetStrength *p_s, GError **p_err) {
   if (!g_str_has_prefix(c_inner, "s:")) {
      return (_FAIL(p_err, "'{%s}' is not " _SYNTAX, c_inner));
   }
   char   **pp_f = g_strsplit(c_inner + 2, ":", -1);
   guint    u_n  = g_strv_length(pp_f);
   gboolean b_ok = u_n == 2 || u_n == 3;
   if (!b_ok) {
      b_ok = _FAIL(p_err, "'{%s}' is not " _SYNTAX, c_inner);
   } else if (!_number(pp_f[0], &p_s->d_default)) {
      b_ok = _FAIL(p_err, "default '%s' is not a number", pp_f[0]);
   } else if (!_range(pp_f[1], p_s, p_err)) {
      b_ok = FALSE;
   } else if (u_n == 3 && !_number(pp_f[2], &p_s->d_step)) {
      b_ok = _FAIL(p_err, "step '%s' is not a number", pp_f[2]);
   } else if (u_n == 2) {
      p_s->d_step = (p_s->d_max - p_s->d_min) / 20.0;
   }
   g_strfreev(pp_f);
   return (b_ok && _check(p_s, p_err));
}

/* _find + _fields: the placeholder's span (NULL, NULL for none) and its
 * range, decimals included. */
static gboolean
_parse(const char *c_graph, const char **pc_open, const char **pc_close,
       PresetStrength *p_s, GError **p_err) {
   if (!_find(c_graph, pc_open, pc_close, p_err)) {
      return (FALSE);
   }
   if (*pc_open == NULL) {
      return (TRUE);
   }
   char *c_inner = g_strndup(*pc_open + 1, (gsize)(*pc_close - (*pc_open + 1)));
   gboolean b_ok = _fields(c_inner, p_s, p_err);
   g_free(c_inner);
   if (b_ok) {
      p_s->u_decimals =
         MAX(MAX(_decimals(p_s->d_default), _decimals(p_s->d_step)),
             MAX(_decimals(p_s->d_min), _decimals(p_s->d_max)));
   }
   return (b_ok);
}

gboolean
preset_strength_parse(const char *c_graph, gboolean *pb_tunable,
                      PresetStrength *p_out, GError **p_err) {
   g_return_val_if_fail(c_graph != NULL, FALSE);
   const char    *c_open  = NULL;
   const char    *c_close = NULL;
   PresetStrength t_s     = {0};
   if (!_parse(c_graph, &c_open, &c_close, &t_s, p_err)) {
      return (FALSE);
   }
   if (pb_tunable != NULL) {
      *pb_tunable = c_open != NULL;
   }
   if (p_out != NULL && c_open != NULL) {
      *p_out = t_s;
   }
   return (TRUE);
}

char *
preset_strength_format(const PresetStrength *p_s, gdouble d_value) {
   g_return_val_if_fail(p_s != NULL, NULL);
   char c_fmt[8];
   char c_buf[G_ASCII_DTOSTR_BUF_SIZE];
   g_snprintf(c_fmt, sizeof(c_fmt), "%%.%uf", MIN(p_s->u_decimals, 9u));
   g_ascii_formatd(c_buf, sizeof(c_buf), c_fmt, d_value);
   if (strchr(c_buf, '.') != NULL) {
      gsize u_len = strlen(c_buf);
      while (u_len > 0 && c_buf[u_len - 1] == '0') {
         c_buf[--u_len] = '\0';
      }
      if (u_len > 0 && c_buf[u_len - 1] == '.') {
         c_buf[--u_len] = '\0';
      }
   }
   if (g_str_equal(c_buf, "-0")) {
      return (g_strdup("0"));
   }
   return (g_strdup(c_buf));
}

gdouble
preset_strength_clamp(const PresetStrength *p_s, gdouble d_value) {
   g_return_val_if_fail(p_s != NULL, 0.0);
   if (isnan(d_value)) {
      d_value = p_s->d_default;
   }
   d_value = CLAMP(d_value, p_s->d_min, p_s->d_max);
   /* Canonical: the value its own text parses back to. The range's ends
    * are written exactly at u_decimals, so rounding cannot leave it. */
   char   *c_text = preset_strength_format(p_s, d_value);
   gdouble d_out  = g_ascii_strtod(c_text, NULL);
   g_free(c_text);
   return (d_out);
}

/* Steps on the grid of preset_strength_snap -- default + n * step -- so
 * a key and the slider agree on every value between the ends: from
 * {s:0.3:0..1:0.25}'s maximum 1, one step down is 0.8 (the grid point the
 * slider snaps to), not 0.75. A value off the grid (an end of the range,
 * which the grid need not hit) counts from the grid point on the far side
 * of it, so the first step always moves toward the next grid point and
 * never skips one. The tolerance absorbs the binary error of a value
 * that is a grid point. */
gdouble
preset_strength_nudge(const PresetStrength *p_s, gdouble d_value,
                      gint i_steps) {
   g_return_val_if_fail(p_s != NULL, 0.0);
   gdouble d_n =
      (preset_strength_clamp(p_s, d_value) - p_s->d_default) / p_s->d_step;
   gdouble d_base = i_steps > 0   ? floor(d_n + 1e-6)
                    : i_steps < 0 ? ceil(d_n - 1e-6)
                                  : round(d_n);
   return (preset_strength_clamp(p_s, p_s->d_default +
                                         (d_base + i_steps) * p_s->d_step));
}

gdouble
preset_strength_snap(const PresetStrength *p_s, gdouble d_value) {
   g_return_val_if_fail(p_s != NULL, 0.0);
   if (isnan(d_value)) {
      return (preset_strength_clamp(p_s, d_value));
   }
   gdouble d_n = round((d_value - p_s->d_default) / p_s->d_step);
   return (preset_strength_clamp(p_s, p_s->d_default + d_n * p_s->d_step));
}

char *
preset_strength_label(const PresetStrength *p_s, gdouble d_value) {
   g_return_val_if_fail(p_s != NULL, NULL);
   char *c_text = preset_strength_format(p_s, d_value);
   if (p_s->d_min < 0.0 && c_text[0] != '-' && !g_str_equal(c_text, "0")) {
      char *c_signed = g_strconcat("+", c_text, NULL);
      g_free(c_text);
      return (c_signed);
   }
   return (c_text);
}

char *
preset_strength_substitute(const char *c_graph, gdouble d_value,
                           GError **p_err) {
   g_return_val_if_fail(c_graph != NULL, NULL);
   const char    *c_open  = NULL;
   const char    *c_close = NULL;
   PresetStrength t_s     = {0};
   if (!_parse(c_graph, &c_open, &c_close, &t_s, p_err)) {
      return (NULL);
   }
   if (c_open == NULL) {
      return (g_strdup(c_graph));
   }
   char *c_value =
      preset_strength_format(&t_s, preset_strength_clamp(&t_s, d_value));
   char *c_head = g_strndup(c_graph, (gsize)(c_open - c_graph));
   char *c_out  = g_strconcat(c_head, c_value, c_close + 1, NULL);
   g_free(c_head);
   g_free(c_value);
   return (c_out);
}
