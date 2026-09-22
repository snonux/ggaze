#ifndef GGAZE_ENHANCE_UI_H
#define GGAZE_ENHANCE_UI_H

/*:*
 * ggaze — enhance side-panel widget construction
 *
 * The `a` enhance UI is a narrow panel that sits BESIDE the large view, so
 * the image being judged keeps the whole viewer and the choices are small
 * cards down the side: a title, an "Original" card, one card per preset
 * (a bounded preview thumbnail over its hotkey label, or the label alone
 * when thumbnails are disabled in Preferences), a save-state line that says
 * in words whether the preview is unsaved, a Save button, and a key hint.
 * This module builds that panel and the widgets it is made of; it owns NO
 * state and depends on no GgazeWindow, no GEGL, and no signal wiring.
 *
 * The caller passes the data the build needs (the preset list, the current
 * file's basename for the title, the active mask for the initial card
 * highlights, and the thumbnail/label-only choice) and receives every
 * widget it must touch later back through EnhanceUIWidgets, so it can store
 * them in its own fields and connect its own card-toggle handler. Each card
 * button carries its index in the "idx" GObject data (0..n-1 for presets,
 * -1 for Original), which is what the controller's toggle handler reads, so
 * the caller's signal wiring is a plain loop over the returned buttons.
 *
 * The cards are deliberately NOT focusable: the panel lives inside the
 * window's widget tree, and a focused GtkButton activates on Space -- the
 * very key that is hold-to-compare here. Mouse clicks still work; the
 * keyboard path is the digits, which the window binds (docs/gegl.md).
 *
 * Compiled only when GEGL is enabled (alongside enhancer.c): every caller is
 * under #if GGAZE_HAVE_GEGL, and there is no enhance UI without GEGL. The
 * module itself uses no GEGL type -- it is grouped with the enhancer because
 * it is the enhancer's UI.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <glib.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The panel's requested width in px. Wide enough for a 3:2 thumbnail the eye
 * can compare against its neighbours, narrow enough to leave the viewer most
 * of an 800 px window. */
#define GGAZE_ENHANCE_PANEL_WIDTH 208

/* The CSS class on the panel root, so tests and styling can find it without
 * depending on the widget tree's shape. */
#define GGAZE_ENHANCE_PANEL_CLASS "ggaze-enhance-panel"
/* The CSS class on every card (Original + presets) -- the buttons that carry
 * an "idx" datum; the Save button is not one. */
#define GGAZE_ENHANCE_CARD_CLASS "ggaze-enhance-card"

/* Widgets built by enhance_ui_build_panel that the caller must store (the
 * card buttons + preview pictures, the title and save-state labels, the Save
 * button). Picture entries are NULL in label-only mode. u_n_presets is the
 * number of p_btns/p_pics entries actually filled (<= 8). */
typedef struct {
   GtkWidget *p_panel;        /* the panel root (a vertical box) */
   GtkWidget *p_title;        /* "Enhance <basename>" label */
   GtkWidget *p_original_btn; /* the "0 Original" card (idx -1) */
   GtkWidget *p_original_pic; /* its GtkPicture (thumbnail mode only) */
   GtkWidget *p_btns[8];      /* preset cards (idx 0..n-1) */
   GtkWidget *p_pics[8];      /* preset preview pictures (thumbnail mode) */
   guint      u_n_presets;    /* number of p_btns/p_pics entries filled */
   GtkWidget *p_state;        /* save-state line ("Unsaved -- s saves...") */
   GtkWidget *p_save_btn;     /* Save button, bound to win.enhance-save */
} EnhanceUIWidgets;

/* Build the side panel (title + Original/preset cards + save state + Save
 * button + key hint) and return its root, filling p_out with every widget
 * the caller must store. The caller owns the returned panel and all widgets
 * in p_out; it is not yet parented anywhere.
 *
 * c_basename (nullable) is the current file's basename for the title
 * ("Enhance <basename>"); NULL gives a bare "Enhance".
 * u_mask is the current enhance mask; each enabled preset's card gets the
 * "ggaze-enhance-on" CSS class at build time.
 * b_thumbnails selects picture cards vs label-only cards.
 *
 * Every card button (Original + presets) carries its index in the "idx"
 * GObject data (-1 for Original, 0..n-1 for presets). This function connects
 * NO signals and stores nothing -- the caller wires its toggle handler and
 * stores the widgets. */
GtkWidget *enhance_ui_build_panel(const GPtrArray *p_presets,
                                  const char *c_basename, guint8 u_mask,
                                  gboolean          b_thumbnails,
                                  EnhanceUIWidgets *p_out);

/* Retitle the panel for another file ("Enhance <basename>", or "Enhance"
 * when c_basename is NULL). Pure label update. */
void enhance_ui_set_title(GtkWidget *p_title, const char *c_basename);

/* Put the save state into words on p_state and enable/disable p_save_btn to
 * match: no preset on -> "Pick a preset..." (button insensitive); a preview
 * that is on screen but unsaved -> says so and names the save keys; a saved
 * one -> "Saved as <c_saved>" (c_saved nullable, then a generic "Saved").
 * Pure widget update. */
void enhance_ui_set_save_state(GtkWidget *p_state, GtkWidget *p_save_btn,
                               gboolean b_active, gboolean b_saved,
                               const char *c_saved);

G_END_DECLS

#endif /* GGAZE_ENHANCE_UI_H */
