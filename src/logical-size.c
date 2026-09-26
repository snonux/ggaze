/*:*
 * ggaze — the image size a texture stands for
 *
 * See logical-size.h. The size is a small allocation kept as the
 * texture's qdata and freed with it; a texture at its own size carries
 * none.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "logical-size.h"

#include <gdk/gdk.h>
#include <glib.h>

typedef struct {
   gint i_w;
   gint i_h;
} _Size;

static GQuark
_quark(void) {
   return (g_quark_from_static_string("ggaze-logical-size"));
}

void
logical_size_set(GdkTexture *p_tex, gint i_w, gint i_h) {
   g_return_if_fail(GDK_IS_TEXTURE(p_tex));
   g_return_if_fail(i_w >= 1 && i_h >= 1);
   if (i_w == gdk_texture_get_width(p_tex) &&
       i_h == gdk_texture_get_height(p_tex)) {
      g_object_set_qdata(G_OBJECT(p_tex), _quark(), NULL);
      return;
   }
   _Size *p_size = g_new(_Size, 1);
   p_size->i_w   = i_w;
   p_size->i_h   = i_h;
   g_object_set_qdata_full(G_OBJECT(p_tex), _quark(), p_size, g_free);
}

void
logical_size_get(GdkTexture *p_tex, gint *p_w, gint *p_h) {
   g_return_if_fail(GDK_IS_TEXTURE(p_tex));
   const _Size *p_size = g_object_get_qdata(G_OBJECT(p_tex), _quark());
   if (p_w != NULL) {
      *p_w = p_size != NULL ? p_size->i_w : gdk_texture_get_width(p_tex);
   }
   if (p_h != NULL) {
      *p_h = p_size != NULL ? p_size->i_h : gdk_texture_get_height(p_tex);
   }
}

gboolean
logical_size_is_scaled(GdkTexture *p_tex) {
   g_return_val_if_fail(GDK_IS_TEXTURE(p_tex), FALSE);
   return (g_object_get_qdata(G_OBJECT(p_tex), _quark()) != NULL);
}
