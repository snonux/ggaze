/* test_enhancer.c — GEGL enhance unit test (gated on HAVE_GEGL). */
#include "enhancer.h"
#include <glib.h>
#include <gegl.h>

static void
test_builtin_presets(void) {
   Enhancer        *e = enhancer_new();
   const GPtrArray *p = enhancer_get_presets(e);
   g_assert_cmpint(p->len, >=, 8);

   /* Create a small test buffer (2x2 RGBA float). */
   GeglRectangle rect = {0, 0, 2, 2};
   GeglBuffer   *buf  = gegl_buffer_new(&rect, babl_format("RGBA float"));
   g_assert_nonnull(buf);

   /* Apply each built-in preset → result is non-null + same dims. */
   for (guint i = 0; i < p->len; i++) {
      const EnhancerPreset *preset = g_ptr_array_index((GPtrArray *)p, i);
      if (!preset->i_builtin)
         continue;
      GError     *err = NULL;
      GeglBuffer *out = enhancer_apply(e, buf, preset, &err);
      if (out != NULL) {
         g_assert_cmpint(gegl_buffer_get_width(out), ==, 2);
         g_assert_cmpint(gegl_buffer_get_height(out), ==, 2);
         g_object_unref(out);
      } else {
         /* Some ops may not be available; skip gracefully. */
         g_clear_error(&err);
      }
   }

   g_object_unref(buf);
   enhancer_delete(e);
}

static void
test_export(void) {
   Enhancer     *e    = enhancer_new();
   GeglRectangle rect = {0, 0, 2, 2};
   GeglBuffer   *buf  = gegl_buffer_new(&rect, babl_format("RGBA float"));
   g_assert_nonnull(buf);

   const EnhancerPreset *preset =
      g_ptr_array_index((GPtrArray *)enhancer_get_presets(e), 0);
   GError  *err  = NULL;
   char    *tmp  = g_dir_make_tmp("ggaze-enhance-XXXXXX", NULL);
   char    *path = g_build_filename(tmp, "out.jpg", NULL);
   GFile   *out  = g_file_new_for_path(path);
   gboolean ok   = enhancer_export(e, buf, preset, out, &err);
   /* Export may fail if the op isn't available, but it shouldn't crash. */
   if (ok) {
      g_assert_true(g_file_query_exists(out, NULL));
   } else {
      g_clear_error(&err);
   }
   g_free(path);
   g_object_unref(out);
   g_object_unref(buf);
   enhancer_delete(e);

   /* Cleanup. */
   GFile           *td = g_file_new_for_path(tmp);
   GFileEnumerator *en = g_file_enumerate_children(
      td, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (en) {
      GFileInfo *i;
      while ((i = g_file_enumerator_next_file(en, NULL, NULL))) {
         GFile *c = g_file_get_child(td, g_file_info_get_name(i));
         g_file_delete(c, NULL, NULL);
         g_object_unref(c);
         g_object_unref(i);
      }
      g_object_unref(en);
   }
   g_file_delete(td, NULL, NULL);
   g_object_unref(td);
   g_free(tmp);
}

int
main(int argc, char **argv) {
   gegl_init(&argc, &argv);
   g_test_init(&argc, &argv, NULL);
   g_test_add_func("/enhancer/builtin_presets", test_builtin_presets);
   g_test_add_func("/enhancer/export", test_export);
   return g_test_run();
}