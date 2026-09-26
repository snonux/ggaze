/*:*
 * ggaze — logical-size unit tests (GdkMemoryTextures, no display)
 *
 * The image size a scaled-down preview texture stands for (8l2): a
 * texture without a note is its own size, a note is read back (either
 * out-parameter NULL), setting the texture's own size removes the note,
 * and the note goes with the texture.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "logical-size.h"

#include <gdk/gdk.h>
#include <glib.h>

/* An opaque i_w x i_h RGBA texture. */
static GdkTexture *
make_texture(gint i_w, gint i_h) {
   gsize   u_len = (gsize)i_w * (gsize)i_h * 4;
   guint8 *p_px  = g_malloc0(u_len);
   GBytes *p_b   = g_bytes_new_take(p_px, u_len);

   GdkTexture *p_tex =
      gdk_memory_texture_new(i_w, i_h, GDK_MEMORY_R8G8B8A8, p_b, i_w * 4);
   g_bytes_unref(p_b);
   return (p_tex);
}

static void
test_plain_texture_is_its_own_size(void) {
   GdkTexture *p_tex = make_texture(6, 4);
   gint        i_w = 0, i_h = 0;
   logical_size_get(p_tex, &i_w, &i_h);
   g_assert_cmpint(i_w, ==, 6);
   g_assert_cmpint(i_h, ==, 4);
   g_assert_false(logical_size_is_scaled(p_tex));
   g_object_unref(p_tex);
}

static void
test_note_is_read_back_and_removed(void) {
   GdkTexture *p_tex = make_texture(6, 4);
   gint        i_w = 0, i_h = 0;
   logical_size_set(p_tex, 600, 400);
   g_assert_true(logical_size_is_scaled(p_tex));
   logical_size_get(p_tex, &i_w, NULL);
   logical_size_get(p_tex, NULL, &i_h);
   g_assert_cmpint(i_w, ==, 600);
   g_assert_cmpint(i_h, ==, 400);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 6); /* the pixels stay */
   logical_size_set(p_tex, 601, 400);                    /* replaced */
   logical_size_get(p_tex, &i_w, NULL);
   g_assert_cmpint(i_w, ==, 601);
   logical_size_set(p_tex, 6, 4); /* its own size: no note */
   g_assert_false(logical_size_is_scaled(p_tex));
   logical_size_get(p_tex, &i_w, &i_h);
   g_assert_cmpint(i_w, ==, 6);
   g_assert_cmpint(i_h, ==, 4);
   g_object_unref(p_tex);
}

/* Another texture never sees the note (it is per object, not per size). */
static void
test_note_is_per_texture(void) {
   GdkTexture *p_a = make_texture(6, 4);
   GdkTexture *p_b = make_texture(6, 4);
   logical_size_set(p_a, 60, 40);
   gint i_w = 0;
   logical_size_get(p_b, &i_w, NULL);
   g_assert_cmpint(i_w, ==, 6);
   g_object_unref(p_a);
   g_object_unref(p_b);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
   g_test_add_func("/logical_size/plain_texture_is_its_own_size",
                   test_plain_texture_is_its_own_size);
   g_test_add_func("/logical_size/note_is_read_back_and_removed",
                   test_note_is_read_back_and_removed);
   g_test_add_func("/logical_size/note_is_per_texture",
                   test_note_is_per_texture);
   return (g_test_run());
}
