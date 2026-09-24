/*:*
 * ggaze — key modes of the ONE key table (unit, no display)
 *
 * shortcuts.c's table scopes rows to the edit panel and the two tools and
 * generates the key-hint bars from them (6i2). These are pure table
 * lookups and string building -- gdk_keyval_* need no display -- so they
 * run in every lane, the minimal one included: which key does what in
 * which mode, the modifier matching (Shift only for letters, Ctrl only
 * when a row names it, Caps Lock never read as Shift), and the exact hint
 * lines (the preset digits as many as there are presets), so a
 * table edit that changes what the bar or the panel buttons say shows up
 * here.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "shortcuts.h"

#include <glib.h>
#include <gtk/gtk.h>

/* The panel's own keys: the digits fire the preset actions only in the
 * PANEL mode; nothing is a panel key while browsing. */
static void
test_panel_keys_are_scoped(void) {
   g_assert_cmpstr(shortcuts_mode_action(GGAZE_KEY_MODE_PANEL, GDK_KEY_1, 0),
                   ==, "win.enhance-1");
   g_assert_cmpstr(shortcuts_mode_action(GGAZE_KEY_MODE_PANEL, GDK_KEY_8, 0),
                   ==, "win.enhance-8");
   g_assert_null(shortcuts_mode_action(GGAZE_KEY_MODE_PANEL, GDK_KEY_9, 0));
   g_assert_null(shortcuts_mode_action(GGAZE_KEY_MODE_NONE, GDK_KEY_1, 0));
   g_assert_null(shortcuts_mode_action(GGAZE_KEY_MODE_CROP, GDK_KEY_1, 0));
   /* h / l / j / k stay free in the panel (navigation now, strength later),
    * and so does u (undo later). */
   g_assert_null(shortcuts_mode_action(GGAZE_KEY_MODE_PANEL, GDK_KEY_h, 0));
   g_assert_null(shortcuts_mode_action(GGAZE_KEY_MODE_PANEL, GDK_KEY_u, 0));
   /* A chord is not the digit. */
   g_assert_null(
      shortcuts_mode_action(GGAZE_KEY_MODE_PANEL, GDK_KEY_1, GDK_CONTROL_MASK));
}

/* The digits and `0` are no longer global: only the panel binds 1-8, and
 * `0` is zoom and nothing else. c / r / [ / ] / x / s are global (they
 * open the panel or explain themselves); R is free. */
static void
test_global_keys(void) {
   g_assert_null(shortcuts_global_action(GDK_KEY_1, 0));
   g_assert_cmpstr(shortcuts_global_action(GDK_KEY_0, 0), ==, "win.zoom-reset");
   g_assert_cmpstr(shortcuts_global_action(GDK_KEY_c, 0), ==, "win.crop");
   g_assert_cmpstr(shortcuts_global_action(GDK_KEY_r, 0), ==, "win.straighten");
   g_assert_null(shortcuts_global_action(GDK_KEY_R, GDK_SHIFT_MASK));
   g_assert_cmpstr(shortcuts_global_action(GDK_KEY_x, 0), ==,
                   "win.edit-revert");
   g_assert_cmpstr(shortcuts_global_action(GDK_KEY_bracketleft, 0), ==,
                   "win.rotate-ccw");
   g_assert_cmpstr(shortcuts_global_action(GDK_KEY_s, GDK_CONTROL_MASK), ==,
                   "win.enhance-save");
   /* Caps Lock does not turn `c` into something else. */
   g_assert_cmpstr(shortcuts_global_action(GDK_KEY_c, GDK_LOCK_MASK), ==,
                   "win.crop");
   /* A tool-only row is not global. */
   g_assert_null(shortcuts_global_action(GDK_KEY_h, GDK_CONTROL_MASK));
}

/* The crop tool: h/j/k/l move, Shift grows and Ctrl shrinks the side the
 * key points at -- all four sides -- and `a` cycles the aspect. Shift may
 * arrive as the upper-case keyval (a real press) or as the modifier. */
static void
test_crop_ops(void) {
   static const struct {
      guint           u_key;
      GdkModifierType e_mods;
      GgazeKeyOp      e_op;
   } CASES[] = {
      {GDK_KEY_h, 0, GGAZE_KEY_OP_CROP_MOVE_LEFT},
      {GDK_KEY_l, 0, GGAZE_KEY_OP_CROP_MOVE_RIGHT},
      {GDK_KEY_k, 0, GGAZE_KEY_OP_CROP_MOVE_UP},
      {GDK_KEY_j, 0, GGAZE_KEY_OP_CROP_MOVE_DOWN},
      {GDK_KEY_H, 0, GGAZE_KEY_OP_CROP_GROW_LEFT},
      {GDK_KEY_H, GDK_SHIFT_MASK, GGAZE_KEY_OP_CROP_GROW_LEFT},
      {GDK_KEY_h, GDK_SHIFT_MASK, GGAZE_KEY_OP_CROP_GROW_LEFT},
      {GDK_KEY_L, GDK_SHIFT_MASK, GGAZE_KEY_OP_CROP_GROW_RIGHT},
      {GDK_KEY_K, GDK_SHIFT_MASK, GGAZE_KEY_OP_CROP_GROW_TOP},
      {GDK_KEY_J, GDK_SHIFT_MASK, GGAZE_KEY_OP_CROP_GROW_BOTTOM},
      {GDK_KEY_h, GDK_CONTROL_MASK, GGAZE_KEY_OP_CROP_SHRINK_LEFT},
      {GDK_KEY_l, GDK_CONTROL_MASK, GGAZE_KEY_OP_CROP_SHRINK_RIGHT},
      {GDK_KEY_k, GDK_CONTROL_MASK, GGAZE_KEY_OP_CROP_SHRINK_TOP},
      {GDK_KEY_j, GDK_CONTROL_MASK, GGAZE_KEY_OP_CROP_SHRINK_BOTTOM},
      {GDK_KEY_a, 0, GGAZE_KEY_OP_CROP_ASPECT},
      {GDK_KEY_Return, 0, GGAZE_KEY_OP_TOOL_APPLY},
      {GDK_KEY_KP_Enter, 0, GGAZE_KEY_OP_TOOL_APPLY},
      {GDK_KEY_Escape, 0, GGAZE_KEY_OP_TOOL_CANCEL},
      /* not the crop tool's: */
      {GDK_KEY_H, GDK_CONTROL_MASK | GDK_SHIFT_MASK, GGAZE_KEY_OP_NONE},
      {GDK_KEY_1, 0, GGAZE_KEY_OP_NONE}, /* digits are no aspect keys now */
      {GDK_KEY_0, 0, GGAZE_KEY_OP_NONE}, /* `0` is zoom */
      {GDK_KEY_A, GDK_SHIFT_MASK, GGAZE_KEY_OP_NONE},
      {GDK_KEY_q, GDK_CONTROL_MASK, GGAZE_KEY_OP_NONE},
      {GDK_KEY_minus, 0, GGAZE_KEY_OP_NONE},
   };
   for (gsize u = 0; u < G_N_ELEMENTS(CASES); u++) {
      g_assert_cmpint(shortcuts_mode_op(GGAZE_KEY_MODE_CROP, CASES[u].u_key,
                                        CASES[u].e_mods),
                      ==, CASES[u].e_op);
   }
   g_assert_cmpint(shortcuts_mode_op(GGAZE_KEY_MODE_NONE, GDK_KEY_h, 0), ==,
                   GGAZE_KEY_OP_NONE);
}

/* The straighten tool: h / - / _ counter-clockwise, l / + / = clockwise,
 * `a` (no longer `A`) the auto-crop. `+` arrives with Shift on most
 * layouts; Shift is part of the keyval there, not a modifier. */
static void
test_straighten_ops(void) {
   GgazeKeyMode e_m = GGAZE_KEY_MODE_STRAIGHTEN;
   g_assert_cmpint(shortcuts_mode_op(e_m, GDK_KEY_h, 0), ==,
                   GGAZE_KEY_OP_STRAIGHTEN_CCW);
   g_assert_cmpint(shortcuts_mode_op(e_m, GDK_KEY_underscore, GDK_SHIFT_MASK),
                   ==, GGAZE_KEY_OP_STRAIGHTEN_CCW);
   g_assert_cmpint(shortcuts_mode_op(e_m, GDK_KEY_plus, GDK_SHIFT_MASK), ==,
                   GGAZE_KEY_OP_STRAIGHTEN_CW);
   g_assert_cmpint(shortcuts_mode_op(e_m, GDK_KEY_equal, 0), ==,
                   GGAZE_KEY_OP_STRAIGHTEN_CW);
   g_assert_cmpint(shortcuts_mode_op(e_m, GDK_KEY_a, 0), ==,
                   GGAZE_KEY_OP_STRAIGHTEN_AUTOCROP);
   g_assert_cmpint(shortcuts_mode_op(e_m, GDK_KEY_A, GDK_SHIFT_MASK), ==,
                   GGAZE_KEY_OP_NONE);
   g_assert_cmpint(shortcuts_mode_op(e_m, GDK_KEY_h, GDK_CONTROL_MASK), ==,
                   GGAZE_KEY_OP_NONE);
   g_assert_cmpint(shortcuts_mode_op(e_m, GDK_KEY_Escape, 0), ==,
                   GGAZE_KEY_OP_TOOL_CANCEL);
}

/* How the panel buttons and the hint bar print a key. */
static void
test_key_labels(void) {
   static const struct {
      guint           u_key;
      GdkModifierType e_mods;
      const char     *c_label;
   } CASES[] = {
      {GDK_KEY_c, 0, "c"},
      {GDK_KEY_H, GDK_SHIFT_MASK, "Shift+h"},
      {GDK_KEY_H, 0, "Shift+h"},
      {GDK_KEY_l, GDK_CONTROL_MASK, "Ctrl+l"},
      {GDK_KEY_q, GDK_ALT_MASK, "Alt+q"},
      {GDK_KEY_Return, 0, "Enter"},
      {GDK_KEY_KP_Enter, 0, "Enter"},
      {GDK_KEY_Escape, 0, "Esc"},
      {GDK_KEY_space, 0, "Space"},
      {GDK_KEY_bracketleft, 0, "["},
      {GDK_KEY_plus, 0, "+"},
      {GDK_KEY_Left, GDK_SHIFT_MASK, "Shift+Left"},
   };
   for (gsize u = 0; u < G_N_ELEMENTS(CASES); u++) {
      char *c_label = shortcuts_key_label(CASES[u].u_key, CASES[u].e_mods);
      g_assert_cmpstr(c_label, ==, CASES[u].c_label);
      g_free(c_label);
   }
}

/* A hint line with its key/label joins (U+00A0, so a wrapped bar never
 * splits a key from its label) read as plain spaces, for comparing whole
 * lines; test_hint_nbsp checks the joins themselves. */
static char *
hint(GgazeKeyMode e_mode, guint u_n, gboolean b_markup) {
   char *c_raw = shortcuts_hint_for_mode(e_mode, u_n, b_markup);
   if (c_raw == NULL) {
      return (NULL);
   }
   char **c_parts = g_strsplit(c_raw, "\u00a0", -1);
   char  *c_out   = g_strjoinv(" ", c_parts);
   g_strfreev(c_parts);
   g_free(c_raw);
   return (c_out);
}

/* Every key is glued to its label by a no-break space, and only there: the
 * " · " separators stay breakable. */
static void
test_hint_nbsp(void) {
   char *c_raw = shortcuts_hint_for_mode(GGAZE_KEY_MODE_CROP, 8, FALSE);
   g_assert_true(g_str_has_prefix(c_raw, "h/j/k/l\u00a0move  ·  "));
   g_assert_nonnull(g_strstr_len(c_raw, -1, "Esc\u00a0cancel"));
   g_free(c_raw);
}

/* The three hint bars, word for word: the order, the merging (a digit run
 * as a range, a shared modifier said once, a/Esc joined) and the labels. */
static void
test_hint_lines(void) {
   char *c_panel = hint(GGAZE_KEY_MODE_PANEL, 8, FALSE);
   g_assert_cmpstr(c_panel, ==,
                   "1–8 presets  ·  c crop  ·  r straighten  "
                   "·  [/] rotate  ·  s save copy  ·  "
                   "x revert  ·  Space hold: original  ·  "
                   "a/Esc close");
   g_free(c_panel);
   char *c_crop = hint(GGAZE_KEY_MODE_CROP, 8, FALSE);
   g_assert_cmpstr(c_crop, ==,
                   "h/j/k/l move  ·  Shift+h/j/k/l grow  ·  "
                   "Ctrl+h/j/k/l shrink  ·  a aspect  ·  "
                   "Enter apply  ·  Esc cancel");
   g_free(c_crop);
   char *c_str = hint(GGAZE_KEY_MODE_STRAIGHTEN, 8, FALSE);
   g_assert_cmpstr(c_str, ==,
                   "h/l/-/+ nudge ½°  ·  a auto-crop  "
                   "·  Enter apply  ·  Esc cancel");
   g_free(c_str);
   g_assert_null(hint(GGAZE_KEY_MODE_NONE, 8, FALSE));
   /* Markup: the keys in bold, the text escaped ("<" never appears raw). */
   char *c_markup = hint(GGAZE_KEY_MODE_CROP, 8, TRUE);
   g_assert_true(g_str_has_prefix(c_markup, "<b>h/j/k/l</b> move"));
   g_free(c_markup);
}

/* The panel's hint line lists the digits of the presets that exist, never
 * "1–8" over fewer: a range from three on, "1/2" for two, the singular for
 * one, and no presets segment at all for none. */
static void
test_hint_preset_count(void) {
   static const struct {
      guint       u_n;
      const char *c_prefix;
   } CASES[] = {
      {8, "1–8 presets  ·  c crop"},
      {12, "1–8 presets  ·  c crop"}, /* the mask stops at 8 */
      {5, "1–5 presets  ·  c crop"},
      {3, "1–3 presets  ·  c crop"},
      {2, "1/2 presets  ·  c crop"},
      {1, "1 preset  ·  c crop"},
      {0, "c crop  ·  r straighten"},
   };
   for (gsize u = 0; u < G_N_ELEMENTS(CASES); u++) {
      char *c_line = hint(GGAZE_KEY_MODE_PANEL, CASES[u].u_n, FALSE);
      if (!g_str_has_prefix(c_line, CASES[u].c_prefix)) {
         g_error("%u presets: \"%s\"", CASES[u].u_n, c_line);
      }
      g_free(c_line);
   }
   /* The tools' lines have no digits to trim. */
   char *c_a = hint(GGAZE_KEY_MODE_CROP, 0, FALSE);
   char *c_b = hint(GGAZE_KEY_MODE_CROP, 8, FALSE);
   g_assert_cmpstr(c_a, ==, c_b);
   g_free(c_a);
   g_free(c_b);
}

/* Caps Lock is not Shift: with it on, a plain `h` arrives as `H` + Lock
 * and must still MOVE the crop rectangle (it used to grow it), `a` must
 * still be the tool's (it fell through to the global `a` and closed the
 * panel under the tool), and `c` the global crop key. Shift with Caps Lock
 * is Shift, in whichever case the keyval arrives. */
static void
test_caps_lock(void) {
   static const struct {
      GgazeKeyMode    e_mode;
      guint           u_key;
      GdkModifierType e_mods;
      GgazeKeyOp      e_op;
   } CASES[] = {
      {GGAZE_KEY_MODE_CROP, GDK_KEY_H, GDK_LOCK_MASK,
       GGAZE_KEY_OP_CROP_MOVE_LEFT},
      {GGAZE_KEY_MODE_CROP, GDK_KEY_J, GDK_LOCK_MASK,
       GGAZE_KEY_OP_CROP_MOVE_DOWN},
      {GGAZE_KEY_MODE_CROP, GDK_KEY_A, GDK_LOCK_MASK, GGAZE_KEY_OP_CROP_ASPECT},
      {GGAZE_KEY_MODE_STRAIGHTEN, GDK_KEY_A, GDK_LOCK_MASK,
       GGAZE_KEY_OP_STRAIGHTEN_AUTOCROP},
      {GGAZE_KEY_MODE_STRAIGHTEN, GDK_KEY_L, GDK_LOCK_MASK,
       GGAZE_KEY_OP_STRAIGHTEN_CW},
      {GGAZE_KEY_MODE_CROP, GDK_KEY_h, GDK_SHIFT_MASK | GDK_LOCK_MASK,
       GGAZE_KEY_OP_CROP_GROW_LEFT},
      {GGAZE_KEY_MODE_CROP, GDK_KEY_H, GDK_SHIFT_MASK | GDK_LOCK_MASK,
       GGAZE_KEY_OP_CROP_GROW_LEFT},
      {GGAZE_KEY_MODE_CROP, GDK_KEY_H, GDK_CONTROL_MASK | GDK_LOCK_MASK,
       GGAZE_KEY_OP_CROP_SHRINK_LEFT},
      /* Shift+a is no crop key, with or without Caps Lock. */
      {GGAZE_KEY_MODE_CROP, GDK_KEY_a, GDK_SHIFT_MASK | GDK_LOCK_MASK,
       GGAZE_KEY_OP_NONE},
   };
   for (gsize u = 0; u < G_N_ELEMENTS(CASES); u++) {
      g_assert_cmpint(
         shortcuts_mode_op(CASES[u].e_mode, CASES[u].u_key, CASES[u].e_mods),
         ==, CASES[u].e_op);
   }
   g_assert_cmpstr(shortcuts_global_action(GDK_KEY_C, GDK_LOCK_MASK), ==,
                   "win.crop");
   g_assert_cmpstr(shortcuts_global_action(GDK_KEY_A, GDK_LOCK_MASK), ==,
                   "win.enhance");
   g_assert_cmpstr(
      shortcuts_mode_action(GGAZE_KEY_MODE_PANEL, GDK_KEY_1, GDK_LOCK_MASK), ==,
      "win.enhance-1");
   /* Shift+Lock+g is G (last image), as Shift+g is. */
   g_assert_cmpstr(
      shortcuts_global_action(GDK_KEY_g, GDK_SHIFT_MASK | GDK_LOCK_MASK), ==,
      "win.last");
   g_assert_cmpstr(shortcuts_global_action(GDK_KEY_G, GDK_LOCK_MASK), ==,
                   "win.first");
}

/* The F10 menu's short labels; the help keeps the long description. */
static void
test_menu_labels(void) {
   static const struct {
      const char *c_action;
      const char *c_label;
   } CASES[] = {
      {"win.enhance", "Edit panel"},
      {"win.crop", "Crop"},
      {"win.straighten", "Straighten"},
      {"win.rotate-ccw", "Rotate left"},
      {"win.rotate-cw", "Rotate right"},
      {"win.enhance-save", "Save edited copy"},
      {"win.edit-revert", "Revert all edits"},
      {"win.next", "Next image"}, /* no short label: the title */
      {"win.no-such-action", NULL},
   };
   for (gsize u = 0; u < G_N_ELEMENTS(CASES); u++) {
      g_assert_cmpstr(shortcuts_label_for_action(CASES[u].c_action), ==,
                      CASES[u].c_label);
   }
   g_assert_true(g_str_has_prefix(shortcuts_title_for_action("win.crop"),
                                  "Crop tool (Enter applies"));
}

/* What the panel's buttons print, and the mode names on the bar. */
static void
test_button_keys_and_titles(void) {
   static const struct {
      const char *c_action;
      const char *c_keys;
   } CASES[] = {
      {"win.crop", "c"},
      {"win.straighten", "r"},
      {"win.rotate-ccw", "["},
      {"win.rotate-cw", "]"},
      {"win.enhance-save", "s"},
      {"win.edit-revert", "x"},
      {"win.enhance-3", "3"},
      {"win.enhance", "a"},
      {"win.no-such-action", NULL},
   };
   for (gsize u = 0; u < G_N_ELEMENTS(CASES); u++) {
      char *c_keys = shortcuts_hint_keys_for_action(CASES[u].c_action);
      g_assert_cmpstr(c_keys, ==, CASES[u].c_keys);
      g_free(c_keys);
   }
   char *c_close =
      shortcuts_hint_keys(GGAZE_KEY_MODE_PANEL, SHORTCUTS_HINT_CLOSE);
   g_assert_cmpstr(c_close, ==, "a/Esc");
   g_free(c_close);
   g_assert_null(shortcuts_hint_keys(GGAZE_KEY_MODE_NONE, "close"));
   g_assert_null(shortcuts_hint_keys(GGAZE_KEY_MODE_CROP, "no such hint"));
   g_assert_cmpstr(shortcuts_mode_title(GGAZE_KEY_MODE_PANEL), ==, "Edit");
   g_assert_cmpstr(shortcuts_mode_title(GGAZE_KEY_MODE_CROP), ==, "Crop");
   g_assert_cmpstr(shortcuts_mode_title(GGAZE_KEY_MODE_STRAIGHTEN), ==,
                   "Straighten");
   g_assert_null(shortcuts_mode_title(GGAZE_KEY_MODE_NONE));
   /* The panel's digits show in the menu / tooltips like any action. */
   char *c_tip = shortcuts_tooltip_for_action("win.enhance-1");
   g_assert_nonnull(c_tip);
   g_assert_nonnull(g_strstr_len(c_tip, -1, "panel open"));
   g_free(c_tip);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
   g_test_add_func("/keymodes/panel_keys_are_scoped",
                   test_panel_keys_are_scoped);
   g_test_add_func("/keymodes/global_keys", test_global_keys);
   g_test_add_func("/keymodes/crop_ops", test_crop_ops);
   g_test_add_func("/keymodes/straighten_ops", test_straighten_ops);
   g_test_add_func("/keymodes/key_labels", test_key_labels);
   g_test_add_func("/keymodes/hint_nbsp", test_hint_nbsp);
   g_test_add_func("/keymodes/hint_lines", test_hint_lines);
   g_test_add_func("/keymodes/button_keys_and_titles",
                   test_button_keys_and_titles);
   g_test_add_func("/keymodes/hint_preset_count", test_hint_preset_count);
   g_test_add_func("/keymodes/caps_lock", test_caps_lock);
   g_test_add_func("/keymodes/menu_labels", test_menu_labels);
   return (g_test_run());
}
