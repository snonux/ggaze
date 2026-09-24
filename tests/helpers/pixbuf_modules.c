/*:*
 * ggaze — which gdk-pixbuf modules this machine has (see pixbuf_modules.h)
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "pixbuf_modules.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

gboolean
ggtest_pixbuf_module_available(const char *c_module) {
   GSList  *p_formats = gdk_pixbuf_get_formats();
   gboolean b_found   = FALSE;
   for (GSList *p_l = p_formats; p_l != NULL; p_l = p_l->next) {
      gchar *c_name = gdk_pixbuf_format_get_name((GdkPixbufFormat *)p_l->data);
      b_found       = b_found || g_strcmp0(c_name, c_module) == 0;
      g_free(c_name);
   }
   g_slist_free(p_formats);
   return (b_found);
}
