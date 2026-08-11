#ifndef GGAZE_ENHANCE_UI_H
#define GGAZE_ENHANCE_UI_H

/*:*
 * ggaze — enhance gallery/popover widget construction
 *
 * The `a` enhance UI has two forms: a resizable gallery window (thumbnail
 * previews) and a compact popover (label-only rows). Both share the same
 * content: a title, an "Original" row, a read-only "Current" card (preview
 * mode only), one row per preset, and a hint label. This module builds that
 * content and the widgets it is made of; it owns NO state and depends on no
 * GgazeWindow, no GEGL, and no signal wiring.
 *
 * The caller passes the data the build needs (the preset list, the current
 * file's basename for the title, the active mask for the initial row
 * highlights, and the preview/compact choice) and receives every built
 * widget back through EnhanceUIWidgets, so the caller can store them in its
 * own fields and connect its own row-toggle handler. Each row button carries
 * its index in the "idx" GObject data (0..n-1 for presets, -1 for Original),
 * matching what the existing _enhance_row_toggle handler reads, so the
 * caller's signal wiring is a plain loop over the returned buttons.
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

/* Widgets built by enhance_ui_build_content that the caller must store (the
 * preset row buttons + preview pictures, the Original/Current cards'
 * pictures, and the gallery flow box / scroll). Entries are NULL when the
 * corresponding feature was not built (compact mode builds no gallery, no
 * preview pictures, and no Current card). u_n_presets is the number of
 * p_btns/p_pics entries actually filled (<= 8). */
typedef struct {
   GtkWidget *p_original_btn; /* the "0 Original" row button (idx -1) */
   GtkWidget *p_original_pic; /* its GtkPicture (preview mode only) */
   GtkWidget *p_current_pic;  /* the "Current" card's GtkPicture
                               * (preview mode only) */
   GtkWidget *p_gallery;      /* the GtkFlowBox (preview mode only) */
   GtkWidget *p_scroll;       /* the scrolled window (preview mode only) */
   GtkWidget *p_btns[8];      /* preset row buttons (idx 0..n-1) */
   GtkWidget *p_pics[8];      /* preset preview pictures (preview mode only) */
   guint      u_n_presets;    /* number of p_btns/p_pics entries filled */
} EnhanceUIWidgets;

/* Expand a preview GtkPicture to fill its cell (hexpand/vexpand, FILL align,
 * a 96x64 floor so a cell never collapses before its texture arrives). Pure
 * widget setup. */
void enhance_ui_expand_preview(GtkWidget *p_pic);

/* One picture-over-label preview card. Returns the button and hands back its
 * GtkPicture via p_pic_out. Pure widget construction. */
GtkWidget *enhance_ui_preview_card(const char *c_label, GtkWidget **p_pic_out);

/* Pin the gallery flow box to the column count that maximises fitted image
 * area for an i_width x i_height gallery of i_items cards. A no-op when
 * p_gallery is NULL. Pure (operates only on p_gallery). */
void enhance_ui_apply_grid_columns(GtkFlowBox *p_gallery, int i_items,
                                   int i_width, int i_height);

/* Build the enhance content box (title + Original/Current/preset rows + hint)
 * and return it, filling p_out with every widget the caller must store. The
 * caller owns the returned box and all widgets in p_out.
 *
 * c_basename (nullable) is the current file's basename for the title
 * ("Enhance <basename>:"); NULL gives a bare "Enhance:".
 * u_mask is the current enhance mask; each enabled preset's row gets the
 * "ggaze-enhance-on" CSS class at build time.
 * b_previews selects the gallery (thumbnail previews) vs compact (label-only)
 * layout.
 *
 * Every row button (Original + presets) carries its index in the "idx"
 * GObject data (-1 for Original, 0..n-1 for presets). This function connects
 * NO signals and stores nothing -- the caller wires its row-toggle handler
 * and stores the widgets. */
GtkWidget *enhance_ui_build_content(const GPtrArray *p_presets,
                                    const char *c_basename, guint8 u_mask,
                                    gboolean          b_previews,
                                    EnhanceUIWidgets *p_out);

G_END_DECLS

#endif /* GGAZE_ENHANCE_UI_H */