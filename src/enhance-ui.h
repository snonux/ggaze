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
 *   Presets    one ROW card per preset -- the eight built-ins, then the
 *              user's own from Preferences (ai2) -- in a list that
 *              scrolls when it does not fit: a small preview thumbnail
 *              (or none when thumbnails are disabled in Preferences),
 *              "1  Auto-fix" (the first GGAZE_ENHANCE_DIGIT_PRESETS rows
 *              show their digit; a row past them has none and shows its
 *              name alone), the strength of a tunable preset ("+0.5",
 *              8i2) and a check mark -- an enabled card is highlighted
 *              and checked, the SELECTED card (j / k) wears a ring, and
 *              under the selected card of a tunable preset its strength
 *              slider shows (one slider at a time, so the eight built-in
 *              rows still fit an 800 px tall window unscrolled);
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
 * Only the preset list scrolls: the title, the Original, Transform and
 * the Actions stay where they are, so "how do I keep this" never scrolls
 * away however many presets there are. Each card and its slider sit in
 * one ROW box (p_rows), which is what the caller scrolls into view when
 * the selection moves (enhance_ui_scroll_to_row).
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

#include "enhancer.h" /* GGAZE_ENHANCE_MAX_PRESETS: a card per row */
#include "preset-strength.h"

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
/* The CSS class on the selected card (j / k move it, 8i2): a ring,
 * distinct from the on state's fill. */
#define GGAZE_ENHANCE_SELECTED_CLASS "ggaze-enhance-selected"
/* The CSS class on a tunable card's strength slider (a GtkScale carrying
 * the card's "idx" datum) and on its value label. */
#define GGAZE_ENHANCE_SCALE_CLASS "ggaze-enhance-scale"
#define GGAZE_ENHANCE_VALUE_CLASS "ggaze-enhance-value"

/* Widgets built by enhance_ui_build_panel that the caller must store (the
 * card buttons + preview pictures, the save-state and save-target labels,
 * the Save button). Picture entries are NULL in label-only mode.
 * u_n_presets is the number of p_btns/p_pics entries actually filled
 * (<= GGAZE_ENHANCE_MAX_PRESETS: one per row the mask can reach). */
typedef struct {
   GtkWidget *p_panel;        /* the panel root (a vertical box) */
   GtkWidget *p_original;     /* the Original reference (not a button) */
   GtkWidget *p_original_pic; /* its GtkPicture (thumbnail mode only) */
   GtkWidget *p_scroll;       /* the preset list's GtkScrolledWindow */
   GtkWidget *p_rows[GGAZE_ENHANCE_MAX_PRESETS];   /* row i: its card and,
                                                    * under it, its slider */
   GtkWidget *p_btns[GGAZE_ENHANCE_MAX_PRESETS];   /* preset cards (idx
                                                    * 0..n-1) */
   GtkWidget *p_pics[GGAZE_ENHANCE_MAX_PRESETS];   /* preset preview pictures
                                                    * (thumbnail mode) */
   GtkWidget *p_scales[GGAZE_ENHANCE_MAX_PRESETS]; /* each tunable preset's
                                                    * strength slider, NULL
                                                    * for one without a
                                                    * tunable number; built
                                                    * hidden (the caller
                                                    * shows the selected
                                                    * card's) */
   GtkWidget *p_values[GGAZE_ENHANCE_MAX_PRESETS]; /* its strength label on
                                                    * the card, likewise
                                                    * NULL; the caller sets
                                                    * the text */
   guint      u_n_presets;   /* number of p_btns/p_pics entries filled */
   GtkWidget *p_state;       /* save-state line ("Unsaved edits ...") */
   GtkWidget *p_save_btn;    /* Save button, bound to win.enhance-save */
   GtkWidget *p_save_target; /* the file Save would write, under it */
   GtkWidget *p_undo_btn;    /* Undo, bound to win.edit-undo (the caller
                              * keeps it insensitive with nothing to undo) */
   GtkWidget *p_redo_btn;    /* Redo, bound to win.edit-redo (likewise) */
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
 * Every preset card carries its index in the "idx" GObject data (0..n-1),
 * and so does its slider. A slider's range and step are the preset's own
 * (EnhancerPreset.t_strength); its value, its visibility, the value label
 * and the selection ring are the caller's to set.
 * This function connects NO signals and stores nothing -- the caller wires
 * its toggle and slider handlers and stores the widgets; the action
 * buttons resolve their win.* actions once the panel is inside the
 * window. */
GtkWidget *enhance_ui_build_panel(const GPtrArray *p_presets, guint32 u_mask,
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

/* Show d_value on a tunable card: the label's text (p_value) and the
 * slider's position (p_scale) -- both nullable, both the preset p_s's.
 * Pure widget update; the caller blocks its own value-changed handler. */
void enhance_ui_set_strength(GtkWidget *p_value, GtkWidget *p_scale,
                             const PresetStrength *p_s, gdouble d_value);

/* Scroll the preset list p_scroll (EnhanceUIWidgets.p_scroll) the least
 * that brings p_row (one of p_rows) wholly into view -- its card and, when
 * shown, its slider; a row taller than the view shows its top. Reads the
 * current layout, so the caller runs it once the row is laid out (after a
 * slider showed or hid, on the next layout). No-op when either is NULL or
 * p_row is not inside p_scroll. */
void enhance_ui_scroll_to_row(GtkWidget *p_scroll, GtkWidget *p_row);

/* Name the file the next Save writes under the Save button ("as <name>"),
 * or clear it (c_name NULL: no file open, or no free name). Plain ASCII
 * around the name on purpose: an arrow glyph the UI font lacks sent pango
 * to a fallback font, and fontconfig leaks the charset it builds for that
 * font's coverage (caught by the ASan lane). */
void enhance_ui_set_save_target(GtkWidget *p_target, const char *c_name);

G_END_DECLS

#endif /* GGAZE_ENHANCE_UI_H */
