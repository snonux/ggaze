/*:*
 * ggaze — edit side-panel widget construction
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
#include "shortcuts.h"

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

/* A section heading ("Presets", "Transform", "Actions"). */
static GtkWidget *
_section_label(const char *c_text) {
   GtkWidget *p_lbl = gtk_label_new(c_text);
   gtk_label_set_xalign(GTK_LABEL(p_lbl), 0.0f);
   gtk_widget_add_css_class(p_lbl, "caption-heading");
   gtk_widget_add_css_class(p_lbl, "dim-label");
   gtk_widget_set_margin_top(p_lbl, 6);
   return (p_lbl);
}

/* A picture over a label -- a card's content, or the Original reference.
 * *p_pic_out is the picture, NULL in label-only mode (then the label alone
 * is returned). */
static GtkWidget *
_picture_over_label(const char *c_label, gboolean b_thumbnail,
                    GtkWidget **p_pic_out) {
   *p_pic_out = NULL;
   if (!b_thumbnail) {
      return (_card_label(c_label));
   }
   GtkWidget *p_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
   GtkWidget *p_pic = gtk_picture_new();
   gtk_picture_set_content_fit(GTK_PICTURE(p_pic), GTK_CONTENT_FIT_CONTAIN);
   gtk_picture_set_can_shrink(GTK_PICTURE(p_pic), TRUE);
   gtk_widget_set_size_request(p_pic, -1, _PIC_MIN_HEIGHT);
   gtk_widget_set_hexpand(p_pic, TRUE);
   gtk_box_append(GTK_BOX(p_box), p_pic);
   gtk_box_append(GTK_BOX(p_box), _card_label(c_label));
   *p_pic_out = p_pic;
   return (p_box);
}

/* One preset card (idx u_idx), highlighted when the preset is already on.
 * Not focusable -- see the header: a focused button activates on Space,
 * which is hold-to-compare. */
static GtkWidget *
_build_preset_card(guint u_idx, const char *c_name, gboolean b_thumbnail,
                   gboolean b_on, GtkWidget **p_pic_out) {
   char      *c_lbl = popup_list_row_label(u_idx, c_name);
   GtkWidget *p_btn = gtk_button_new();
   gtk_button_set_child(GTK_BUTTON(p_btn),
                        _picture_over_label(c_lbl, b_thumbnail, p_pic_out));
   g_free(c_lbl);
   gtk_widget_set_can_focus(p_btn, FALSE);
   gtk_widget_set_halign(p_btn, GTK_ALIGN_FILL);
   gtk_widget_add_css_class(p_btn, GGAZE_ENHANCE_CARD_CLASS);
   g_object_set_data(G_OBJECT(p_btn), "idx", GINT_TO_POINTER((gint)u_idx));
   if (b_on) {
      gtk_widget_add_css_class(p_btn, "ggaze-enhance-on");
   }
   return (p_btn);
}

/* The Original reference: the untransformed image as the cards see it, to
 * compare against. Not a button -- reverting is x / the Revert button. */
static GtkWidget *
_build_original(gboolean b_thumbnail, GtkWidget **p_pic_out) {
   GtkWidget *p_orig = _picture_over_label("Original", b_thumbnail, p_pic_out);
   gtk_widget_add_css_class(p_orig, GGAZE_ENHANCE_ORIGINAL_CLASS);
   return (p_orig);
}

/* Append the Original reference and one card per preset (capped at the
 * mask's 8 bits) to p_box, recording each in p_out. */
static void
_build_cards(GtkWidget *p_box, const GPtrArray *p_presets, guint8 u_mask,
             gboolean b_thumbnails, EnhanceUIWidgets *p_out) {
   p_out->p_original = _build_original(b_thumbnails, &p_out->p_original_pic);
   gtk_box_append(GTK_BOX(p_box), p_out->p_original);
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

/* The content of an action button: an optional icon, the label (which may
 * be NULL for an icon-only button), and the key right-aligned in a dim
 * face. c_keys comes from shortcuts.c's table. */
static GtkWidget *
_button_content(const char *c_icon, const char *c_label, const char *c_keys) {
   GtkWidget *p_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
   if (c_icon != NULL) {
      gtk_box_append(GTK_BOX(p_row), gtk_image_new_from_icon_name(c_icon));
   }
   GtkWidget *p_lbl = gtk_label_new(c_label);
   gtk_label_set_xalign(GTK_LABEL(p_lbl), 0.0f);
   gtk_label_set_ellipsize(GTK_LABEL(p_lbl), PANGO_ELLIPSIZE_END);
   gtk_widget_set_hexpand(p_lbl, TRUE);
   gtk_box_append(GTK_BOX(p_row), p_lbl);
   GtkWidget *p_key = gtk_label_new(c_keys);
   gtk_widget_add_css_class(p_key, GGAZE_ENHANCE_KEY_CLASS);
   gtk_widget_add_css_class(p_key, "dim-label");
   gtk_box_append(GTK_BOX(p_row), p_key);
   return (p_row);
}

/* A non-focusable button on c_action holding p_child, with the table's
 * title + keys as its tooltip, like the header bar's buttons. */
static GtkWidget *
_actionable(const char *c_action, GtkWidget *p_child) {
   GtkWidget *p_btn = gtk_button_new();
   gtk_button_set_child(GTK_BUTTON(p_btn), p_child);
   gtk_actionable_set_action_name(GTK_ACTIONABLE(p_btn), c_action);
   char *c_tip = shortcuts_tooltip_for_action(c_action);
   gtk_widget_set_tooltip_text(p_btn, c_tip);
   g_free(c_tip);
   gtk_widget_set_can_focus(p_btn, FALSE);
   return (p_btn);
}

/* A button on c_action showing its key(s) -- the ones the hint bar lists
 * for it, or c_keys when given (Close shows a and Esc, two actions). */
static GtkWidget *
_action_button(const char *c_icon, const char *c_label, const char *c_action,
               const char *c_keys) {
   char      *c_own = c_keys == NULL ? shortcuts_hint_keys_for_action(c_action)
                                     : g_strdup(c_keys);
   GtkWidget *p_btn =
      _actionable(c_action, _button_content(c_icon, c_label, c_own));
   g_free(c_own);
   return (p_btn);
}

/* The Transform section: Crop and Straighten full width, the two quarter
 * turns side by side under them. */
static void
_build_transform(GtkWidget *p_box) {
   gtk_box_append(GTK_BOX(p_box), _section_label("Transform"));
   gtk_box_append(GTK_BOX(p_box), _action_button("edit-cut-symbolic", "Crop",
                                                 "win.crop", NULL));
   gtk_box_append(GTK_BOX(p_box),
                  _action_button("object-flip-horizontal-symbolic",
                                 "Straighten", "win.straighten", NULL));
   GtkWidget *p_turns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_box_set_homogeneous(GTK_BOX(p_turns), TRUE);
   gtk_box_append(GTK_BOX(p_turns),
                  _action_button("object-rotate-left-symbolic", "Left",
                                 "win.rotate-ccw", NULL));
   gtk_box_append(GTK_BOX(p_turns),
                  _action_button("object-rotate-right-symbolic", "Right",
                                 "win.rotate-cw", NULL));
   gtk_box_append(GTK_BOX(p_box), p_turns);
}

/* The Save button: its key row over the name of the file it will write. */
static GtkWidget *
_build_save(EnhanceUIWidgets *p_out) {
   char      *c_keys = shortcuts_hint_keys_for_action("win.enhance-save");
   GtkWidget *p_col  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
   gtk_box_append(GTK_BOX(p_col), _button_content("document-save-symbolic",
                                                  "Save copy", c_keys));
   g_free(c_keys);
   GtkWidget *p_target = gtk_label_new(NULL);
   gtk_label_set_xalign(GTK_LABEL(p_target), 0.0f);
   gtk_label_set_ellipsize(GTK_LABEL(p_target), PANGO_ELLIPSIZE_MIDDLE);
   gtk_label_set_max_width_chars(GTK_LABEL(p_target), _LABEL_MAX_CHARS);
   gtk_widget_add_css_class(p_target, "caption");
   gtk_box_append(GTK_BOX(p_col), p_target);
   GtkWidget *p_btn = _actionable("win.enhance-save", p_col);
   gtk_widget_add_css_class(p_btn, "suggested-action");
   p_out->p_save_btn    = p_btn;
   p_out->p_save_target = p_target;
   return (p_btn);
}

/* The Actions section: the save-state line, Save copy, Revert all, Close.
 * Pinned below the card scroller: "how do I keep (or drop) this" must
 * never be what scrolled away. */
static void
_build_actions(GtkWidget *p_box, EnhanceUIWidgets *p_out) {
   gtk_box_append(GTK_BOX(p_box), _section_label("Actions"));
   GtkWidget *p_state = gtk_label_new(NULL);
   gtk_label_set_xalign(GTK_LABEL(p_state), 0.0f);
   gtk_label_set_wrap(GTK_LABEL(p_state), TRUE);
   gtk_label_set_wrap_mode(GTK_LABEL(p_state), PANGO_WRAP_WORD_CHAR);
   gtk_label_set_max_width_chars(GTK_LABEL(p_state), _LABEL_MAX_CHARS);
   gtk_box_append(GTK_BOX(p_box), p_state);
   p_out->p_state = p_state;
   gtk_box_append(GTK_BOX(p_box), _build_save(p_out));
   gtk_box_append(GTK_BOX(p_box),
                  _action_button("edit-undo-symbolic", "Revert all",
                                 "win.edit-revert", NULL));
   char *c_close =
      shortcuts_hint_keys(GGAZE_KEY_MODE_PANEL, SHORTCUTS_HINT_CLOSE);
   gtk_box_append(
      GTK_BOX(p_box),
      _action_button("window-close-symbolic", "Close", "win.enhance", c_close));
   g_free(c_close);
}

/* The panel root: a narrow vertical column with the panel's margins. */
static GtkWidget *
_panel_root(void) {
   GtkWidget *p_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_start(p_panel, 8);
   gtk_widget_set_margin_end(p_panel, 8);
   gtk_widget_set_margin_top(p_panel, 8);
   gtk_widget_set_margin_bottom(p_panel, 8);
   gtk_widget_set_size_request(p_panel, GGAZE_ENHANCE_PANEL_WIDTH, -1);
   gtk_widget_set_hexpand(p_panel, FALSE);
   gtk_widget_set_vexpand(p_panel, TRUE);
   gtk_widget_add_css_class(p_panel, GGAZE_ENHANCE_PANEL_CLASS);
   return (p_panel);
}

GtkWidget *
enhance_ui_build_panel(const GPtrArray *p_presets, const char *c_basename,
                       guint8 u_mask, gboolean b_thumbnails,
                       EnhanceUIWidgets *p_out) {
   g_return_val_if_fail(p_out != NULL, NULL);
   memset(p_out, 0, sizeof(*p_out)); /* label-only mode leaves pics NULL */

   /* The title, then the Presets cards in a vertical scroller that takes
    * whatever height is left, then Transform and Actions pinned below it:
    * nine cards outgrow any normal window height, and the tools and the
    * save must never be the part that scrolled away. */
   GtkWidget *p_panel = _panel_root();
   GtkWidget *p_title = gtk_label_new(NULL);
   gtk_label_set_xalign(GTK_LABEL(p_title), 0.0f);
   gtk_label_set_ellipsize(GTK_LABEL(p_title), PANGO_ELLIPSIZE_MIDDLE);
   gtk_label_set_max_width_chars(GTK_LABEL(p_title), _LABEL_MAX_CHARS);
   gtk_widget_add_css_class(p_title, "heading");
   enhance_ui_set_title(p_title, c_basename);
   gtk_box_append(GTK_BOX(p_panel), p_title);
   p_out->p_title = p_title;

   gtk_box_append(GTK_BOX(p_panel), _section_label("Presets"));
   GtkWidget *p_cards = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
   _build_cards(p_cards, p_presets, u_mask, b_thumbnails, p_out);
   GtkWidget *p_scroll = gtk_scrolled_window_new();
   gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p_scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(p_scroll), p_cards);
   gtk_widget_set_vexpand(p_scroll, TRUE);
   gtk_box_append(GTK_BOX(p_panel), p_scroll);

   _build_transform(p_panel);
   _build_actions(p_panel, p_out);
   p_out->p_panel = p_panel;
   return (p_panel);
}

void
enhance_ui_set_title(GtkWidget *p_title, const char *c_basename) {
   g_return_if_fail(GTK_IS_LABEL(p_title));
   char *c_text = c_basename != NULL ? g_strdup_printf("Edit %s", c_basename)
                                     : g_strdup("Edit");
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
      c_text = g_strdup("No edits yet — pick a preset or a tool.");
   } else if (b_saved) {
      c_text = c_saved != NULL ? g_strdup_printf("Saved as %s.", c_saved)
                               : g_strdup("Saved.");
   } else {
      c_text = g_strdup("Unsaved edits — s saves a copy (the original is "
                        "never modified).");
   }
   gtk_label_set_text(GTK_LABEL(p_state), c_text);
   g_free(c_text);
   if (p_save_btn != NULL) {
      gtk_widget_set_sensitive(p_save_btn, b_active);
   }
}

void
enhance_ui_set_save_target(GtkWidget *p_target, const char *c_name) {
   g_return_if_fail(GTK_IS_LABEL(p_target));
   char *c_text =
      c_name != NULL ? g_strdup_printf("→ %s", c_name) : g_strdup("");
   gtk_label_set_text(GTK_LABEL(p_target), c_text);
   g_free(c_text);
}
