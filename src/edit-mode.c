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

   GtkWidget *p_bar;     /* the hint bar root (owned by the window's tree
                          * once packed; a ref is held until then) */
   GtkWidget   *p_title; /* "Edit" / "Crop" / "Straighten" */
   GtkWidget   *p_keys;  /* the mode's keys (markup) */
   GgazeKeyMode e_shown; /* the mode whose keys the labels hold, NONE when
                          * the bar is hidden */
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

GgazeKeyMode
edit_mode_get_mode(EditMode *p_em) {
   g_return_val_if_fail(p_em != NULL, GGAZE_KEY_MODE_NONE);
   switch (tool_ctrl_get_tool(p_em->p_tc)) {
   case GGAZE_TOOL_CROP:
      return (GGAZE_KEY_MODE_CROP);
   case GGAZE_TOOL_STRAIGHTEN:
      return (GGAZE_KEY_MODE_STRAIGHTEN);
   default:
      break;
   }
   return (enhance_ctrl_is_open(p_em->p_ec) ? GGAZE_KEY_MODE_PANEL
                                            : GGAZE_KEY_MODE_NONE);
}

/* A tool first -- its keys are the most transient thing on screen and
 * shadow the panel's (the crop tool's `a` cycles the aspect, it does not
 * close the panel) -- then, with the panel open, the panel's own rows. A
 * key a tool leaves alone still reaches the panel: a digit toggles a
 * preset under the straighten tool as it does beside it. */
gboolean
edit_mode_key(EditMode *p_em, guint u_keyval, GdkModifierType e_state) {
   g_return_val_if_fail(p_em != NULL, FALSE);
   if (tool_ctrl_get_tool(p_em->p_tc) != GGAZE_TOOL_NONE &&
       tool_ctrl_key(p_em->p_tc, u_keyval, e_state)) {
      return (TRUE);
   }
   if (!enhance_ctrl_is_open(p_em->p_ec)) {
      return (FALSE); /* the panel's keys are the panel's alone */
   }
   const char *c_action =
      shortcuts_mode_action(GGAZE_KEY_MODE_PANEL, u_keyval, e_state);
   if (c_action == NULL) {
      return (FALSE);
   }
   gtk_widget_activate_action(p_em->p_window, c_action, NULL);
   return (TRUE);
}

void
edit_mode_sync(EditMode *p_em, gboolean b_large) {
   g_return_if_fail(p_em != NULL);
   GgazeKeyMode e_mode =
      b_large ? edit_mode_get_mode(p_em) : GGAZE_KEY_MODE_NONE;
   if (e_mode == p_em->e_shown) {
      return;
   }
   p_em->e_shown = e_mode;
   if (e_mode == GGAZE_KEY_MODE_NONE) {
      gtk_widget_set_visible(p_em->p_bar, FALSE);
      return;
   }
   char *c_markup = shortcuts_hint_for_mode(e_mode, TRUE);
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
