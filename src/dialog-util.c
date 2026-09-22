/*:*
 * ggaze — modal alert-dialog plumbing
 *
 * See dialog-util.h.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "dialog-util.h"

#include <gtk/gtk.h>

GtkWindow *
dialog_util_newest_transient_for(GtkWindow *p_parent) {
   g_return_val_if_fail(GTK_IS_WINDOW(p_parent), NULL);
   GListModel *p_tops = gtk_window_get_toplevels();
   for (guint u = g_list_model_get_n_items(p_tops); u > 0; u--) {
      GtkWindow *p_top = GTK_WINDOW(g_list_model_get_item(p_tops, u - 1));
      if (gtk_window_get_transient_for(p_top) == p_parent) {
         return (p_top); /* the ref from get_item is the caller's */
      }
      g_object_unref(p_top);
   }
   return (NULL);
}
