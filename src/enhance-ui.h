#ifndef GGAZE_ENHANCE_UI_H
#define GGAZE_ENHANCE_UI_H

/*:*
 * ggaze — edit side-panel widget construction
 *
 * The `a` EDIT PANEL is a narrow column that sits BESIDE the large view, so
 * the image being judged keeps the whole viewer, and it is the one home of
 * every edit (6i2). Compact, top to bottom, so everything fits an 800 px
 * tall window without scrolling:
 *
 *   title      "Edit" (the window title already names the file) and a
 *              flat close button showing its keys (a/Esc);
 *   Original   a small reference thumbnail, "hold Space to compare" --
 *              dim and frameless, visibly not a preset;
 *   Presets    one ROW card per preset: a small preview thumbnail (or
 *              none when thumbnails are disabled in Preferences), "1
 *              Auto-fix", and a check mark -- an enabled card is
 *              highlighted and checked;
 *   Transform  one row of four icon buttons: crop, straighten, rotate
 *              left, rotate right, each with its key badge and a tooltip;
 *   actions    a one-line save state, then Save copy (naming the file it
 *              will write, "as <name>") and Revert side by side, and
 *              under them Undo and Redo (7i2) -- insensitive while there
 *              is nothing to undo / redo (the controller's call).
 *
 * Every button shows its key, read from shortcuts.c's table
 * (shortcuts_hint_keys_for_action), and every panel key has a button, so
 * the panel, the key-hint bar and the `?` help say the same thing. The
 * buttons are GtkActionables on the win.* actions the keys fire: a click
 * and a key press are one path.
 *
 * This module builds that panel and owns NO state; it depends on no
 * GgazeWindow, no GEGL, and wires no signal of its own. The caller passes
 * the data the build needs (the preset list, the active mask for the
 * initial card highlights, and the thumbnail/label-only choice) and
 * receives every widget it must touch later back through
 * EnhanceUIWidgets. Each preset card carries its index
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

/* The panel's requested width in px: a row card's thumbnail beside a
 * preset name, and Save copy beside Revert, while leaving the viewer most
 * of an 800 px wide window. */
#define GGAZE_ENHANCE_PANEL_WIDTH 232

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
/* The CSS class on a card's check mark: window.c's stylesheet shows it
 * only on a card that also has the "ggaze-enhance-on" class. */
#define GGAZE_ENHANCE_CHECK_CLASS "ggaze-enhance-check"

/* Widgets built by enhance_ui_build_panel that the caller must store (the
 * card buttons + preview pictures, the save-state and save-target labels,
 * the Save button). Picture entries are NULL in label-only mode.
 * u_n_presets is the number of p_btns/p_pics entries actually filled
 * (<= 8). */
typedef struct {
   GtkWidget *p_panel;        /* the panel root (a vertical box) */
   GtkWidget *p_original;     /* the Original reference (not a button) */
   GtkWidget *p_original_pic; /* its GtkPicture (thumbnail mode only) */
   GtkWidget *p_btns[8];      /* preset cards (idx 0..n-1) */
   GtkWidget *p_pics[8];      /* preset preview pictures (thumbnail mode) */
   guint      u_n_presets;    /* number of p_btns/p_pics entries filled */
   GtkWidget *p_state;        /* save-state line ("Unsaved edits ...") */
   GtkWidget *p_save_btn;     /* Save button, bound to win.enhance-save */
   GtkWidget *p_save_target;  /* the file Save would write, under it */
   GtkWidget *p_undo_btn;     /* Undo, bound to win.edit-undo (the caller
                               * keeps it insensitive with nothing to undo) */
   GtkWidget *p_redo_btn;     /* Redo, bound to win.edit-redo (likewise) */
} EnhanceUIWidgets;

/* Build the side panel (see the header) and return its root, filling
 * p_out with every widget the caller must store. The caller owns the
 * returned panel and all widgets in p_out; it is not yet parented
 * anywhere.
 *
 * u_mask is the current enhance mask; each enabled preset's card gets the
 * "ggaze-enhance-on" CSS class at build time.
 * b_thumbnails selects thumbnail rows vs label-only rows.
 *
 * Every preset card carries its index in the "idx" GObject data (0..n-1).
 * This function connects NO signals and stores nothing -- the caller wires
 * its toggle handler and stores the widgets; the action buttons resolve
 * their win.* actions once the panel is inside the window. */
GtkWidget *enhance_ui_build_panel(const GPtrArray *p_presets, guint8 u_mask,
                                  gboolean          b_thumbnails,
                                  EnhanceUIWidgets *p_out);

/* Put the save state into one line on p_state and enable/disable
 * p_save_btn to match: nothing edited -> "No edits yet" (button
 * insensitive); edits on screen but unsaved -> says so, and that the
 * original is kept; saved -> "Saved as <c_saved>" (c_saved nullable, then
 * a generic "Saved"). Pure widget update. */
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
