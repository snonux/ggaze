#ifndef GGAZE_ENHANCE_UI_H
#define GGAZE_ENHANCE_UI_H

/*:*
 * ggaze — edit side-panel widget construction
 *
 * The `a` EDIT PANEL is a narrow column that sits BESIDE the large view, so
 * the image being judged keeps the whole viewer, and it is the one home of
 * every edit (6i2): three sections under a title --
 *
 *   Presets    an "Original" reference, then one card per preset (a
 *              bounded preview thumbnail over its hotkey label, or the
 *              label alone when thumbnails are disabled in Preferences);
 *   Transform  Crop, Straighten and the two quarter turns, as buttons;
 *   Actions    a save-state line, Save copy (naming the file it will
 *              write), Revert all, and Close.
 *
 * Every button shows its key, read from shortcuts.c's table
 * (shortcuts_hint_keys_for_action), so the panel, the key-hint bar and the
 * `?` help say the same thing. The buttons are GtkActionables on the win.*
 * actions the keys fire: a click and a key press are one path.
 *
 * This module builds that panel and owns NO state; it depends on no
 * GgazeWindow, no GEGL, and wires no signal of its own. The caller passes
 * the data the build needs (the preset list, the current file's basename
 * for the title, the active mask for the initial card highlights, and the
 * thumbnail/label-only choice) and receives every widget it must touch
 * later back through EnhanceUIWidgets. Each preset card carries its index
 * in the "idx" GObject data (0..n-1), which is what the controller's
 * toggle handler reads. The Original is a reference picture, not a card:
 * reverting is the Revert button's (x) job alone.
 *
 * Nothing in the panel is focusable: it lives inside the window's widget
 * tree, and a focused GtkButton activates on Space -- the very key that is
 * hold-to-compare here. Mouse clicks still work; the keyboard path is the
 * keys each button shows.
 *
 * Compiled only when GEGL is enabled (alongside enhancer.c): every caller is
 * under #if GGAZE_HAVE_GEGL, and there is no edit UI without GEGL. The
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
/* The CSS class on every preset card -- the buttons that carry an "idx"
 * datum; the Original reference and the action buttons are not cards. */
#define GGAZE_ENHANCE_CARD_CLASS "ggaze-enhance-card"
/* The CSS class on the Original reference (picture + label). */
#define GGAZE_ENHANCE_ORIGINAL_CLASS "ggaze-enhance-original"
/* The CSS class on the key label inside every panel button. */
#define GGAZE_ENHANCE_KEY_CLASS "ggaze-key"

/* Widgets built by enhance_ui_build_panel that the caller must store (the
 * card buttons + preview pictures, the title, save-state and save-target
 * labels, the Save button). Picture entries are NULL in label-only mode.
 * u_n_presets is the number of p_btns/p_pics entries actually filled
 * (<= 8). */
typedef struct {
   GtkWidget *p_panel;        /* the panel root (a vertical box) */
   GtkWidget *p_title;        /* "Edit <basename>" label */
   GtkWidget *p_original;     /* the Original reference (not a button) */
   GtkWidget *p_original_pic; /* its GtkPicture (thumbnail mode only) */
   GtkWidget *p_btns[8];      /* preset cards (idx 0..n-1) */
   GtkWidget *p_pics[8];      /* preset preview pictures (thumbnail mode) */
   guint      u_n_presets;    /* number of p_btns/p_pics entries filled */
   GtkWidget *p_state;        /* save-state line ("Unsaved edits ...") */
   GtkWidget *p_save_btn;     /* Save button, bound to win.enhance-save */
   GtkWidget *p_save_target;  /* the file Save would write, under it */
} EnhanceUIWidgets;

/* Build the side panel (title + the three sections) and return its root,
 * filling p_out with every widget the caller must store. The caller owns
 * the returned panel and all widgets in p_out; it is not yet parented
 * anywhere.
 *
 * c_basename (nullable) is the current file's basename for the title
 * ("Edit <basename>"); NULL gives a bare "Edit".
 * u_mask is the current enhance mask; each enabled preset's card gets the
 * "ggaze-enhance-on" CSS class at build time.
 * b_thumbnails selects picture cards vs label-only cards.
 *
 * Every preset card carries its index in the "idx" GObject data (0..n-1).
 * This function connects NO signals and stores nothing -- the caller wires
 * its toggle handler and stores the widgets; the action buttons resolve
 * their win.* actions once the panel is inside the window. */
GtkWidget *enhance_ui_build_panel(const GPtrArray *p_presets,
                                  const char *c_basename, guint8 u_mask,
                                  gboolean          b_thumbnails,
                                  EnhanceUIWidgets *p_out);

/* Retitle the panel for another file ("Edit <basename>", or "Edit" when
 * c_basename is NULL). Pure label update. */
void enhance_ui_set_title(GtkWidget *p_title, const char *c_basename);

/* Put the save state into words on p_state and enable/disable p_save_btn to
 * match: nothing edited -> says how to start (button insensitive); edits on
 * screen but unsaved -> says so and names the save key; saved -> "Saved as
 * <c_saved>" (c_saved nullable, then a generic "Saved"). Pure widget
 * update. */
void enhance_ui_set_save_state(GtkWidget *p_state, GtkWidget *p_save_btn,
                               gboolean b_active, gboolean b_saved,
                               const char *c_saved);

/* Name the file the next Save writes under the Save button ("as <name>"),
 * or clear it (c_name NULL: no file open, or no free name). Plain ASCII
 * around the name on purpose: an arrow glyph the UI font lacks sent pango
 * to a fallback font, and fontconfig leaks the charset it builds for that
 * font's coverage (caught by the ASan lane). */
void enhance_ui_set_save_target(GtkWidget *p_target, const char *c_name);

G_END_DECLS

#endif /* GGAZE_ENHANCE_UI_H */
