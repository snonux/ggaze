/*:*
 * ggaze — edit side-panel widget construction
 *
 * See enhance-ui.h. Pure widget building: no state, no signals, no GEGL.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "enhance-ui.h"

#include <adwaita.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <string.h>

#include "enhancer.h"
#include "popup_list.h"
#include "shortcuts.h"

/* The box every thumbnail (the Original's and each card's) is drawn in.
 * Small on purpose: the cards are ROWS, so all eight fit a 800 px tall
 * window without scrolling, and the picture only has to say "warmer" or
 * "more contrast" next to its neighbours -- the large view shows the real
 * thing. */
#define _THUMB_W 60
#define _THUMB_H 40

/* Natural width cap, in characters, for every ellipsizing label in the
 * panel. Without it a long state line or file name is what sizes the
 * panel -- it grew past its width the moment "Unsaved ..." appeared and
 * pushed the viewer aside. With it the labels ellipsize inside the width
 * the panel asked for. */
#define _LABEL_MAX_CHARS 24

/* A one-line label, left-aligned, ellipsized (e_mode) within the panel. */
static GtkWidget *
_line_label(const char *c_text, PangoEllipsizeMode e_mode) {
   GtkWidget *p_lbl = gtk_label_new(c_text);
   gtk_label_set_xalign(GTK_LABEL(p_lbl), 0.0f);
   gtk_label_set_ellipsize(GTK_LABEL(p_lbl), e_mode);
   gtk_label_set_max_width_chars(GTK_LABEL(p_lbl), _LABEL_MAX_CHARS);
   return (p_lbl);
}

/* A section heading ("Presets", "Transform"). */
static GtkWidget *
_section_label(const char *c_text) {
   GtkWidget *p_lbl = gtk_label_new(c_text);
   gtk_label_set_xalign(GTK_LABEL(p_lbl), 0.0f);
   gtk_widget_add_css_class(p_lbl, "caption-heading");
   gtk_widget_add_css_class(p_lbl, "dim-label");
   gtk_widget_set_margin_top(p_lbl, 4);
   return (p_lbl);
}

/* The key a button fires, as a small dim badge (from shortcuts.c's
 * table). */
static GtkWidget *
_key_badge(const char *c_keys) {
   GtkWidget *p_key = gtk_label_new(c_keys);
   gtk_widget_add_css_class(p_key, GGAZE_ENHANCE_KEY_CLASS);
   gtk_widget_add_css_class(p_key, "dim-label");
   return (p_key);
}

/* A fixed _THUMB_W x _THUMB_H box around a GtkPicture (*p_pic_out). A
 * GtkPicture asks for its paintable's full size (the previews are up to
 * 512 px), which would size the whole panel; the two clamps cap that
 * request in both directions, and the size request keeps the box from
 * collapsing before the thumbnail arrives from its worker. CONTAIN keeps
 * the image's aspect inside the box. */
static GtkWidget *
_thumb_box(GtkWidget **p_pic_out) {
   GtkWidget *p_pic = gtk_picture_new();
   gtk_picture_set_content_fit(GTK_PICTURE(p_pic), GTK_CONTENT_FIT_CONTAIN);
   gtk_picture_set_can_shrink(GTK_PICTURE(p_pic), TRUE);
   GtkWidget *p_tall = adw_clamp_new();
   gtk_orientable_set_orientation(GTK_ORIENTABLE(p_tall),
                                  GTK_ORIENTATION_VERTICAL);
   adw_clamp_set_maximum_size(ADW_CLAMP(p_tall), _THUMB_H);
   adw_clamp_set_tightening_threshold(ADW_CLAMP(p_tall), _THUMB_H);
   adw_clamp_set_child(ADW_CLAMP(p_tall), p_pic);
   GtkWidget *p_wide = adw_clamp_new();
   adw_clamp_set_maximum_size(ADW_CLAMP(p_wide), _THUMB_W);
   adw_clamp_set_tightening_threshold(ADW_CLAMP(p_wide), _THUMB_W);
   adw_clamp_set_child(ADW_CLAMP(p_wide), p_tall);
   gtk_widget_set_size_request(p_wide, _THUMB_W, _THUMB_H);
   gtk_widget_set_valign(p_wide, GTK_ALIGN_CENTER);
   *p_pic_out = p_pic;
   return (p_wide);
}

/* One preset card (idx u_idx): a row -- the thumbnail (thumbnail mode),
 * "1  Auto-fix", and a check mark that the "ggaze-enhance-on" class shows
 * (window.c's CSS) along with the highlight, so on / off reads at a glance
 * and needs no state here. Not focusable -- see the header: a focused
 * button activates on Space, which is hold-to-compare. */
static GtkWidget *
_build_preset_card(guint u_idx, const char *c_name, gboolean b_thumbnail,
                   gboolean b_on, GtkWidget **p_pic_out) {
   GtkWidget *p_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
   *p_pic_out       = NULL;
   if (b_thumbnail) {
      gtk_box_append(GTK_BOX(p_row), _thumb_box(p_pic_out));
   }
   char      *c_lbl   = popup_list_row_label(u_idx, c_name);
   GtkWidget *p_label = _line_label(c_lbl, PANGO_ELLIPSIZE_END);
   g_free(c_lbl);
   gtk_widget_set_hexpand(p_label, TRUE);
   gtk_box_append(GTK_BOX(p_row), p_label);
   GtkWidget *p_check = gtk_image_new_from_icon_name("object-select-symbolic");
   gtk_widget_add_css_class(p_check, GGAZE_ENHANCE_CHECK_CLASS);
   gtk_box_append(GTK_BOX(p_row), p_check);
   GtkWidget *p_btn = gtk_button_new();
   gtk_button_set_child(GTK_BUTTON(p_btn), p_row);
   gtk_widget_set_can_focus(p_btn, FALSE);
   gtk_widget_add_css_class(p_btn, GGAZE_ENHANCE_CARD_CLASS);
   g_object_set_data(G_OBJECT(p_btn), "idx", GINT_TO_POINTER((gint)u_idx));
   if (b_on) {
      gtk_widget_add_css_class(p_btn, "ggaze-enhance-on");
   }
   return (p_btn);
}

/* The Original reference: the untouched image as the cards see it, to
 * compare against -- a small thumbnail beside "Original" and how to see it
 * large. Not a button (no frame, dim text), so it never reads as one more
 * preset: reverting is x / the Revert button. */
static GtkWidget *
_build_original(gboolean b_thumbnail, GtkWidget **p_pic_out) {
   GtkWidget *p_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
   *p_pic_out       = NULL;
   if (b_thumbnail) {
      gtk_box_append(GTK_BOX(p_row), _thumb_box(p_pic_out));
   }
   GtkWidget *p_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
   gtk_widget_set_valign(p_text, GTK_ALIGN_CENTER);
   gtk_box_append(GTK_BOX(p_text),
                  _line_label("Original", PANGO_ELLIPSIZE_END));
   GtkWidget *p_hint =
      _line_label("hold Space to compare", PANGO_ELLIPSIZE_END);
   gtk_widget_add_css_class(p_hint, "caption");
   gtk_box_append(GTK_BOX(p_text), p_hint);
   gtk_box_append(GTK_BOX(p_row), p_text);
   gtk_widget_add_css_class(p_row, "dim-label");
   gtk_widget_add_css_class(p_row, GGAZE_ENHANCE_ORIGINAL_CLASS);
   return (p_row);
}

/* Append one card per preset (capped at the mask's 8 bits) to p_box,
 * recording each in p_out. */
static void
_build_cards(GtkWidget *p_box, const GPtrArray *p_presets, guint8 u_mask,
             gboolean b_thumbnails, EnhanceUIWidgets *p_out) {
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

/* A non-focusable button on c_action holding p_child, with c_tip as its
 * tooltip -- or, when that is NULL, the table's title + keys, like the
 * header bar's buttons. */
static GtkWidget *
_actionable(const char *c_action, GtkWidget *p_child, const char *c_tip) {
   GtkWidget *p_btn = gtk_button_new();
   gtk_button_set_child(GTK_BUTTON(p_btn), p_child);
   gtk_actionable_set_action_name(GTK_ACTIONABLE(p_btn), c_action);
   char *c_own =
      c_tip == NULL ? shortcuts_tooltip_for_action(c_action) : g_strdup(c_tip);
   gtk_widget_set_tooltip_text(p_btn, c_own);
   g_free(c_own);
   gtk_widget_set_can_focus(p_btn, FALSE);
   return (p_btn);
}

/* The content of an action button: an icon, an optional label (NULL: the
 * icon alone), then the key badge. c_keys comes from shortcuts.c's
 * table. */
static GtkWidget *
_button_content(const char *c_icon, const char *c_label, const char *c_keys) {
   GtkWidget *p_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_box_append(GTK_BOX(p_row), gtk_image_new_from_icon_name(c_icon));
   if (c_label != NULL) {
      GtkWidget *p_lbl = _line_label(c_label, PANGO_ELLIPSIZE_END);
      gtk_widget_set_hexpand(p_lbl, TRUE);
      gtk_box_append(GTK_BOX(p_row), p_lbl);
   } else {
      gtk_widget_set_halign(p_row, GTK_ALIGN_CENTER);
   }
   gtk_box_append(GTK_BOX(p_row), _key_badge(c_keys));
   return (p_row);
}

/* A button on c_action showing an icon, c_label (nullable) and the keys
 * the hint bars list for the action. */
static GtkWidget *
_action_button(const char *c_icon, const char *c_label, const char *c_action) {
   char      *c_keys = shortcuts_hint_keys_for_action(c_action);
   GtkWidget *p_btn =
      _actionable(c_action, _button_content(c_icon, c_label, c_keys), NULL);
   g_free(c_keys);
   return (p_btn);
}

/* The Transform section: one row of four compact icon buttons -- crop,
 * straighten, the two quarter turns -- each with its key badge and the
 * table's description as its tooltip. */
static void
_build_transform(GtkWidget *p_box) {
   gtk_box_append(GTK_BOX(p_box), _section_label("Transform"));
   GtkWidget *p_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
   gtk_box_set_homogeneous(GTK_BOX(p_row), TRUE);
   gtk_box_append(GTK_BOX(p_row),
                  _action_button("edit-cut-symbolic", NULL, "win.crop"));
   gtk_box_append(GTK_BOX(p_row), _action_button("rotation-allowed-symbolic",
                                                 NULL, "win.straighten"));
   gtk_box_append(GTK_BOX(p_row), _action_button("object-rotate-left-symbolic",
                                                 NULL, "win.rotate-ccw"));
   gtk_box_append(GTK_BOX(p_row), _action_button("object-rotate-right-symbolic",
                                                 NULL, "win.rotate-cw"));
   gtk_box_append(GTK_BOX(p_box), p_row);
}

/* The Save button: its key row over the name of the file it will write. */
static GtkWidget *
_build_save(EnhanceUIWidgets *p_out) {
   char      *c_keys = shortcuts_hint_keys_for_action("win.enhance-save");
   GtkWidget *p_col  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
   gtk_box_append(GTK_BOX(p_col), _button_content("document-save-symbolic",
                                                  "Save copy", c_keys));
   g_free(c_keys);
   GtkWidget *p_target = _line_label(NULL, PANGO_ELLIPSIZE_MIDDLE);
   gtk_widget_add_css_class(p_target, "caption");
   gtk_box_append(GTK_BOX(p_col), p_target);
   GtkWidget *p_btn = _actionable("win.enhance-save", p_col, NULL);
   gtk_widget_add_css_class(p_btn, "suggested-action");
   gtk_widget_set_hexpand(p_btn, TRUE);
   p_out->p_save_btn    = p_btn;
   p_out->p_save_target = p_target;
   return (p_btn);
}

/* Undo and Redo (7i2) side by side, the same width, each with its key.
 * Built insensitive: a fresh panel's controller says when there is
 * something to step through. */
static GtkWidget *
_build_history(EnhanceUIWidgets *p_out) {
   GtkWidget *p_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
   gtk_box_set_homogeneous(GTK_BOX(p_row), TRUE);
   p_out->p_undo_btn =
      _action_button("edit-undo-symbolic", "Undo", "win.edit-undo");
   p_out->p_redo_btn =
      _action_button("edit-redo-symbolic", "Redo", "win.edit-redo");
   gtk_widget_set_sensitive(p_out->p_undo_btn, FALSE);
   gtk_widget_set_sensitive(p_out->p_redo_btn, FALSE);
   gtk_box_append(GTK_BOX(p_row), p_out->p_undo_btn);
   gtk_box_append(GTK_BOX(p_row), p_out->p_redo_btn);
   return (p_row);
}

/* The Actions: the one-line save state, then Save copy and Revert side by
 * side, then Undo / Redo. Pinned below the card scroller: "how do I keep
 * (or drop, or step back from) this" must never be what scrolled away.
 * Revert shows a "revert" icon now that the undo arrow is Undo's. */
static void
_build_actions(GtkWidget *p_box, EnhanceUIWidgets *p_out) {
   GtkWidget *p_state = _line_label(NULL, PANGO_ELLIPSIZE_MIDDLE);
   gtk_widget_set_margin_top(p_state, 4);
   gtk_box_append(GTK_BOX(p_box), p_state);
   p_out->p_state   = p_state;
   GtkWidget *p_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
   gtk_box_append(GTK_BOX(p_row), _build_save(p_out));
   gtk_box_append(GTK_BOX(p_row), _action_button("document-revert-symbolic",
                                                 "Revert", "win.edit-revert"));
   gtk_box_append(GTK_BOX(p_box), p_row);
   gtk_box_append(GTK_BOX(p_box), _build_history(p_out));
}

/* The title row: "Edit" (the window title already names the file) and the
 * close button, flat, showing both its keys -- `a` (the same action) and
 * Esc (win.back, which closes the panel first). */
static GtkWidget *
_build_title_row(void) {
   GtkWidget *p_row   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
   GtkWidget *p_title = gtk_label_new("Edit");
   gtk_label_set_xalign(GTK_LABEL(p_title), 0.0f);
   gtk_widget_add_css_class(p_title, "title-4");
   gtk_widget_set_hexpand(p_title, TRUE);
   gtk_box_append(GTK_BOX(p_row), p_title);
   char *c_close =
      shortcuts_hint_keys(GGAZE_KEY_MODE_PANEL, SHORTCUTS_HINT_CLOSE);
   GtkWidget *p_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_box_append(GTK_BOX(p_content), _key_badge(c_close));
   gtk_box_append(GTK_BOX(p_content),
                  gtk_image_new_from_icon_name("window-close-symbolic"));
   char *c_tip = g_strdup_printf("Close (%s)", c_close);
   gtk_box_append(GTK_BOX(p_row), _actionable("win.enhance", p_content, c_tip));
   gtk_widget_add_css_class(gtk_widget_get_last_child(p_row), "flat");
   g_free(c_tip);
   g_free(c_close);
   return (p_row);
}

/* The panel root: a narrow vertical column with the panel's margins. */
static GtkWidget *
_panel_root(void) {
   GtkWidget *p_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
   gtk_widget_set_margin_start(p_panel, 8);
   gtk_widget_set_margin_end(p_panel, 8);
   gtk_widget_set_margin_top(p_panel, 6);
   gtk_widget_set_margin_bottom(p_panel, 8);
   gtk_widget_set_size_request(p_panel, GGAZE_ENHANCE_PANEL_WIDTH, -1);
   gtk_widget_set_hexpand(p_panel, FALSE);
   gtk_widget_set_vexpand(p_panel, TRUE);
   gtk_widget_add_css_class(p_panel, GGAZE_ENHANCE_PANEL_CLASS);
   return (p_panel);
}

/* The preset cards in a vertical scroller as tall as the cards when there
 * is room (propagate-natural-height) and shorter when there is not: the
 * eight built-in rows fit an 800 px window, but user presets or a short
 * window can outgrow it, and then the cards scroll rather than the tools
 * and the save below them. Nothing expands, so the sections stay packed
 * together at the top and any spare height is left below them. */
static GtkWidget *
_build_card_scroller(const GPtrArray *p_presets, guint8 u_mask,
                     gboolean b_thumbnails, EnhanceUIWidgets *p_out) {
   GtkWidget *p_cards = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
   _build_cards(p_cards, p_presets, u_mask, b_thumbnails, p_out);
   GtkWidget *p_scroll = gtk_scrolled_window_new();
   gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p_scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(p_scroll), TRUE);
   gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(p_scroll), p_cards);
   return (p_scroll);
}

GtkWidget *
enhance_ui_build_panel(const GPtrArray *p_presets, guint8 u_mask,
                       gboolean b_thumbnails, EnhanceUIWidgets *p_out) {
   g_return_val_if_fail(p_out != NULL, NULL);
   memset(p_out, 0, sizeof(*p_out)); /* label-only mode leaves pics NULL */
   GtkWidget *p_panel = _panel_root();
   gtk_box_append(GTK_BOX(p_panel), _build_title_row());
   p_out->p_original = _build_original(b_thumbnails, &p_out->p_original_pic);
   gtk_box_append(GTK_BOX(p_panel), p_out->p_original);
   gtk_box_append(GTK_BOX(p_panel), _section_label("Presets"));
   gtk_box_append(GTK_BOX(p_panel),
                  _build_card_scroller(p_presets, u_mask, b_thumbnails, p_out));
   _build_transform(p_panel);
   _build_actions(p_panel, p_out);
   p_out->p_panel = p_panel;
   return (p_panel);
}

void
enhance_ui_set_save_state(GtkWidget *p_state, GtkWidget *p_save_btn,
                          gboolean b_active, gboolean b_saved,
                          const char *c_saved) {
   g_return_if_fail(GTK_IS_LABEL(p_state));
   char *c_text = NULL;
   if (!b_active) {
      c_text = g_strdup("No edits yet");
   } else if (b_saved) {
      c_text = c_saved != NULL ? g_strdup_printf("Saved as %s", c_saved)
                               : g_strdup("Saved");
   } else {
      c_text = g_strdup("Unsaved edits · original kept");
   }
   gtk_label_set_text(GTK_LABEL(p_state), c_text);
   gtk_widget_set_tooltip_text(p_state, c_text); /* when it is ellipsized */
   g_free(c_text);
   if (p_save_btn != NULL) {
      gtk_widget_set_sensitive(p_save_btn, b_active);
   }
}

void
enhance_ui_set_save_target(GtkWidget *p_target, const char *c_name) {
   g_return_if_fail(GTK_IS_LABEL(p_target));
   char *c_text =
      c_name != NULL ? g_strdup_printf("as %s", c_name) : g_strdup("");
   gtk_label_set_text(GTK_LABEL(p_target), c_text);
   /* The label middle-ellipsizes a long name; the tooltip has all of it. */
   gtk_widget_set_tooltip_text(p_target, c_name);
   g_free(c_text);
}
