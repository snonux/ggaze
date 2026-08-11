#include "enhance-ui.h"

#include <glib.h>
#include <gtk/gtk.h>

#include "enhancer.h"
#include "popup_list.h"

/* A preview picture claims the whole cell: expand in both directions, and keep
 * a small floor so a cell can never collapse to nothing before the texture
 * arrives (a GtkPicture with no paintable measures zero). CONTENT_FIT_CONTAIN
 * on top of this is what keeps the image's aspect ratio inside the cell, so no
 * caller needs to compute a width/height pair by hand. */
void
enhance_ui_expand_preview(GtkWidget *p_pic) {
   gtk_widget_set_hexpand(p_pic, TRUE);
   gtk_widget_set_vexpand(p_pic, TRUE);
   gtk_widget_set_halign(p_pic, GTK_ALIGN_FILL);
   gtk_widget_set_valign(p_pic, GTK_ALIGN_FILL);
   gtk_widget_set_size_request(p_pic, 96, 64);
}

/* One picture-over-label preview card. Returns the button and hands back its
 * GtkPicture so the caller can store it in the right field. */
GtkWidget *
enhance_ui_preview_card(const char *c_label, GtkWidget **p_pic_out) {
   GtkWidget *p_btn = gtk_button_new();
   GtkWidget *p_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
   GtkWidget *p_pic = gtk_picture_new();
   GtkWidget *p_lbl = gtk_label_new(c_label);
   gtk_picture_set_content_fit(GTK_PICTURE(p_pic), GTK_CONTENT_FIT_CONTAIN);
   enhance_ui_expand_preview(p_pic);
   gtk_widget_set_halign(p_lbl, GTK_ALIGN_CENTER);
   gtk_box_append(GTK_BOX(p_box), p_pic);
   gtk_box_append(GTK_BOX(p_box), p_lbl);
   gtk_button_set_child(GTK_BUTTON(p_btn), p_box);
   gtk_widget_set_halign(p_btn, GTK_ALIGN_FILL);
   *p_pic_out = p_pic;
   return (p_btn);
}

/* Pick the column count that makes the thumbnails BIGGEST for a gallery of
 * i_width x i_height, then pin the flow box to it.
 *
 * Maximising fitted image area is the same objective the original
 * _enhance_resize_gallery had, and it is the right one: a column count that
 * tiles the cells perfectly can still be the worse layout. With 10 cards in a
 * 790x590 window, 5x2 leaves no empty cell but gives each card a 158x295 slot
 * that a landscape photo fills only 158x105 of, while 4x3 wastes two cells and
 * still shows a far larger 197x131 image in each. Cell ASPECT dominates; empty
 * cells in the last row do not.
 *
 * What was wrong before was never this arithmetic -- it was running it on
 * every frame against the window it was itself resizing. This runs ONCE, from
 * the size the gallery is about to be given, and nothing re-runs it: a later
 * resize keeps the column count and simply lets the cells grow, which is
 * stable by construction. i_cell_h subtracts a label strip; the exact figure
 * only has to be in the right neighbourhood, since it merely ranks candidates
 * against each other. */
void
enhance_ui_apply_grid_columns(GtkFlowBox *p_gallery, int i_items, int i_width,
                              int i_height) {
   if (p_gallery == NULL) {
      return;
   }
   i_items         = MAX(1, i_items);
   int i_columns   = 1;
   int i_best_area = -1;
   for (int i_try = 1; i_try <= i_items; i_try++) {
      int i_rows   = (i_items + i_try - 1) / i_try;
      int i_cell_w = i_width / i_try - 8;
      int i_cell_h = i_height / i_rows - 40; /* card padding + label */
      if (i_cell_w <= 0 || i_cell_h <= 0) {
         continue;
      }
      /* The image is letterboxed into the cell, so its area is set by
       * whichever dimension runs out first at a typical 3:2 photo aspect. */
      int i_fit_w = MIN(i_cell_w, i_cell_h * 3 / 2);
      int i_area  = i_fit_w * (i_fit_w * 2 / 3);
      if (i_area > i_best_area) {
         i_best_area = i_area;
         i_columns   = i_try;
      }
   }
   gtk_flow_box_set_min_children_per_line(p_gallery, (guint)i_columns);
   gtk_flow_box_set_max_children_per_line(p_gallery, (guint)i_columns);
}

/* Build the popover's title row: "Enhance <basename>:" (or a bare "Enhance:"
 * if c_basename is NULL). */
static void
_build_title(const char *c_basename, GtkWidget *p_box) {
   char *c_title = NULL;
   if (c_basename != NULL) {
      c_title = g_strdup_printf("Enhance %s:", c_basename);
   }
   GtkWidget *p_lbl = gtk_label_new(c_title != NULL ? c_title : "Enhance:");
   gtk_widget_set_halign(p_lbl, GTK_ALIGN_START);
   gtk_box_append(GTK_BOX(p_box), p_lbl);
   g_free(c_title);
}

/* The "Current" card: the layered result of every enabled preset, i.e. what
 * the large view is showing. Read-only on purpose -- it reports a state rather
 * than offering a toggle, so it takes no clicks and no focus (can_target /
 * can_focus FALSE) while keeping the same card framing as its neighbours. */
static GtkWidget *
_build_current_card(GtkWidget **p_pic_out) {
   GtkWidget *p_pic = NULL;
   GtkWidget *p_btn = enhance_ui_preview_card("Current", &p_pic);
   gtk_widget_set_can_target(p_btn, FALSE);
   gtk_widget_set_can_focus(p_btn, FALSE);
   *p_pic_out = p_pic;
   return (p_btn);
}

/* The "0 Original" row (idx -1). In preview mode it is a picture card whose
 * GtkPicture is handed back via p_pic_out; in compact mode it is a plain
 * left-aligned label button. The caller wires the clicked handler. */
static GtkWidget *
_build_original_button(gboolean b_previews, GtkWidget **p_pic_out) {
   GtkWidget *p_btn = NULL;
   if (b_previews) {
      p_btn = enhance_ui_preview_card("0  Original", p_pic_out);
   } else {
      p_btn = gtk_button_new();
      gtk_button_set_label(GTK_BUTTON(p_btn), "0  Original");
      /* The compact popover keeps the left-aligned list shape a vertical menu
       * wants; preview cards FILL their share of the grid (set in
       * enhance_ui_preview_card). */
      gtk_widget_set_halign(p_btn, GTK_ALIGN_START);
   }
   g_object_set_data(G_OBJECT(p_btn), "idx", GINT_TO_POINTER(-1));
   return (p_btn);
}

GtkWidget *
enhance_ui_build_content(const GPtrArray *p_presets, const char *c_basename,
                         guint8 u_mask, gboolean b_previews,
                         EnhanceUIWidgets *p_out) {
   g_return_val_if_fail(p_out != NULL, NULL);
   /* Zero every slot first so the compact-mode branches that never assign
    * leave NULL rather than garbage. */
   p_out->p_original_btn = NULL;
   p_out->p_original_pic = NULL;
   p_out->p_current_pic  = NULL;
   p_out->p_gallery      = NULL;
   p_out->p_scroll       = NULL;
   p_out->u_n_presets    = 0;
   for (guint i = 0; i < G_N_ELEMENTS(p_out->p_btns); i++) {
      p_out->p_btns[i] = NULL;
      p_out->p_pics[i] = NULL;
   }

   GtkWidget *p_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
   gtk_widget_set_margin_start(p_box, 8);
   gtk_widget_set_margin_end(p_box, 8);
   gtk_widget_set_margin_top(p_box, 8);
   gtk_widget_set_margin_bottom(p_box, 8);
   _build_title(c_basename, p_box);

   GtkWidget *p_gallery = NULL;
   if (b_previews) {
      p_gallery = gtk_flow_box_new();
      gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(p_gallery), TRUE);
      gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(p_gallery),
                                      GTK_SELECTION_NONE);
      gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(p_gallery), 4);
      gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(p_gallery), 4);
      gtk_widget_set_hexpand(p_gallery, TRUE);
      gtk_widget_set_vexpand(p_gallery, TRUE);
      GtkWidget *p_scroll = gtk_scrolled_window_new();
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p_scroll),
                                     GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
      /* The scroll area takes ALL the leftover room in the window, and takes
       * it from GTK's layout rather than from a size request computed off the
       * window's own dimensions -- which is what previously coupled the
       * gallery's size back to itself. */
      gtk_widget_set_hexpand(p_scroll, TRUE);
      gtk_widget_set_vexpand(p_scroll, TRUE);
      gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(p_scroll), p_gallery);
      gtk_box_append(GTK_BOX(p_box), p_scroll);
      p_out->p_gallery = p_gallery;
      p_out->p_scroll  = p_scroll;
   }

   GtkWidget *p_btn0 =
      _build_original_button(b_previews, &p_out->p_original_pic);
   p_out->p_original_btn = p_btn0;
   if (b_previews) {
      /* Original then Current, so the two whole-image references sit side by
       * side ahead of the per-preset cards. */
      gtk_flow_box_append(GTK_FLOW_BOX(p_gallery), p_btn0);
      gtk_flow_box_append(GTK_FLOW_BOX(p_gallery),
                          _build_current_card(&p_out->p_current_pic));
   }

   guint u_n = p_presets != NULL ? p_presets->len : 0;
   if (u_n > G_N_ELEMENTS(p_out->p_btns)) {
      u_n = G_N_ELEMENTS(p_out->p_btns); /* the mask is 8 bits wide */
   }
   for (guint i = 0; i < u_n; i++) {
      const EnhancerPreset *p_pr = g_ptr_array_index((GPtrArray *)p_presets, i);
      char                 *c_lbl = popup_list_row_label(i, p_pr->c_name);
      GtkWidget            *p_btn = gtk_button_new();
      if (b_previews) {
         GtkWidget *p_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
         GtkWidget *p_pic = gtk_picture_new();
         GtkWidget *p_lbl = gtk_label_new(c_lbl);
         gtk_picture_set_content_fit(GTK_PICTURE(p_pic),
                                     GTK_CONTENT_FIT_CONTAIN);
         enhance_ui_expand_preview(p_pic);
         gtk_widget_set_halign(p_lbl, GTK_ALIGN_CENTER);
         gtk_box_append(GTK_BOX(p_row), p_pic);
         gtk_box_append(GTK_BOX(p_row), p_lbl);
         gtk_button_set_child(GTK_BUTTON(p_btn), p_row);
         p_out->p_pics[i] = p_pic;
      } else {
         gtk_button_set_label(GTK_BUTTON(p_btn), c_lbl);
      }
      gtk_widget_set_halign(p_btn,
                            b_previews ? GTK_ALIGN_FILL : GTK_ALIGN_START);
      g_object_set_data(G_OBJECT(p_btn), "idx", GINT_TO_POINTER((gint)i));
      if ((u_mask & (guint8)(1u << i)) != 0) {
         gtk_widget_add_css_class(p_btn, "ggaze-enhance-on");
      }
      if (b_previews) {
         gtk_flow_box_append(GTK_FLOW_BOX(p_gallery), p_btn);
      } else {
         gtk_box_append(GTK_BOX(p_box), p_btn);
      }
      p_out->p_btns[i] = p_btn;
      g_free(c_lbl);
   }
   p_out->u_n_presets = u_n;

   if (!b_previews) {
      gtk_box_append(GTK_BOX(p_box), p_btn0);
   }

   GtkWidget *p_hint = gtk_label_new("s  Save enhanced copy");
   gtk_widget_set_halign(p_hint, GTK_ALIGN_START);
   gtk_widget_set_margin_top(p_hint, 8);
   gtk_widget_add_css_class(p_hint, "dim-label");
   gtk_box_append(GTK_BOX(p_box), p_hint);
   return (p_box);
}