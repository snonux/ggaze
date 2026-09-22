/*:*
 * ggaze — enhance side-panel widget construction
 *
 * See enhance-ui.h. Pure widget building: no state, no signals, no GEGL.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "enhance-ui.h"

#include <glib.h>
#include <gtk/gtk.h>
#include <string.h>

#include "enhancer.h"
#include "popup_list.h"

/* Height floor for a card's picture: a GtkPicture with no paintable measures
 * zero, and the thumbnails arrive from a worker a moment after the panel
 * opens, so without this the cards would collapse and then jump. The width
 * comes from the panel (the picture fills it) and CONTENT_FIT_CONTAIN keeps
 * the image's aspect inside that box. */
#define _PIC_MIN_HEIGHT 96

/* Natural width cap, in characters, for every wrapping/ellipsizing label in
 * the panel. Without it a long state line or file name is what sizes the
 * panel -- it grew from 208 to 240 px the moment "Unsaved preview ..."
 * appeared and pushed the viewer aside. With it the labels wrap or
 * ellipsize inside the width the panel asked for. */
#define _LABEL_MAX_CHARS 24

/* A card's label: "1  Auto-fix" left-aligned and ellipsized, so a long user
 * preset name never widens the panel. */
static GtkWidget *
_card_label(const char *c_text) {
   GtkWidget *p_lbl = gtk_label_new(c_text);
   gtk_label_set_xalign(GTK_LABEL(p_lbl), 0.0f);
   gtk_label_set_ellipsize(GTK_LABEL(p_lbl), PANGO_ELLIPSIZE_END);
   gtk_label_set_max_width_chars(GTK_LABEL(p_lbl), _LABEL_MAX_CHARS);
   return (p_lbl);
}

/* One card: a button whose child is the label alone, or a picture over the
 * label. Not focusable -- see the header: a focused button activates on
 * Space, which is hold-to-compare. *p_pic_out is NULL in label-only mode. */
static GtkWidget *
_build_card(const char *c_label, gboolean b_thumbnail, GtkWidget **p_pic_out) {
   GtkWidget *p_btn = gtk_button_new();
   *p_pic_out       = NULL;
   if (b_thumbnail) {
      GtkWidget *p_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      GtkWidget *p_pic = gtk_picture_new();
      gtk_picture_set_content_fit(GTK_PICTURE(p_pic), GTK_CONTENT_FIT_CONTAIN);
      gtk_picture_set_can_shrink(GTK_PICTURE(p_pic), TRUE);
      gtk_widget_set_size_request(p_pic, -1, _PIC_MIN_HEIGHT);
      gtk_widget_set_hexpand(p_pic, TRUE);
      gtk_box_append(GTK_BOX(p_box), p_pic);
      gtk_box_append(GTK_BOX(p_box), _card_label(c_label));
      gtk_button_set_child(GTK_BUTTON(p_btn), p_box);
      *p_pic_out = p_pic;
   } else {
      gtk_button_set_child(GTK_BUTTON(p_btn), _card_label(c_label));
   }
   gtk_widget_set_can_focus(p_btn, FALSE);
   gtk_widget_set_halign(p_btn, GTK_ALIGN_FILL);
   gtk_widget_add_css_class(p_btn, GGAZE_ENHANCE_CARD_CLASS);
   return (p_btn);
}

/* The "0 Original" card (idx -1): clicking it discards the whole preview. */
static GtkWidget *
_build_original_card(gboolean b_thumbnail, GtkWidget **p_pic_out) {
   GtkWidget *p_btn = _build_card("0  Original", b_thumbnail, p_pic_out);
   g_object_set_data(G_OBJECT(p_btn), "idx", GINT_TO_POINTER(-1));
   return (p_btn);
}

/* One preset card (idx u_idx), highlighted when the preset is already on. */
static GtkWidget *
_build_preset_card(guint u_idx, const char *c_name, gboolean b_thumbnail,
                   gboolean b_on, GtkWidget **p_pic_out) {
   char      *c_lbl = popup_list_row_label(u_idx, c_name);
   GtkWidget *p_btn = _build_card(c_lbl, b_thumbnail, p_pic_out);
   g_free(c_lbl);
   g_object_set_data(G_OBJECT(p_btn), "idx", GINT_TO_POINTER((gint)u_idx));
   if (b_on) {
      gtk_widget_add_css_class(p_btn, "ggaze-enhance-on");
   }
   return (p_btn);
}

/* Append the Original card and one card per preset (capped at the mask's 8
 * bits) to p_box, recording each in p_out. */
static void
_build_cards(GtkWidget *p_box, const GPtrArray *p_presets, guint8 u_mask,
             gboolean b_thumbnails, EnhanceUIWidgets *p_out) {
   p_out->p_original_btn =
      _build_original_card(b_thumbnails, &p_out->p_original_pic);
   gtk_box_append(GTK_BOX(p_box), p_out->p_original_btn);
   guint u_n = p_presets != NULL ? p_presets->len : 0;
   if (u_n > G_N_ELEMENTS(p_out->p_btns)) {
      u_n = G_N_ELEMENTS(p_out->p_btns); /* the mask is 8 bits wide */
   }
   for (guint i = 0; i < u_n; i++) {
      const EnhancerPreset *p_pr = g_ptr_array_index((GPtrArray *)p_presets, i);
      p_out->p_btns[i] = _build_preset_card(i, p_pr->c_name, b_thumbnails,
                                            (u_mask & (guint8)(1u << i)) != 0,
                                            &p_out->p_pics[i]);
      gtk_box_append(GTK_BOX(p_box), p_out->p_btns[i]);
   }
   p_out->u_n_presets = u_n;
}

/* The save-state line, the Save button and the key hint under the cards.
 * The button is a GtkActionable on win.enhance-save: the panel is inside the
 * window's tree, so the action resolves without any wiring here. */
static void
_build_footer(GtkWidget *p_box, EnhanceUIWidgets *p_out) {
   GtkWidget *p_state = gtk_label_new(NULL);
   gtk_label_set_xalign(GTK_LABEL(p_state), 0.0f);
   gtk_label_set_wrap(GTK_LABEL(p_state), TRUE);
   gtk_label_set_wrap_mode(GTK_LABEL(p_state), PANGO_WRAP_WORD_CHAR);
   gtk_label_set_max_width_chars(GTK_LABEL(p_state), _LABEL_MAX_CHARS);
   gtk_widget_set_margin_top(p_state, 8);
   gtk_box_append(GTK_BOX(p_box), p_state);
   p_out->p_state = p_state;

   GtkWidget *p_save = gtk_button_new_with_label("Save copy  (s)");
   gtk_actionable_set_action_name(GTK_ACTIONABLE(p_save), "win.enhance-save");
   gtk_widget_set_can_focus(p_save, FALSE);
   gtk_widget_add_css_class(p_save, "suggested-action");
   gtk_box_append(GTK_BOX(p_box), p_save);
   p_out->p_save_btn = p_save;

   GtkWidget *p_hint = gtk_label_new("1-8  toggle a preset\n"
                                     "0  original\n"
                                     "Space  hold: see the original\n"
                                     "s / Ctrl+S  save a copy\n"
                                     "Esc  close this panel");
   gtk_label_set_xalign(GTK_LABEL(p_hint), 0.0f);
   gtk_label_set_wrap(GTK_LABEL(p_hint), TRUE);
   gtk_label_set_max_width_chars(GTK_LABEL(p_hint), _LABEL_MAX_CHARS);
   gtk_widget_set_margin_top(p_hint, 8);
   gtk_widget_add_css_class(p_hint, "dim-label");
   gtk_box_append(GTK_BOX(p_box), p_hint);
}

GtkWidget *
enhance_ui_build_panel(const GPtrArray *p_presets, const char *c_basename,
                       guint8 u_mask, gboolean b_thumbnails,
                       EnhanceUIWidgets *p_out) {
   g_return_val_if_fail(p_out != NULL, NULL);
   memset(p_out, 0, sizeof(*p_out)); /* label-only mode leaves pics NULL */

   /* Three bands: the title, the cards in a vertical scroller that takes
    * whatever height is left, and the footer (save state, Save button, key
    * hint) pinned below it. The footer is outside the scroller on purpose:
    * nine cards outgrow any normal window height, and "how do I save this"
    * must never be the part that scrolled away. */
   GtkWidget *p_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_start(p_panel, 8);
   gtk_widget_set_margin_end(p_panel, 8);
   gtk_widget_set_margin_top(p_panel, 8);
   gtk_widget_set_margin_bottom(p_panel, 8);
   gtk_widget_set_size_request(p_panel, GGAZE_ENHANCE_PANEL_WIDTH, -1);
   gtk_widget_set_hexpand(p_panel, FALSE);
   gtk_widget_set_vexpand(p_panel, TRUE);
   gtk_widget_add_css_class(p_panel, GGAZE_ENHANCE_PANEL_CLASS);

   GtkWidget *p_title = gtk_label_new(NULL);
   gtk_label_set_xalign(GTK_LABEL(p_title), 0.0f);
   gtk_label_set_ellipsize(GTK_LABEL(p_title), PANGO_ELLIPSIZE_MIDDLE);
   gtk_label_set_max_width_chars(GTK_LABEL(p_title), _LABEL_MAX_CHARS);
   gtk_widget_add_css_class(p_title, "heading");
   enhance_ui_set_title(p_title, c_basename);
   gtk_box_append(GTK_BOX(p_panel), p_title);
   p_out->p_title = p_title;

   GtkWidget *p_cards = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
   _build_cards(p_cards, p_presets, u_mask, b_thumbnails, p_out);
   GtkWidget *p_scroll = gtk_scrolled_window_new();
   gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p_scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(p_scroll), p_cards);
   gtk_widget_set_vexpand(p_scroll, TRUE);
   gtk_box_append(GTK_BOX(p_panel), p_scroll);

   _build_footer(p_panel, p_out);
   p_out->p_panel = p_panel;
   return (p_panel);
}

void
enhance_ui_set_title(GtkWidget *p_title, const char *c_basename) {
   g_return_if_fail(GTK_IS_LABEL(p_title));
   char *c_text = c_basename != NULL ? g_strdup_printf("Enhance %s", c_basename)
                                     : g_strdup("Enhance");
   gtk_label_set_text(GTK_LABEL(p_title), c_text);
   g_free(c_text);
}

void
enhance_ui_set_save_state(GtkWidget *p_state, GtkWidget *p_save_btn,
                          gboolean b_active, gboolean b_saved,
                          const char *c_saved) {
   g_return_if_fail(GTK_IS_LABEL(p_state));
   char *c_text = NULL;
   if (!b_active) {
      c_text = g_strdup("No preset on — press 1-8 to try one.");
   } else if (b_saved) {
      c_text = c_saved != NULL ? g_strdup_printf("Saved as %s.", c_saved)
                               : g_strdup("Saved.");
   } else {
      c_text = g_strdup("Unsaved preview — press s to save a copy "
                        "(the original is never modified).");
   }
   gtk_label_set_text(GTK_LABEL(p_state), c_text);
   g_free(c_text);
   if (p_save_btn != NULL) {
      gtk_widget_set_sensitive(p_save_btn, b_active);
   }
}
