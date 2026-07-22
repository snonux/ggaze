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

/* /enhancer/load_and_to_texture: load a fixture via the gegl:load bridge and
 * convert it to a GdkTexture (no display needed). */
static void
test_load_and_to_texture(void) {
   const gchar *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char  *c_path = g_build_filename(c_fx, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);

   GError     *p_err = NULL;
   GeglBuffer *p_buf = enhancer_load(p_file, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_buf);
   g_assert_cmpint(gegl_buffer_get_width(p_buf), >, 0);
   g_assert_cmpint(gegl_buffer_get_height(p_buf), >, 0);

   GdkTexture *p_tex = enhancer_buffer_to_texture(p_buf, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_tex);
   g_assert_true(GDK_IS_TEXTURE(p_tex));
   g_assert_cmpint(gdk_texture_get_width(p_tex), >, 0);
   g_assert_cmpint(gdk_texture_get_height(p_tex), >, 0);

   g_object_unref(p_tex);
   g_object_unref(p_buf);
   g_object_unref(p_file);
}

/* /enhancer/export_format: apply Auto-fix and export to .png and .jpg,
 * asserting the file signatures (catches ju0 — never write JPEG into a .png).
 * Skips gracefully if the saver op is unavailable. */
static void
test_export_format(void) {
   const gchar *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char  *c_path = g_build_filename(c_fx, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);

   GError     *p_err = NULL;
   GeglBuffer *p_buf = enhancer_load(p_file, &p_err);
   g_assert_nonnull(p_buf);

   Enhancer             *e = enhancer_new();
   const EnhancerPreset *preset =
      g_ptr_array_index((GPtrArray *)enhancer_get_presets(e), 0); /* Auto-fix */
   char *tmp = g_dir_make_tmp("ggaze-fmt-XXXXXX", NULL);

   /* PNG */
   {
      char  *c_p   = g_build_filename(tmp, "out.png", NULL);
      GFile *p_out = g_file_new_for_path(c_p);
      g_clear_error(&p_err);
      gboolean ok = enhancer_export(e, p_buf, preset, p_out, &p_err);
      if (ok) {
         gchar *data = NULL;
         gsize  len  = 0;
         g_assert_true(g_file_get_contents(c_p, &data, &len, NULL));
         g_assert_cmpint(len, >=, 8);
         g_assert_cmpmem(data, 8, "\x89PNG\r\n\x1a\n", 8);
         g_free(data);
      } else {
         g_clear_error(&p_err);
      }
      g_object_unref(p_out);
      g_free(c_p);
   }

   /* JPEG */
   {
      char  *c_p   = g_build_filename(tmp, "out.jpg", NULL);
      GFile *p_out = g_file_new_for_path(c_p);
      g_clear_error(&p_err);
      gboolean ok = enhancer_export(e, p_buf, preset, p_out, &p_err);
      if (ok) {
         gchar *data = NULL;
         gsize  len  = 0;
         g_assert_true(g_file_get_contents(c_p, &data, &len, NULL));
         g_assert_cmpint(len, >=, 2);
         g_assert_cmpmem(data, 2, "\xff\xd8", 2);
         g_free(data);
      } else {
         g_clear_error(&p_err);
      }
      g_object_unref(p_out);
      g_free(c_p);
   }

   g_object_unref(p_buf);
   g_object_unref(p_file);
   enhancer_delete(e);

   /* Cleanup tmp. */
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

/* /enhancer/export_real_success (ku0): a save that produces no real file must
 * return FALSE with a GError, not TRUE-on-pre-existence. Two cases: (a) an
 * output path whose parent directory does not exist, and (b) an output path
 * that is a pre-existing directory (named like a supported extension so a
 * saver op is actually selected). */
static void
test_export_real_success(void) {
   const gchar *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char  *c_path = g_build_filename(c_fx, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);

   GError     *p_err = NULL;
   GeglBuffer *p_buf = enhancer_load(p_file, &p_err);
   g_assert_nonnull(p_buf);

   Enhancer             *e = enhancer_new();
   const EnhancerPreset *preset =
      g_ptr_array_index((GPtrArray *)enhancer_get_presets(e), 0);
   char *tmp = g_dir_make_tmp("ggaze-real-XXXXXX", NULL);

   /* (a) parent dir does not exist: the saver cannot write, no file appears.
    * GEGL emits a g_warning on the failed save; relax the fatal mask so
    * enhancer_export can return FALSE and be asserted instead of aborting. */
   {
      char  *c_bad = g_build_filename(tmp, "no-such-dir", "out.png", NULL);
      GFile *p_out = g_file_new_for_path(c_bad);
      g_clear_error(&p_err);
      GLogLevelFlags old_mask = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
      gboolean       ok = enhancer_export(e, p_buf, preset, p_out, &p_err);
      g_log_set_always_fatal(old_mask);
      g_assert_false(ok);
      g_assert_nonnull(p_err);
      g_assert_cmpint(p_err->code, ==, G_IO_ERROR_FAILED);
      g_clear_error(&p_err);
      g_object_unref(p_out);
      g_free(c_bad);
   }

   /* (b) output path is a pre-existing directory named out.png: the saver op
    * is selected (extension matches), the write fails (EISDIR), and the
    * pre-existing directory must NOT count as a successful save. */
   {
      char *c_dir = g_build_filename(tmp, "out.png", NULL);
      g_assert_true(g_mkdir_with_parents(c_dir, 0700) == 0);
      GFile *p_out = g_file_new_for_path(c_dir);
      g_clear_error(&p_err);
      GLogLevelFlags old_mask = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
      gboolean       ok = enhancer_export(e, p_buf, preset, p_out, &p_err);
      g_log_set_always_fatal(old_mask);
      g_assert_false(ok);
      g_assert_nonnull(p_err);
      g_clear_error(&p_err);
      g_object_unref(p_out);
      GFile *p_dirf = g_file_new_for_path(c_dir);
      g_assert_true(g_file_delete(p_dirf, NULL, NULL));
      g_object_unref(p_dirf);
      g_free(c_dir);
   }

   g_object_unref(p_buf);
   g_object_unref(p_file);
   enhancer_delete(e);
   GFile *p_tmpf = g_file_new_for_path(tmp);
   g_file_delete(p_tmpf, NULL, NULL);
   g_object_unref(p_tmpf);
   g_free(tmp);
}

static void
test_apply_chain(void) {
   Enhancer        *e    = enhancer_new();
   const GPtrArray *p    = enhancer_get_presets(e);
   GeglRectangle    rect = {0, 0, 4, 4};
   GeglBuffer      *buf  = gegl_buffer_new(&rect, babl_format("RGBA float"));
   g_assert_nonnull(buf);
   /* Compose Auto-fix (bit 0) + Sharpen (bit 6) if those ops exist. */
   guint8      u_mask = (guint8)((1u << 0) | (1u << 6));
   GError     *p_err  = NULL;
   GeglBuffer *p_out  = enhancer_apply_chain(e, buf, p, u_mask, &p_err);
   if (p_out != NULL) {
      g_assert_cmpint(gegl_buffer_get_width(p_out), ==, 4);
      g_assert_cmpint(gegl_buffer_get_height(p_out), ==, 4);
      g_object_unref(p_out);
   } else {
      g_clear_error(&p_err); /* ops may be unavailable; skip gracefully */
   }
   /* An empty mask must fail (no preset enabled). */
   p_out = enhancer_apply_chain(e, buf, p, 0, &p_err);
   g_assert_null(p_out);
   g_assert_nonnull(p_err);
   g_clear_error(&p_err);
   g_object_unref(buf);
   enhancer_delete(e);
}

int
main(int argc, char **argv) {
   gegl_init(&argc, &argv);
   g_test_init(&argc, &argv, NULL);
   g_test_add_func("/enhancer/builtin_presets", test_builtin_presets);
   g_test_add_func("/enhancer/export", test_export);
   g_test_add_func("/enhancer/load_and_to_texture", test_load_and_to_texture);
   g_test_add_func("/enhancer/export_format", test_export_format);
   g_test_add_func("/enhancer/export_real_success", test_export_real_success);
   g_test_add_func("/enhancer/apply_chain", test_apply_chain);
   return g_test_run();
}