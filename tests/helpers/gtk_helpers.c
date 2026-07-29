/*:*
 * ggaze — shared GTK test helpers (see gtk_helpers.h)
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "gtk_helpers.h"

#include <glib.h>
#include <gtk/gtk.h>

/* --- grid cells ---------------------------------------------------------- */

GtkFlowBox *
ggtest_find_flow_box(GtkWidget *p_w) {
   if (GTK_IS_FLOW_BOX(p_w)) {
      return (GTK_FLOW_BOX(p_w));
   }
   for (GtkWidget *p_c = gtk_widget_get_first_child(p_w); p_c != NULL;
        p_c            = gtk_widget_get_next_sibling(p_c)) {
      GtkFlowBox *p_f = ggtest_find_flow_box(p_c);
      if (p_f != NULL) {
         return (p_f);
      }
   }
   return (NULL);
}

void
ggtest_activate_cell(GgazeGrid *p_grid, gint i_idx) {
   GtkFlowBox *p_flow = ggtest_find_flow_box(GTK_WIDGET(p_grid));
   g_assert_nonnull(p_flow);
   GtkFlowBoxChild *p_child = gtk_flow_box_get_child_at_index(p_flow, i_idx);
   g_assert_nonnull(p_child);
   g_signal_emit_by_name(p_flow, "child-activated", p_child);
}

/* --- alert dialogs -------------------------------------------------------- */

void
ggtest_drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

GtkWidget *
ggtest_find_button(GtkWidget *p_root, const char *c_label) {
   if (GTK_IS_BUTTON(p_root)) {
      const char *c_have = gtk_button_get_label(GTK_BUTTON(p_root));
      if (c_have != NULL && g_strcmp0(c_have, c_label) == 0) {
         return (p_root);
      }
   }
   for (GtkWidget *p_c = gtk_widget_get_first_child(p_root); p_c != NULL;
        p_c            = gtk_widget_get_next_sibling(p_c)) {
      GtkWidget *p_hit = ggtest_find_button(p_c, c_label);
      if (p_hit != NULL) {
         return (p_hit);
      }
   }
   return (NULL);
}

/* Single walk over the toplevel list behind all three public queries: counts
 * the toplevels other than p_skip that own a button labelled c_label and, for
 * the first of them, hands back the window (*pp_win) and that button
 * (*pp_btn). Either out-parameter may be NULL. */
static guint
_scan_dialogs(GtkWindow *p_skip, const char *c_label, GtkWindow **pp_win,
              GtkWidget **pp_btn) {
   GList *p_tops = gtk_window_list_toplevels();
   guint  u_n    = 0;
   for (GList *p_l = p_tops; p_l != NULL; p_l = p_l->next) {
      GtkWidget *p_top = GTK_WIDGET(p_l->data);
      if (p_skip != NULL && p_top == GTK_WIDGET(p_skip)) {
         continue;
      }
      GtkWidget *p_btn = ggtest_find_button(p_top, c_label);
      if (p_btn == NULL) {
         continue;
      }
      if (u_n == 0) {
         if (pp_win != NULL) {
            *pp_win = GTK_WINDOW(p_top);
         }
         if (pp_btn != NULL) {
            *pp_btn = p_btn;
         }
      }
      u_n++;
   }
   g_list_free(p_tops);
   return (u_n);
}

GtkWindow *
ggtest_find_dialog(GtkWindow *p_skip, const char *c_label) {
   GtkWindow *p_dlg = NULL;
   _scan_dialogs(p_skip, c_label, &p_dlg, NULL);
   return (p_dlg);
}

guint
ggtest_count_dialogs(GtkWindow *p_skip, const char *c_label) {
   return (_scan_dialogs(p_skip, c_label, NULL, NULL));
}

GtkWindow *
ggtest_wait_for_dialog(GtkWindow *p_skip, const char *c_label,
                       guint u_timeout_ms) {
   for (guint u = 0; u < u_timeout_ms; u++) {
      GtkWindow *p_dlg = ggtest_find_dialog(p_skip, c_label);
      if (p_dlg != NULL) {
         return (p_dlg);
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   return (NULL);
}

void
ggtest_click_button(GtkWidget *p_btn) {
   g_assert_true(GTK_IS_BUTTON(p_btn));
   g_signal_emit_by_name(p_btn, "clicked");
}

gboolean
ggtest_is_open_toplevel(GtkWindow *p_win) {
   GList   *p_tops = gtk_window_list_toplevels();
   gboolean b_open = (g_list_find(p_tops, p_win) != NULL);
   g_list_free(p_tops);
   return (b_open);
}

gboolean
ggtest_click_dialog_button(GtkWindow *p_skip, const char *c_label) {
   GtkWidget *p_btn = NULL;
   if (_scan_dialogs(p_skip, c_label, NULL, &p_btn) == 0) {
      return (FALSE);
   }
   ggtest_click_button(p_btn);
   return (TRUE);
}
