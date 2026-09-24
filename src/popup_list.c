/*:*
 * ggaze — shared hotkey list popover
 *
 * See popup_list.h.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "popup_list.h"

#include <glib.h>
#include <gtk/gtk.h>

#include "settings-pair.h"

struct PopupList {
   GtkWidget  *p_pop;      /* GtkPopover, parented to the caller's stack */
   PopupList **pp_storage; /* address of the caller's field; cleared on
                            * destroy so a re-entrant "closed" no-ops */
   PopupListActivateFn fn_activate;
   gpointer            p_user_data;
   guint               u_count;      /* number of rows actually shown (<= 36) */
   GtkWidget          *p_prev_focus; /* weak: the window's focus widget when
                                      * the list opened, restored on close */
};

/* --- hotkey helpers (shared with the enhance popover) -------------------- */

char
popup_list_hotkey_char(guint u_idx) {
   if (u_idx < 9) {
      return ((char)('1' + u_idx));
   }
   if (u_idx == 9) {
      return ('0');
   }
   if (u_idx < 36) {
      return ((char)('a' + (u_idx - 10)));
   }
   return (0);
}

gint
popup_list_key_to_index(guint u_keyval) {
   if (u_keyval >= GDK_KEY_1 && u_keyval <= GDK_KEY_9) {
      return ((gint)(u_keyval - GDK_KEY_1));
   }
   if (u_keyval == GDK_KEY_0) {
      return (9);
   }
   if (u_keyval >= GDK_KEY_a && u_keyval <= GDK_KEY_z) {
      return ((gint)(10 + (u_keyval - GDK_KEY_a)));
   }
   return (-1);
}

const char *
popup_list_settings_pair_name(const GPtrArray *p_items, guint u_idx) {
   const SettingsPair *p_pr = g_ptr_array_index((GPtrArray *)p_items, u_idx);
   return (p_pr->c_name);
}

char *
popup_list_row_label(guint u_idx, const char *c_name) {
   char c_hk = popup_list_hotkey_char(u_idx);
   return (g_strdup_printf("%c  %s", c_hk != 0 ? c_hk : ' ',
                           c_name != NULL ? c_name : "(unnamed)"));
}

/* --- popover callbacks --------------------------------------------------- */

/* GTK hid the popover itself (outside click, lost grab, no room to place
 * it): tear down synchronously through the caller's field. This runs INSIDE
 * gtk_popover_popdown(), which is why the teardown keeps the popover alive
 * until an idle -- see _detach(). (Esc is the key controller's.) */
static void
_on_closed(GtkPopover *p_pop, gpointer p_data) {
   (void)p_pop;
   PopupList *p_list = (PopupList *)p_data;
   popup_list_delete(p_list->pp_storage);
}

/* Esc cancels; a bare digit/letter hotkey fires the matching row. Every other
 * unmodified key is swallowed: the popover is parented into the window's
 * stack, so a key it propagates bubbles up to the window's GLOBAL-scope
 * shortcuts -- with the chooser open, `d` used to trash the current image,
 * `q` quit and `h`/`l` moved the target the popover's title still named.
 * A modal chooser must answer only its own keys. Chords (Ctrl+..., Alt+...)
 * still propagate; lock modifiers (Caps Lock, Num Lock) are ignored so the
 * hotkeys keep working with Caps Lock on. */
static gboolean
_on_key_pressed(GtkEventControllerKey *p_c, guint u_keyval, guint u_kc,
                GdkModifierType e_state, gpointer p_data) {
   (void)p_c;
   (void)u_kc;
   PopupList *p_list = (PopupList *)p_data;
   if (u_keyval == GDK_KEY_Escape) {
      popup_list_delete(p_list->pp_storage);
      return (GDK_EVENT_STOP);
   }
   if ((e_state & gtk_accelerator_get_default_mod_mask() & ~GDK_SHIFT_MASK) !=
       0) {
      return (GDK_EVENT_PROPAGATE); /* a chord: not ours */
   }
   gint i_idx = popup_list_key_to_index(gdk_keyval_to_lower(u_keyval));
   if (i_idx >= 0 && (guint)i_idx < p_list->u_count) {
      p_list->fn_activate(p_list->p_user_data, (guint)i_idx);
   }
   return (GDK_EVENT_STOP); /* bound or not, the key stops here */
}

/* Row click (mouse): fire the matching row's action. */
static void
_on_row_clicked(GtkButton *p_btn, gpointer p_data) {
   PopupList *p_list = (PopupList *)p_data;
   guint u_idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(p_btn), "idx"));
   p_list->fn_activate(p_list->p_user_data, u_idx);
}

/* --- build --------------------------------------------------------------- */

/* The title label plus one "<hotkey>  <name>" button per row. */
static void
_build_rows(PopupList *p_list, GtkWidget *p_box, const char *c_title,
            const GPtrArray *p_items, PopupListNameFn p_name_fn) {
   GtkWidget *p_lbl = gtk_label_new(c_title);
   gtk_widget_set_halign(p_lbl, GTK_ALIGN_START);
   gtk_box_append(GTK_BOX(p_box), p_lbl);
   for (guint u = 0; u < p_list->u_count; u++) {
      char      *c_lbl = popup_list_row_label(u, p_name_fn(p_items, u));
      GtkWidget *p_btn = gtk_button_new_with_label(c_lbl);
      gtk_widget_set_halign(p_btn, GTK_ALIGN_START);
      g_object_set_data(G_OBJECT(p_btn), "idx", GUINT_TO_POINTER(u));
      g_signal_connect(p_btn, "clicked", G_CALLBACK(_on_row_clicked), p_list);
      gtk_box_append(GTK_BOX(p_box), p_btn);
      g_free(c_lbl);
   }
}

/* Note (weakly) the window's focus widget before the popover takes it, for
 * _restore_focus() on close. */
static void
_remember_focus(PopupList *p_list, GtkWidget *p_parent) {
   GtkRoot *p_root      = gtk_widget_get_root(p_parent);
   p_list->p_prev_focus = p_root != NULL ? gtk_root_get_focus(p_root) : NULL;
   if (p_list->p_prev_focus != NULL) {
      g_object_add_weak_pointer(G_OBJECT(p_list->p_prev_focus),
                                (gpointer *)&p_list->p_prev_focus);
   }
}

PopupList *
popup_list_new(GtkWidget *p_parent, PopupList **pp_storage, const char *c_title,
               const char *c_empty_msg, const GPtrArray *p_items,
               PopupListNameFn p_name_fn, PopupListActivateFn p_activate,
               gpointer p_user_data) {
   g_return_val_if_fail(GTK_IS_WIDGET(p_parent), NULL);
   g_return_val_if_fail(pp_storage != NULL, NULL);
   g_return_val_if_fail(p_name_fn != NULL, NULL);
   g_return_val_if_fail(p_activate != NULL, NULL);

   PopupList *p_list   = g_new0(PopupList, 1);
   p_list->p_pop       = gtk_popover_new();
   p_list->pp_storage  = pp_storage;
   p_list->fn_activate = p_activate;
   p_list->p_user_data = p_user_data;
   guint u_len         = p_items != NULL ? p_items->len : 0;
   p_list->u_count     = MIN(u_len, 36);

   gtk_popover_set_position(GTK_POPOVER(p_list->p_pop), GTK_POS_TOP);
   gtk_popover_set_pointing_to(GTK_POPOVER(p_list->p_pop),
                               &(const GdkRectangle){0, 0, 1, 1});
   g_signal_connect(GTK_POPOVER(p_list->p_pop), "closed",
                    G_CALLBACK(_on_closed), p_list);

   /* Key controller on the popover (capture phase): it sees every key first
    * and stops all but chords, so nothing leaks to the window's GLOBAL
    * shortcuts (those are registered on the root, which the popover shares
    * with the window). */
   GtkEventController *p_kc = gtk_event_controller_key_new();
   gtk_event_controller_set_propagation_phase(p_kc, GTK_PHASE_CAPTURE);
   g_signal_connect(p_kc, "key-pressed", G_CALLBACK(_on_key_pressed), p_list);
   gtk_widget_add_controller(p_list->p_pop, p_kc);

   GtkWidget *p_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
   gtk_widget_set_margin_start(p_box, 8);
   gtk_widget_set_margin_end(p_box, 8);
   gtk_widget_set_margin_top(p_box, 8);
   gtk_widget_set_margin_bottom(p_box, 8);
   gtk_popover_set_child(GTK_POPOVER(p_list->p_pop), p_box);

   if (u_len == 0) {
      GtkWidget *p_lbl = gtk_label_new(c_empty_msg);
      gtk_widget_set_halign(p_lbl, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(p_box), p_lbl);
   } else {
      _build_rows(p_list, p_box, c_title, p_items, p_name_fn);
   }

   gtk_widget_set_parent(p_list->p_pop, p_parent);
   _remember_focus(p_list, p_parent);
   *pp_storage = p_list;
   return (p_list);
}

void
popup_list_popup(PopupList *p_list) {
   g_return_if_fail(p_list != NULL);
   gtk_popover_popup(GTK_POPOVER(p_list->p_pop));
}

/* TRUE iff p_root's focus widget is p_pop or inside it. */
static gboolean
_focus_is_inside(GtkRoot *p_root, GtkWidget *p_pop) {
   GtkWidget *p_focus = gtk_root_get_focus(p_root);
   return (p_focus != NULL &&
           (p_focus == p_pop || gtk_widget_is_ancestor(p_focus, p_pop)));
}

/* TRUE iff p_w and every ancestor below the toplevel are visible and
 * child-visible: not, say, on a GtkStack page that stopped being the
 * visible one while the list was up. The toplevel itself and mapped-ness
 * are no test: a never-presented window is hidden and maps nothing. */
static gboolean
_is_shown(GtkWidget *p_w) {
   for (; p_w != NULL && !GTK_IS_ROOT(p_w); p_w = gtk_widget_get_parent(p_w)) {
      if (!gtk_widget_get_visible(p_w) || !gtk_widget_get_child_visible(p_w)) {
         return (FALSE);
      }
   }
   return (TRUE);
}

/* Put the focus back after the popover is gone: on the widget that had it
 * when the list opened (normally the viewer) if that still lives in this
 * window, is shown and takes it; else on the first focusable widget inside
 * the popover's former parent, the window's stack -- i.e. its visible page,
 * the viewer in the large view. Never the window's first focusable widget
 * (GTK_DIR_TAB_FORWARD from the window), which is the header's "Previous
 * image" button: Enter/Space would then page back. When GTK hid the
 * popover itself it also parked a ref and a deferred focus move on the
 * window, which only a successful grab of a visible widget here cancels;
 * should neither choice take the focus (both stack pages normally do),
 * GTK's own fallback runs at its next after-paint and may land on that
 * header button, and the popover lives until then. */
static void
_restore_focus(GtkRoot *p_root, GtkWidget *p_prev, GtkWidget *p_parent) {
   if (p_prev != NULL && gtk_widget_get_root(p_prev) == p_root &&
       _is_shown(p_prev) && gtk_widget_grab_focus(p_prev)) {
      return;
   }
   if (p_parent != NULL) {
      gtk_widget_child_focus(p_parent, GTK_DIR_TAB_FORWARD);
   }
}

/* Drop the teardown's reference to the popover (see _detach). */
static gboolean
_unref_idle(gpointer p_pop) {
   g_object_unref(p_pop);
   return (G_SOURCE_REMOVE);
}

/* Detach p_list's popover from the window: focus handling, unparent, and the
 * deferred release of the teardown's reference.
 *
 * FOCUS FIRST (gg2). A popped-up popover holds the root's focus widget (its
 * first row, see "POPOVER KEYBOARD FOCUS" in window.c). Unparenting it with
 * the focus inside makes GtkWindow park a REF on the popover
 * (priv->move_focus_widget) and move the focus only in its next frame's
 * after-paint phase. Until that frame the popover is unrealized -- no
 * GdkSurface -- yet alive. GTK 4.14 (fedora:40, CI) does not detach the
 * tooltip machinery from a popover on unmap (later GTKs call
 * gtk_tooltip_unset_surface() in gtk_popover_unmap), so a tooltip timeout in
 * that gap calls gdk_surface_get_device_position(NULL): a critical. On a
 * window that is never painted that frame never comes. Clearing the focus
 * explicitly BEFORE the unparent parks no ref; _restore_focus() then places
 * it at once, where the user was before the list opened (and its grab is
 * what drops the ref GTK parked when it hid the popover itself -- clearing
 * to NULL does not, see _restore_focus()).
 *
 * OUR OWN REF. The teardown can run INSIDE gtk_popover_popdown() -- GTK
 * hides a popover by itself, e.g. one taller than the room it has, and
 * "closed" -> _on_closed() lands here -- and gtk_popover_popdown() touches
 * the popover again after "closed" returns (cascade_popdown). With no other
 * ref the unparent would drop the last one and GTK would read freed memory.
 * So the teardown holds a ref across the unparent and releases it from a
 * G_PRIORITY_HIGH idle: after the current GTK call stack unwinds, yet before
 * any default-priority tooltip timeout, so the tooltip gap stays closed. */
static void
_detach(PopupList *p_list) {
   GtkWidget *p_pop    = g_object_ref(p_list->p_pop);
   GtkWidget *p_parent = gtk_widget_get_parent(p_pop);
   GtkRoot   *p_root   = gtk_widget_get_root(p_pop);
   gboolean   b_inside = p_root != NULL && _focus_is_inside(p_root, p_pop);
   if (b_inside) {
      gtk_root_set_focus(p_root, NULL);
   }
   /* The popover outlives p_list until the idle below. Its own "closed"
    * handler would then point at freed data; none fires today (unparent
    * does not hide), but that is a GTK detail not worth depending on. The
    * row and key handlers need input, which an unparented popover never
    * receives. */
   g_signal_handlers_disconnect_by_data(p_pop, p_list);
   gtk_widget_unparent(p_pop);
   if (b_inside) {
      _restore_focus(p_root, p_list->p_prev_focus, p_parent);
   }
   g_idle_add_full(G_PRIORITY_HIGH, _unref_idle, p_pop, NULL);
}

void
popup_list_delete(PopupList **pp_storage) {
   g_return_if_fail(pp_storage != NULL);
   PopupList *p_list = *pp_storage;
   if (p_list == NULL) {
      return;
   }
   *pp_storage = NULL; /* first, so a re-entrant "closed" is a no-op */
   _detach(p_list);
   if (p_list->p_prev_focus != NULL) {
      g_object_remove_weak_pointer(G_OBJECT(p_list->p_prev_focus),
                                   (gpointer *)&p_list->p_prev_focus);
   }
   g_free(p_list);
}
