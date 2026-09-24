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
   guint               u_count; /* number of rows actually shown (<= 36) */
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

/* Esc / outside-click: tear down synchronously through the caller's field. */
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

/* Hand the window's keyboard focus out of p_pop BEFORE it is unparented,
 * the way GTK itself would -- only synchronously.
 *
 * Why (gg2): a popped-up popover holds the root's focus widget (its first
 * row, see "POPOVER KEYBOARD FOCUS" in window.c). Unparenting it then makes
 * GtkWindow park a REF on the popover (priv->move_focus_widget) and move the
 * focus in its next frame's after-paint phase. Until that frame the popover
 * is unrealized -- no GdkSurface -- yet alive. GTK 4.14 (fedora:40, CI) does
 * not detach the tooltip machinery from a popover on unmap (later GTKs call
 * gtk_tooltip_unset_surface() in gtk_popover_unmap), so a tooltip timeout
 * that fires in that gap calls gdk_surface_get_device_position(NULL):
 * a critical. On a window that is never painted (every never-presented
 * toplevel in the integration suites, under Xvfb's pointer) that frame never
 * comes and the gap lasts until the window is disposed.
 *
 * Moving the focus first mirrors GtkWindow's own deferred fallback (focus
 * the nearest visible ancestor that takes it, else the window's first
 * focusable widget), so the popover's last reference goes with the
 * unparent, it is finalized at once and the tooltip's weak reference to it
 * is cleared. A focus outside the popover is left alone. */
static void
_release_focus(GtkWidget *p_pop) {
   GtkRoot *p_root = gtk_widget_get_root(p_pop);
   if (p_root == NULL || !_focus_is_inside(p_root, p_pop)) {
      return;
   }
   for (GtkWidget *p_w = gtk_widget_get_parent(p_pop); p_w != NULL;
        p_w            = gtk_widget_get_parent(p_w)) {
      if (gtk_widget_get_visible(p_w) && gtk_widget_grab_focus(p_w)) {
         break;
      }
   }
   if (_focus_is_inside(p_root, p_pop)) {
      gtk_widget_child_focus(GTK_WIDGET(p_root), GTK_DIR_TAB_FORWARD);
   }
   /* Whatever the fallback found, the focus must not stay in the popover
    * (a focus chain may lead back into it): no focus beats a parked ref. */
   if (_focus_is_inside(p_root, p_pop)) {
      gtk_root_set_focus(p_root, NULL);
   }
}

void
popup_list_delete(PopupList **pp_storage) {
   g_return_if_fail(pp_storage != NULL);
   PopupList *p_list = *pp_storage;
   if (p_list == NULL) {
      return;
   }
   *pp_storage = NULL; /* first, so a re-entrant "closed" is a no-op */
   _release_focus(p_list->p_pop);
   gtk_widget_unparent(p_list->p_pop);
   g_free(p_list);
}