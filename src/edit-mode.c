/*:*
 * ggaze — the edit key modes: one router for the edit keys + the hint bar
 *
 * See edit-mode.h.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "edit-mode.h"

#include <glib.h>
#include <gtk/gtk.h>

#include "shortcuts.h"

struct EditMode {
   GtkWidget   *p_window; /* borrowed: where panel actions are fired */
   EnhanceCtrl *p_ec;     /* borrowed: the panel */
   ToolCtrl    *p_tc;     /* borrowed: the tools */

   GtkWidget *p_bar;         /* the hint bar root (owned by the window's tree
                              * once packed; a ref is held until then) */
   GtkWidget   *p_title;     /* "Edit" / "Crop" / "Straighten" */
   GtkWidget   *p_keys;      /* the mode's keys (markup) */
   GgazeKeyMode e_shown;     /* the mode whose keys the labels hold, NONE when
                              * the bar is hidden */
   guint    u_shown_presets; /* the preset count the labels were built for */
   gboolean b_large;         /* the large view is up (the last sync's word):
                              * the panel is hidden, not closed, beside the
                              * grid, and its keys are dead there */
};

/* Build the bar: the mode's name, then its keys, on one line that wraps
 * when the window is narrow rather than widening it. */
static void
_build_bar(EditMode *p_em) {
   p_em->p_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
   gtk_widget_add_css_class(p_em->p_bar, GGAZE_HINT_BAR_CLASS);
   gtk_widget_set_visible(p_em->p_bar, FALSE);
   p_em->p_title = gtk_label_new(NULL);
   gtk_widget_add_css_class(p_em->p_title, "heading");
   gtk_widget_set_valign(p_em->p_title, GTK_ALIGN_START);
   p_em->p_keys = gtk_label_new(NULL);
   gtk_label_set_wrap(GTK_LABEL(p_em->p_keys), TRUE);
   gtk_label_set_xalign(GTK_LABEL(p_em->p_keys), 0.0f);
   gtk_widget_set_hexpand(p_em->p_keys, TRUE);
   gtk_box_append(GTK_BOX(p_em->p_bar), p_em->p_title);
   gtk_box_append(GTK_BOX(p_em->p_bar), p_em->p_keys);
   g_object_ref_sink(p_em->p_bar); /* until the window packs it */
}

EditMode *
edit_mode_new(GtkWidget *p_window, EnhanceCtrl *p_ec, ToolCtrl *p_tc) {
   g_return_val_if_fail(GTK_IS_WIDGET(p_window), NULL);
   g_return_val_if_fail(p_ec != NULL && p_tc != NULL, NULL);
   EditMode *p_em = g_new0(EditMode, 1);
   p_em->p_window = p_window;
   p_em->p_ec     = p_ec;
   p_em->p_tc     = p_tc;
   p_em->e_shown  = GGAZE_KEY_MODE_NONE;
   _build_bar(p_em);
   return (p_em);
}

void
edit_mode_delete(EditMode *p_em) {
   if (p_em == NULL) {
      return;
   }
   g_clear_object(&p_em->p_bar); /* our ref; the tree's goes with it */
   g_free(p_em);
}

GtkWidget *
edit_mode_get_hint_bar(EditMode *p_em) {
   g_return_val_if_fail(p_em != NULL, NULL);
   return (p_em->p_bar);
}

/* TRUE iff the panel is the live key mode: open AND on screen. `t` from
 * the large view hides the open panel with the grid (its state survives
 * the round trip) -- its digits must not toggle presets, nor x revert, on
 * an image that is not on screen. */
static gboolean
_panel_live(EditMode *p_em) {
   return (p_em->b_large && enhance_ctrl_is_open(p_em->p_ec));
}

/* The mode for a view: a tool's (only ever up in the large view -- leaving
 * it abandons the tool), else PANEL while the panel is open and b_large,
 * else NONE. */
static GgazeKeyMode
_mode_for(EditMode *p_em, gboolean b_large) {
   switch (tool_ctrl_get_tool(p_em->p_tc)) {
   case GGAZE_TOOL_CROP:
      return (GGAZE_KEY_MODE_CROP);
   case GGAZE_TOOL_STRAIGHTEN:
      return (GGAZE_KEY_MODE_STRAIGHTEN);
   default:
      break;
   }
   return (b_large && enhance_ctrl_is_open(p_em->p_ec) ? GGAZE_KEY_MODE_PANEL
                                                       : GGAZE_KEY_MODE_NONE);
}

GgazeKeyMode
edit_mode_get_mode(EditMode *p_em) {
   g_return_val_if_fail(p_em != NULL, GGAZE_KEY_MODE_NONE);
   return (_mode_for(p_em, p_em->b_large));
}

/* TRUE iff c_action is one of the selected card's actions (8i2): j / k
 * select, Enter toggles the selection, h / l and Shift+H / L tune it. */
static gboolean
_is_card_action(const char *c_action) {
   return (g_str_has_prefix(c_action, "win.edit-select-") ||
           g_str_has_prefix(c_action, "win.edit-strength-") ||
           g_str_equal(c_action, "win.edit-toggle-selected"));
}

/* A tool first -- its keys are the most transient thing on screen and
 * shadow the panel's (the crop tool's `a` cycles the aspect, it does not
 * close the panel) -- then, with the panel open and on screen, the panel's
 * own rows. A key a tool leaves alone still reaches the panel: a digit
 * toggles a preset under the straighten tool as it does beside it. The
 * selected card's keys do NOT (_is_card_action): the straighten tool
 * leaves j / k alone, and a selection or strength stepped under a modal
 * tool would re-render and record undo steps behind it -- they keep their
 * global meaning there instead, as they did before the panel had them. */
gboolean
edit_mode_key(EditMode *p_em, guint u_keyval, GdkModifierType e_state) {
   g_return_val_if_fail(p_em != NULL, FALSE);
   gboolean b_tool = tool_ctrl_get_tool(p_em->p_tc) != GGAZE_TOOL_NONE;
   if (b_tool && tool_ctrl_key(p_em->p_tc, u_keyval, e_state)) {
      return (TRUE);
   }
   if (!_panel_live(p_em)) {
      return (FALSE); /* the panel's keys are the visible panel's alone */
   }
   const char *c_action =
      shortcuts_mode_action(GGAZE_KEY_MODE_PANEL, u_keyval, e_state);
   if (c_action == NULL || (b_tool && _is_card_action(c_action))) {
      return (FALSE);
   }
   gtk_widget_activate_action(p_em->p_window, c_action, NULL);
   return (TRUE);
}

/* How many presets the digits reach: the panel's first rows, at most the
 * GGAZE_ENHANCE_DIGIT_PRESETS that have a digit (ai2: the rows past them,
 * user presets, have no digit, so the hint bar's "1–8 presets" never
 * promises one). */
static guint
_preset_count(EditMode *p_em) {
   const GPtrArray *p_presets = enhance_ctrl_get_presets(p_em->p_ec);
   guint            u_n       = p_presets != NULL ? p_presets->len : 0;
   return (MIN(u_n, (guint)GGAZE_ENHANCE_DIGIT_PRESETS));
}

void
edit_mode_sync(EditMode *p_em, gboolean b_large) {
   g_return_if_fail(p_em != NULL);
   p_em->b_large       = b_large;
   GgazeKeyMode e_mode = b_large ? _mode_for(p_em, TRUE) : GGAZE_KEY_MODE_NONE;
   guint        u_n    = _preset_count(p_em);
   if (e_mode == p_em->e_shown && u_n == p_em->u_shown_presets) {
      return;
   }
   p_em->e_shown         = e_mode;
   p_em->u_shown_presets = u_n;
   if (e_mode == GGAZE_KEY_MODE_NONE) {
      gtk_widget_set_visible(p_em->p_bar, FALSE);
      return;
   }
   char *c_markup = shortcuts_hint_for_mode(e_mode, u_n, TRUE);
   gtk_label_set_markup(GTK_LABEL(p_em->p_keys), c_markup);
   g_free(c_markup);
   gtk_label_set_text(GTK_LABEL(p_em->p_title), shortcuts_mode_title(e_mode));
   gtk_widget_set_visible(p_em->p_bar, TRUE);
}

char *
edit_mode_get_hint_text(EditMode *p_em) {
   g_return_val_if_fail(p_em != NULL, NULL);
   if (p_em->e_shown == GGAZE_KEY_MODE_NONE) {
      return (NULL);
   }
   return (g_strdup_printf("%s: %s",
                           gtk_label_get_text(GTK_LABEL(p_em->p_title)),
                           gtk_label_get_text(GTK_LABEL(p_em->p_keys))));
}
