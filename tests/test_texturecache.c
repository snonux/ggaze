/*:*
 * ggaze — texture cache unit test
 *
 * Exercises the bounded LRU: capacity cap + eviction, MRU ordering on get,
 * replace, and miss. Uses 1x1 GdkMemoryTextures (no display needed).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "texturecache.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

static GdkTexture *
mk_tex(void) {
   static const guint8 u_px[4] = {0, 0, 0, 255};
   GBytes             *p_b     = g_bytes_new_static(u_px, 4);
   GdkTexture *p_t = gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8, p_b, 4);
   g_bytes_unref(p_b);
   return (p_t);
}

static void
test_cap_and_evict(void) {
   TextureCache *p_c      = texturecache_new(4);
   const char   *names[5] = {"a.jpg", "b.jpg", "c.jpg", "d.jpg", "e.jpg"};
   GFile        *f[5];
   GdkTexture   *t[5];
   for (gint i = 0; i < 5; i++) {
      f[i] = g_file_new_for_path(names[i]);
      t[i] = mk_tex();
      texturecache_put(p_c, f[i], t[i]);
   }
   g_assert_cmpint(texturecache_get_size(p_c), ==, 4); /* cap 4, 5 put */
   g_assert_null(texturecache_get(p_c, f[0]));         /* LRU evicted */
   g_assert_nonnull(texturecache_get(p_c, f[4]));      /* newest kept */

   for (gint i = 0; i < 5; i++) {
      g_object_unref(f[i]);
      g_object_unref(t[i]);
   }
   texturecache_delete(p_c);
}

static void
test_lru_order(void) {
   TextureCache *p_c = texturecache_new(2);
   GFile        *f1  = g_file_new_for_path("1.jpg");
   GFile        *f2  = g_file_new_for_path("2.jpg");
   GFile        *f3  = g_file_new_for_path("3.jpg");
   GdkTexture   *t1 = mk_tex(), *t2 = mk_tex(), *t3 = mk_tex();

   texturecache_put(p_c, f1, t1);
   texturecache_put(p_c, f2, t2);
   /* Touch f1 -> f2 becomes LRU. */
   g_assert_nonnull(texturecache_get(p_c, f1));
   texturecache_put(p_c, f3, t3); /* evicts LRU = f2 */
   g_assert_null(texturecache_get(p_c, f2));
   g_assert_nonnull(texturecache_get(p_c, f1));
   g_assert_nonnull(texturecache_get(p_c, f3));

   g_object_unref(f1);
   g_object_unref(f2);
   g_object_unref(f3);
   g_object_unref(t1);
   g_object_unref(t2);
   g_object_unref(t3);
   texturecache_delete(p_c);
}

static void
test_replace_and_miss(void) {
   TextureCache *p_c = texturecache_new(4);
   GFile        *f   = g_file_new_for_path("x.jpg");
   GdkTexture   *t1 = mk_tex(), *t2 = mk_tex();

   texturecache_put(p_c, f, t1);
   g_assert_cmpint(texturecache_get_size(p_c), ==, 1);
   texturecache_put(p_c, f, t2); /* replace, no size growth */
   g_assert_cmpint(texturecache_get_size(p_c), ==, 1);
   g_assert_true(texturecache_get(p_c, f) == t2);

   GFile *f_other = g_file_new_for_path("other.jpg");
   g_assert_null(texturecache_get(p_c, f_other)); /* miss */

   g_object_unref(f);
   g_object_unref(f_other);
   g_object_unref(t1);
   g_object_unref(t2);
   texturecache_delete(p_c);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/texturecache/cap_and_evict", test_cap_and_evict);
   g_test_add_func("/texturecache/lru_order", test_lru_order);
   g_test_add_func("/texturecache/replace_and_miss", test_replace_and_miss);
   return (g_test_run());
}