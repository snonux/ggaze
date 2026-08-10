#ifndef GGAZE_POPUP_LIST_H
#define GGAZE_POPUP_LIST_H

/*:*
 * ggaze — shared hotkey list popover
 *
 * The `e` (open-external), `!` (run-script) and `m` (move) popovers are the
 * same shape: a GtkPopover (position TOP, pointing-to 1x1) with a capture-
 * phase key controller, a vertical margin box holding either an empty-state
 * message or a title + one button row per item (auto-assigned 1-9,0,a-z
 * hotkeys), where a row click or hotkey fires a per-site action and Esc /
 * outside-click tear it down. This module is that shared scaffolding so the
 * three sites shrink to a constructor call plus a small activate callback.
 * The hotkey helpers (popup_list_hotkey_char / popup_list_key_to_index /
 * popup_list_row_label) are also reused by the enhance (`a`) popover, which
 * is a toggle-popover rather than a list-popover and so keeps its own build
 * logic.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <glib.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct PopupList PopupList;

/* Borrowed row name for index u_idx of p_items (the site's configured array
 * of editors / scripts / destinations / ...). */
typedef const char *(*PopupListNameFn)(const GPtrArray *p_items, guint u_idx);

/* Fire the per-site action for index u_idx. The site is responsible for
 * closing the popover (popup_list_destroy on its stored field) in whatever
 * order it needs -- e.g. move closes BEFORE prompting Save/Discard/Cancel,
 * open-external / run-script act THEN close. */
typedef void (*PopupListActivateFn)(gpointer p_user_data, guint u_idx);

/* Build a list popover parented to p_parent (the window's stack): a GtkPopover
 * (position TOP, pointing-to a 1x1 rect) with a capture-phase key controller,
 * containing either c_empty_msg (when p_items is empty) or c_title followed
 * by one GtkButton row per item (capped at 36, the 1-9,0,a-z hotkey range),
 * labelled "<hotkey>  <name>" via p_name_fn. Esc and the popover "closed"
 * signal tear it down through pp_storage (the address of the caller's
 * PopupList* field), so a second activation can toggle it closed and dispose
 * can free it. Row click / hotkey call p_activate with p_user_data and the
 * index. Returns the new PopupList (also stored at *pp_storage); NULL on a
 * bad argument. The caller pops it with popup_list_popup(). */
PopupList *popup_list_new(GtkWidget *p_parent, PopupList **pp_storage,
                          const char *c_title, const char *c_empty_msg,
                          const GPtrArray *p_items, PopupListNameFn p_name_fn,
                          PopupListActivateFn p_activate, gpointer p_user_data);

/* Pop the popover up (gtk_popover_popup). */
void popup_list_popup(PopupList *p_list);

/* Synchronously tear the popover down: clear *pp_storage first (so a re-
 * entrant "closed" is a no-op), unparent the popover, free the PopupList.
 * Idempotent: a no-op when *pp_storage is NULL. */
void popup_list_destroy(PopupList **pp_storage);

/* Auto-assigned hotkey character for row index u_idx, in list order: 1..9,
 * then 0, then a..z. Returns 0 past the 36-hotkey range (such rows are shown
 * without a hotkey and are click-only). */
char popup_list_hotkey_char(guint u_idx);

/* Map a keyval to a popup row index (1-9 -> 0-8, 0 -> 9, a-z -> 10-35), or -1
 * for any other key. */
gint popup_list_key_to_index(guint u_keyval);

/* Newly-allocated "<hotkey>  <name>" label for row u_idx (two spaces), or
 * " <name>" (leading space) when u_idx is past the hotkey range. c_name may be
 * NULL (rendered "(unnamed)"). Caller frees with g_free. */
char *popup_list_row_label(guint u_idx, const char *c_name);

G_END_DECLS

#endif /* GGAZE_POPUP_LIST_H */