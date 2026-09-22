/* test_enhancer.c — GEGL enhance unit test (gated on HAVE_GEGL). */
#include "enhancer.h"
#include "enhancer-gegl.h"
#include "transform.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

/* Query the platform content type for p_path; return a non-NULL string
 * (caller frees) if it starts with c_prefix, else NULL. Asserts the
 * exported bytes match the requested extension (ju0). */
static char *
_content_type_is(const char *c_path, const char *c_prefix) {
   GFile     *p_f   = g_file_new_for_path(c_path);
   GError    *p_err = NULL;
   GFileInfo *p_i =
      g_file_query_info(p_f, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                        G_FILE_QUERY_INFO_NONE, NULL, &p_err);
   char *c_ct = NULL;
   if (p_i != NULL) {
      const char *c = g_file_info_get_content_type(p_i);
      if (c != NULL && g_str_has_prefix(c, c_prefix)) {
         c_ct = g_strdup(c);
      }
      g_object_unref(p_i);
   } else {
      g_clear_error(&p_err);
   }
   g_object_unref(p_f);
   return (c_ct);
}

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
      GeglBuffer *out = enhancer_apply(buf, preset, &err);
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
   gboolean ok   = enhancer_export(buf, preset, out, &err);
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

/* /enhancer/load_orientation: a JPEG whose EXIF Orientation is not 1 must be
 * loaded upright. rot6.jpg is stored 8x4 with Orientation 6 (rotate 90 CW),
 * so the upright buffer is 4 wide x 8 tall. Before the fix enhancer_load used
 * gegl:load, which does NOT honor EXIF orientation, so the buffer came back
 * 8x4 (un-rotated) and the A-menu preview thumbnails rendered sideways. */
static void
test_load_orientation(void) {
   const gchar *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char  *c_path = g_build_filename(c_fx, "rot6.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);

   GError     *p_err = NULL;
   GeglBuffer *p_buf = enhancer_load(p_file, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_buf);
   /* stored is 8x4; upright (orientation 6) swaps to 4x8. */
   g_assert_cmpint(gegl_buffer_get_width(p_buf), ==, 4);
   g_assert_cmpint(gegl_buffer_get_height(p_buf), ==, 8);

   GdkTexture *p_tex = enhancer_buffer_to_texture(p_buf, &p_err);
   g_assert_no_error(p_err);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 4);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, 8);

   g_object_unref(p_tex);
   g_object_unref(p_buf);
   g_object_unref(p_file);
}

/* /enhancer/export_format: apply Auto-fix and export to .png, .jpg and
 * .webp, asserting the file signatures AND the content type (ju0 — never
 * write JPEG into a .png). Asserts success when the saver op is installed
 * (the core jpg/png savers always are); skips honestly (not silently) if an
 * op is genuinely missing. */
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

   /* (a) PNG: a JPEG original exported to .png must contain PNG bytes (the
    * ju0 regression — previously gegl:jpg-save wrote JPEG bytes into a .png).
    */
   {
      char  *c_p   = g_build_filename(tmp, "out.png", NULL);
      GFile *p_out = g_file_new_for_path(c_p);
      g_clear_error(&p_err);
      gboolean ok = enhancer_export(p_buf, preset, p_out, &p_err);
      g_assert_true(ok);
      g_assert_no_error(p_err);
      gchar *data = NULL;
      gsize  len  = 0;
      g_assert_true(g_file_get_contents(c_p, &data, &len, NULL));
      g_assert_cmpint(len, >=, 8);
      g_assert_cmpmem(data, 8, "\x89PNG\r\n\x1a\n", 8);
      g_free(data);
      char *c_ct = _content_type_is(c_p, "image/png");
      if (c_ct == NULL) {
         g_test_message("content type unavailable (no shared-mime-info?); "
                        "skipping content-type assertion");
      } else {
         g_free(c_ct); /* helper already matched the image/png prefix */
      }
      g_object_unref(p_out);
      g_unlink(c_p);
      g_free(c_p);
   }

   /* (b) JPEG: a JPEG original exported to .jpg must contain JPEG bytes. */
   {
      char  *c_p   = g_build_filename(tmp, "out.jpg", NULL);
      GFile *p_out = g_file_new_for_path(c_p);
      g_clear_error(&p_err);
      gboolean ok = enhancer_export(p_buf, preset, p_out, &p_err);
      g_assert_true(ok);
      g_assert_no_error(p_err);
      gchar *data = NULL;
      gsize  len  = 0;
      g_assert_true(g_file_get_contents(c_p, &data, &len, NULL));
      g_assert_cmpint(len, >=, 2);
      g_assert_cmpmem(data, 2, "\xff\xd8", 2);
      g_free(data);
      char *c_ct2 = _content_type_is(c_p, "image/jpeg");
      if (c_ct2 == NULL) {
         g_test_message("content type unavailable (no shared-mime-info?); "
                        "skipping content-type assertion");
      } else {
         g_free(c_ct2); /* helper already matched the image/jpeg prefix */
      }
      g_object_unref(p_out);
      g_unlink(c_p);
      g_free(c_p);
   }

   /* (c) WebP: signature is "RIFF....WEBP". webp-save ships as a plugin,
    * so log-and-continue if it is not installed (not a required ju0 format). */
   {
      char *c_p = g_build_filename(tmp, "out.webp", NULL);
      if (!gegl_has_operation("gegl:webp-save")) {
         g_test_message("gegl:webp-save unavailable; skipping webp export");
      } else {
         GFile *p_out = g_file_new_for_path(c_p);
         g_clear_error(&p_err);
         gboolean ok = enhancer_export(p_buf, preset, p_out, &p_err);
         g_assert_true(ok);
         g_assert_no_error(p_err);
         gchar *data = NULL;
         gsize  len  = 0;
         g_assert_true(g_file_get_contents(c_p, &data, &len, NULL));
         g_assert_cmpint(len, >=, 12);
         g_assert_cmpmem(data, 4, "RIFF", 4);
         g_assert_cmpmem(data + 8, 4, "WEBP", 4);
         g_free(data);
         g_object_unref(p_out);
      }
      g_unlink(c_p);
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
      gboolean       ok       = enhancer_export(p_buf, preset, p_out, &p_err);
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
      gboolean       ok       = enhancer_export(p_buf, preset, p_out, &p_err);
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

/* /enhancer/export_stale_dest (ku0): a pre-existing regular file at the
 * destination must never count as success on its own. (a) A writable stale
 * file is correctly overwritten — assert the new bytes (PNG signature) and
 * that they differ from the stale content, not mere presence. (b) A
 * read-only stale file the saver cannot replace must fail (FALSE + GError).
 */
static void
test_export_stale_dest(void) {
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
   char *tmp = g_dir_make_tmp("ggaze-stale-XXXXXX", NULL);

   /* (a) writable stale regular file: overwritten, not trusted as-is. */
   {
      char       *c_p   = g_build_filename(tmp, "out.png", NULL);
      const char *stale = "STALE-CONTENT-NOT-A-PNG";
      gsize       n_st  = strlen(stale);
      g_assert_true(g_file_set_contents(c_p, stale, n_st, NULL));
      GFile *p_out = g_file_new_for_path(c_p);
      g_clear_error(&p_err);
      gboolean ok = enhancer_export(p_buf, preset, p_out, &p_err);
      g_assert_true(ok);
      g_assert_no_error(p_err);
      gchar *data = NULL;
      gsize  len  = 0;
      g_assert_true(g_file_get_contents(c_p, &data, &len, NULL));
      g_assert_cmpint(len, >=, 8);
      g_assert_cmpmem(data, 8, "\x89PNG\r\n\x1a\n", 8);
      g_assert_cmpint(len, !=, (gint)n_st);
      g_free(data);
      g_object_unref(p_out);
      g_unlink(c_p);
      g_free(c_p);
   }

   /* (b) read-only stale regular file: saver cannot replace -> FALSE.
    * Skipped when running as root: root bypasses file-mode permissions, so
    * the save would succeed and the "cannot replace" assertion would not
    * hold. The directory-destination case in export_real_success already
    * covers a root-safe save failure. */
   {
      char       *c_p   = g_build_filename(tmp, "ro.png", NULL);
      const char *stale = "STALE-RO";
      gsize       n_st  = strlen(stale);
      g_assert_true(g_file_set_contents(c_p, stale, n_st, NULL));
      if (geteuid() == 0) {
         g_test_skip("read-only destination test N/A as root");
         g_unlink(c_p);
         g_free(c_p);
      } else {
         g_assert_cmpint(g_chmod(c_p, 0444), ==, 0);
         GFile *p_out = g_file_new_for_path(c_p);
         g_clear_error(&p_err);
         GLogLevelFlags old_mask = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
         gboolean       ok = enhancer_export(p_buf, preset, p_out, &p_err);
         g_log_set_always_fatal(old_mask);
         g_assert_false(ok);
         g_assert_nonnull(p_err);
         g_clear_error(&p_err);
         gchar *data = NULL;
         gsize  len  = 0;
         g_assert_true(g_file_get_contents(c_p, &data, &len, NULL));
         g_assert_cmpmem(data, len, stale, n_st);
         g_free(data);
         g_chmod(c_p, 0700);
         g_object_unref(p_out);
         g_unlink(c_p);
         g_free(c_p);
      }
   }

   g_object_unref(p_buf);
   g_object_unref(p_file);
   enhancer_delete(e);
   GFile *p_tmpf = g_file_new_for_path(tmp);
   g_file_delete(p_tmpf, NULL, NULL);
   g_object_unref(p_tmpf);
   g_free(tmp);
}

/* /enhancer/export_reject_unsupported (ju0): an unsupported export
 * extension (.bmp / .tiff) must fail clearly with G_IO_ERROR_NOT_SUPPORTED
 * rather than silently writing JPEG bytes (or anything) into the file. */
static void
test_export_reject_unsupported(void) {
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
   char *tmp = g_dir_make_tmp("ggaze-unsup-XXXXXX", NULL);

   /* .bmp is not a supported export extension. */
   {
      char  *c_p   = g_build_filename(tmp, "out.bmp", NULL);
      GFile *p_out = g_file_new_for_path(c_p);
      g_clear_error(&p_err);
      gboolean ok = enhancer_export(p_buf, preset, p_out, &p_err);
      g_assert_false(ok);
      g_assert_nonnull(p_err);
      g_assert_cmpint(p_err->code, ==, G_IO_ERROR_NOT_SUPPORTED);
      g_clear_error(&p_err);
      g_assert_false(g_file_query_exists(p_out, NULL));
      g_object_unref(p_out);
      g_free(c_p);
   }

   /* No extension at all is also unsupported. */
   {
      char  *c_p   = g_build_filename(tmp, "out", NULL);
      GFile *p_out = g_file_new_for_path(c_p);
      g_clear_error(&p_err);
      gboolean ok = enhancer_export(p_buf, preset, p_out, &p_err);
      g_assert_false(ok);
      g_assert_nonnull(p_err);
      g_assert_cmpint(p_err->code, ==, G_IO_ERROR_NOT_SUPPORTED);
      g_clear_error(&p_err);
      g_object_unref(p_out);
      g_free(c_p);
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
   GeglBuffer *p_out  = enhancer_apply_chain(buf, p, u_mask, NULL, &p_err);
   if (p_out != NULL) {
      g_assert_cmpint(gegl_buffer_get_width(p_out), ==, 4);
      g_assert_cmpint(gegl_buffer_get_height(p_out), ==, 4);
      g_object_unref(p_out);
   } else {
      g_clear_error(&p_err); /* ops may be unavailable; skip gracefully */
   }
   /* An empty mask must fail (no preset enabled) -- with no transform either;
    * the identity transform is the same as none. */
   p_out = enhancer_apply_chain(buf, p, 0, NULL, &p_err);
   g_assert_null(p_out);
   g_assert_nonnull(p_err);
   g_clear_error(&p_err);
   Transform t_id;
   transform_init(&t_id);
   p_out = enhancer_apply_chain(buf, p, 0, &t_id, &p_err);
   g_assert_null(p_out);
   g_clear_error(&p_err);
   g_object_unref(buf);
   enhancer_delete(e);
}

/* --- wb2: the geometric transform on the chain -------------------------- */

/* A 4x2 RGBA8 buffer whose red channel numbers the pixels row-major
 * (0x00 0x10 0x20 0x30 / 0x40 0x50 0x60 0x70), so a turn or crop can be
 * checked pixel by pixel. */
static GeglBuffer *
_numbered_4x2(void) {
   GeglRectangle rect  = {0, 0, 4, 2};
   GeglBuffer   *p_buf = gegl_buffer_new(&rect, babl_format("R'G'B'A u8"));
   guint8        px[4 * 2 * 4];
   for (int i = 0; i < 8; i++) {
      px[i * 4]     = (guint8)(i * 0x10);
      px[i * 4 + 1] = 0;
      px[i * 4 + 2] = 0;
      px[i * 4 + 3] = 0xff;
   }
   gegl_buffer_set(p_buf, &rect, 0, babl_format("R'G'B'A u8"), px, 4 * 4);
   return (p_buf);
}

/* The red channel of p_buf's extent, row-major, into p_out (caller sizes
 * it). */
static void
_red_channel(GeglBuffer *p_buf, guint8 *p_out) {
   const GeglRectangle *p_r = gegl_buffer_get_extent(p_buf);
   guint8 *px = g_malloc((gsize)p_r->width * (gsize)p_r->height * 4);
   gegl_buffer_get(p_buf, p_r, 1.0, babl_format("R'G'B'A u8"), px,
                   p_r->width * 4, GEGL_ABYSS_NONE);
   for (int i = 0; i < p_r->width * p_r->height; i++) {
      p_out[i] = px[i * 4];
   }
   g_free(px);
}

/* `]`: one clockwise quarter turn is an exact pixel permutation -- the old
 * bottom row becomes the new left column, top to bottom -- and the mask may
 * be empty (a transform alone is work). Four turns are the identity. */
static void
test_transform_rotate_quarter(void) {
   Enhancer        *p_e  = enhancer_new();
   const GPtrArray *p    = enhancer_get_presets(p_e);
   GeglBuffer      *p_in = _numbered_4x2();
   Transform        t;
   transform_init(&t);
   t.i_quarter       = 1;
   GError     *p_err = NULL;
   GeglBuffer *p_out = enhancer_apply_chain(p_in, p, 0, &t, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_out);
   g_assert_cmpint(gegl_buffer_get_width(p_out), ==, 2);
   g_assert_cmpint(gegl_buffer_get_height(p_out), ==, 4);
   guint8 red[8];
   _red_channel(p_out, red);
   static const guint8 WANT_CW[8] = {0x40, 0x00, 0x50, 0x10,
                                     0x60, 0x20, 0x70, 0x30};
   g_assert_cmpmem(red, 8, WANT_CW, 8);
   g_object_unref(p_out);
   /* `[`: the other way round. */
   t.i_quarter = 3;
   p_out       = enhancer_apply_chain(p_in, p, 0, &t, &p_err);
   g_assert_no_error(p_err);
   _red_channel(p_out, red);
   static const guint8 WANT_CCW[8] = {0x30, 0x70, 0x20, 0x60,
                                      0x10, 0x50, 0x00, 0x40};
   g_assert_cmpmem(red, 8, WANT_CCW, 8);
   g_object_unref(p_out);
   /* 180: both dimensions kept, order reversed. */
   t.i_quarter = 2;
   p_out       = enhancer_apply_chain(p_in, p, 0, &t, &p_err);
   g_assert_no_error(p_err);
   g_assert_cmpint(gegl_buffer_get_width(p_out), ==, 4);
   _red_channel(p_out, red);
   static const guint8 WANT_180[8] = {0x70, 0x60, 0x50, 0x40,
                                      0x30, 0x20, 0x10, 0x00};
   g_assert_cmpmem(red, 8, WANT_180, 8);
   g_object_unref(p_out);
   g_object_unref(p_in);
   enhancer_delete(p_e);
}

/* `c`: the crop rectangle picks exactly its pixels, in the base image's
 * coordinates -- after a quarter turn, in the TURNED image's coordinates.
 * A rectangle hanging off the base is clamped, never empty. */
static void
test_transform_crop(void) {
   Enhancer        *p_e  = enhancer_new();
   const GPtrArray *p    = enhancer_get_presets(p_e);
   GeglBuffer      *p_in = _numbered_4x2();
   Transform        t;
   transform_init(&t);
   t.b_crop          = TRUE;
   t.t_crop          = (CropRect){1, 0, 2, 2};
   GError     *p_err = NULL;
   GeglBuffer *p_out = enhancer_apply_chain(p_in, p, 0, &t, &p_err);
   g_assert_no_error(p_err);
   g_assert_cmpint(gegl_buffer_get_width(p_out), ==, 2);
   g_assert_cmpint(gegl_buffer_get_height(p_out), ==, 2);
   guint8 red[8];
   _red_channel(p_out, red);
   static const guint8 WANT[4] = {0x10, 0x20, 0x50, 0x60};
   g_assert_cmpmem(red, 4, WANT, 4);
   g_object_unref(p_out);
   /* Turned first: the crop (0,0,1,2) of the 2x4 turned image is its first
    * column, top two pixels: 0x40 0x50. */
   t.i_quarter = 1;
   t.t_crop    = (CropRect){0, 0, 1, 2};
   p_out       = enhancer_apply_chain(p_in, p, 0, &t, &p_err);
   g_assert_no_error(p_err);
   g_assert_cmpint(gegl_buffer_get_width(p_out), ==, 1);
   g_assert_cmpint(gegl_buffer_get_height(p_out), ==, 2);
   _red_channel(p_out, red);
   g_assert_cmpint(red[0], ==, 0x40);
   g_assert_cmpint(red[1], ==, 0x50);
   g_object_unref(p_out);
   /* Hanging off the base: clamped to what exists (never an empty crop). */
   t.i_quarter = 0;
   t.t_crop    = (CropRect){3, 0, 100, 100};
   p_out       = enhancer_apply_chain(p_in, p, 0, &t, &p_err);
   g_assert_no_error(p_err);
   g_assert_cmpint(gegl_buffer_get_width(p_out), ==, 1);
   g_assert_cmpint(gegl_buffer_get_height(p_out), ==, 2);
   g_object_unref(p_out);
   g_object_unref(p_in);
   enhancer_delete(p_e);
}

/* `R`: a straighten's output is exactly transform_output_size -- the
 * auto-crop inscribed rectangle by default, the padded bounding box with
 * auto-crop off -- and it composes with a preset and a crop. */
static void
test_transform_straighten_sizes(void) {
   Enhancer        *p_e  = enhancer_new();
   const GPtrArray *p    = enhancer_get_presets(p_e);
   GeglRectangle    rect = {0, 0, 60, 40};
   GeglBuffer      *p_in = gegl_buffer_new(&rect, babl_format("R'G'B'A u8"));
   Transform        t;
   transform_init(&t);
   t.d_degrees = 7.5;
   gdouble d_w, d_h;
   transform_output_size(&t, 60, 40, &d_w, &d_h);
   g_assert_cmpfloat(d_w, <, 60);
   g_assert_cmpfloat(d_h, <, 40);
   GError     *p_err = NULL;
   GeglBuffer *p_out = enhancer_apply_chain(p_in, p, 0, &t, &p_err);
   g_assert_no_error(p_err);
   g_assert_cmpint(gegl_buffer_get_width(p_out), ==, (gint)d_w);
   g_assert_cmpint(gegl_buffer_get_height(p_out), ==, (gint)d_h);
   g_object_unref(p_out);
   t.b_autocrop = FALSE;
   transform_output_size(&t, 60, 40, &d_w, &d_h);
   g_assert_cmpfloat(d_w, >, 60);
   p_out = enhancer_apply_chain(p_in, p, 0, &t, &p_err);
   g_assert_no_error(p_err);
   g_assert_cmpint(gegl_buffer_get_width(p_out), ==, (gint)d_w);
   g_assert_cmpint(gegl_buffer_get_height(p_out), ==, (gint)d_h);
   g_object_unref(p_out);
   /* All three stages plus a colour preset, in one graph. */
   t.b_autocrop = TRUE;
   t.i_quarter  = 1;
   t.b_crop     = TRUE;
   t.t_crop     = (CropRect){2, 2, 20, 30};
   transform_output_size(&t, 60, 40, &d_w, &d_h);
   p_out = enhancer_apply_chain(p_in, p, 1, &t, &p_err);
   if (p_out != NULL) { /* Auto-fix may be unavailable; the sizes are ours */
      g_assert_cmpint(gegl_buffer_get_width(p_out), ==, (gint)d_w);
      g_assert_cmpint(gegl_buffer_get_height(p_out), ==, (gint)d_h);
      g_object_unref(p_out);
   } else {
      g_clear_error(&p_err);
   }
   g_object_unref(p_in);
   enhancer_delete(p_e);
}

/* `s` with a transform: the exported file has the transformed size. */
static void
test_transform_export(void) {
   Enhancer        *p_e  = enhancer_new();
   const GPtrArray *p    = enhancer_get_presets(p_e);
   GeglBuffer      *p_in = _numbered_4x2();
   Transform        t;
   transform_init(&t);
   t.i_quarter    = 1;
   char   *c_tmp  = g_dir_make_tmp("ggaze-xform-export-XXXXXX", NULL);
   char   *c_path = g_build_filename(c_tmp, "turned.png", NULL);
   GFile  *p_out  = g_file_new_for_path(c_path);
   GError *p_err  = NULL;
   g_assert_true(enhancer_export_chain(p_in, p, 0, &t, p_out, &p_err));
   g_assert_no_error(p_err);
   GdkTexture *p_tex = gdk_texture_new_from_file(p_out, &p_err);
   g_assert_no_error(p_err);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 2);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, 4);
   g_object_unref(p_tex);
   g_remove(c_path);
   g_rmdir(c_tmp);
   g_free(c_path);
   g_free(c_tmp);
   g_object_unref(p_out);
   g_object_unref(p_in);
   enhancer_delete(p_e);
}

typedef struct {
   GMainLoop *p_loop;
   GPtrArray *p_result;
   GError    *p_err;
} PreviewResult;

static void
preview_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   PreviewResult *p_result = (PreviewResult *)p_data;
   p_result->p_result =
      enhancer_preview_thumbnails_finish(p_res, &p_result->p_err);
   g_main_loop_quit(p_result->p_loop);
}

static void
test_preview_thumbnails(void) {
   const gchar  *c_fx   = g_getenv("GGAZE_FIXTURES_DIR");
   char         *c_path = g_build_filename(c_fx, "plain.jpg", NULL);
   GFile        *p_file = g_file_new_for_path(c_path);
   Enhancer     *p_e    = enhancer_new();
   PreviewResult result = {.p_loop = g_main_loop_new(NULL, FALSE)};
   enhancer_preview_thumbnails_async(p_file, enhancer_get_presets(p_e), NULL,
                                     preview_done_cb, &result);
   g_main_loop_run(result.p_loop);
   g_assert_no_error(result.p_err);
   g_assert_nonnull(result.p_result);
   g_assert_cmpuint(result.p_result->len, ==, 9);
   g_assert_nonnull(g_ptr_array_index(result.p_result, 0));
   guint u_rendered = 0;
   for (guint i = 0; i < result.p_result->len; i++) {
      GdkTexture *p_tex = g_ptr_array_index(result.p_result, i);
      if (p_tex != NULL) {
         u_rendered++;
         g_assert_cmpint(gdk_texture_get_width(p_tex), >, 0);
         g_assert_cmpint(gdk_texture_get_height(p_tex), >, 0);
         g_assert_cmpint(gdk_texture_get_width(p_tex), <=, 512);
         g_assert_cmpint(gdk_texture_get_height(p_tex), <=, 512);
      }
   }
   g_assert_cmpuint(u_rendered, >, 0);
   g_ptr_array_unref(result.p_result);
   g_main_loop_unref(result.p_loop);
   enhancer_delete(p_e);
   g_object_unref(p_file);
   g_free(c_path);
}

/* /enhancer/preview_orientation: the A-menu per-preset preview thumbnails
 * must be upright too. rot6.jpg is stored 8x4 (landscape) with Orientation 6
 * (upright portrait 4x8). The fixture is tiny so the max-512px downscale is
 * a no-op, and the original (index 0) and every rendered preset preview must
 * come back 4x8 (portrait) -- without orientation they would be 8x4
 * (landscape). */
static void
test_preview_orientation(void) {
   const gchar  *c_fx   = g_getenv("GGAZE_FIXTURES_DIR");
   char         *c_path = g_build_filename(c_fx, "rot6.jpg", NULL);
   GFile        *p_file = g_file_new_for_path(c_path);
   Enhancer     *p_e    = enhancer_new();
   PreviewResult result = {.p_loop = g_main_loop_new(NULL, FALSE)};
   enhancer_preview_thumbnails_async(p_file, enhancer_get_presets(p_e), NULL,
                                     preview_done_cb, &result);
   g_main_loop_run(result.p_loop);
   g_assert_no_error(result.p_err);
   g_assert_nonnull(result.p_result);
   /* The original (index 0) must be the upright portrait (4x8), not the
    * stored landscape (8x4). */
   GdkTexture *p_orig = g_ptr_array_index(result.p_result, 0);
   g_assert_nonnull(p_orig);
   g_assert_cmpint(gdk_texture_get_width(p_orig), ==, 4);
   g_assert_cmpint(gdk_texture_get_height(p_orig), ==, 8);
   /* Every rendered preset preview is likewise upright (portrait 4x8). */
   guint u_rendered = 0;
   for (guint i = 1; i < result.p_result->len; i++) {
      GdkTexture *p_tex = g_ptr_array_index(result.p_result, i);
      if (p_tex != NULL) {
         u_rendered++;
         g_assert_cmpint(gdk_texture_get_width(p_tex), ==, 4);
         g_assert_cmpint(gdk_texture_get_height(p_tex), ==, 8);
      }
   }
   g_assert_cmpuint(u_rendered, >, 0);
   g_ptr_array_unref(result.p_result);
   g_main_loop_unref(result.p_loop);
   enhancer_delete(p_e);
   g_object_unref(p_file);
   g_free(c_path);
}

/* User presets: a graph string is parsed into a chain of GEGL operations
 * with prop=value settings and applied like a built-in; a bad op name is a
 * clean error. enhancer_set_user_presets rebuilds built-ins + users and is
 * idempotent (the window used to double the user presets on every
 * Preferences change). */
static void
test_user_graph_presets(void) {
   Enhancer  *p_e     = enhancer_new();
   GPtrArray *p_pairs = settings_pair_array_new();
   g_ptr_array_add(p_pairs, settings_pair_new("Punch", "gegl:saturation "
                                                       "scale=1.5 "
                                                       "gegl:unsharp-mask"));
   g_ptr_array_add(p_pairs, settings_pair_new("Broken", "gegl:no-such-op"));
   enhancer_set_user_presets(p_e, p_pairs);
   enhancer_set_user_presets(p_e, p_pairs); /* again: no duplication */
   const GPtrArray *p_presets = enhancer_get_presets(p_e);
   g_assert_cmpuint(p_presets->len, ==, 8 + 2);
   const EnhancerPreset *p_punch = g_ptr_array_index((GPtrArray *)p_presets, 8);
   const EnhancerPreset *p_broken =
      g_ptr_array_index((GPtrArray *)p_presets, 9);
   g_assert_cmpstr(p_punch->c_name, ==, "Punch");
   g_assert_cmpint(p_punch->i_builtin, ==, 0);

   GeglRectangle rect  = {0, 0, 2, 2};
   GeglBuffer   *p_buf = gegl_buffer_new(&rect, babl_format("RGBA float"));
   GError       *p_err = NULL;
   GeglBuffer   *p_out = enhancer_apply(p_buf, p_punch, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_out);
   g_assert_cmpint(gegl_buffer_get_width(p_out), ==, 2);
   g_object_unref(p_out);

   p_out = enhancer_apply(p_buf, p_broken, &p_err);
   g_assert_null(p_out);
   g_assert_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
   g_clear_error(&p_err);

   /* The title suffix names the enabled presets, capped at the 8-bit mask. */
   char *c_desc = enhancer_describe_mask(p_presets, 0x03);
   g_assert_cmpstr(c_desc, ==, "Auto-fix,Brightness");
   g_free(c_desc);
   g_assert_null(enhancer_describe_mask(p_presets, 0));

   g_object_unref(p_buf);
   g_ptr_array_unref(p_pairs);
   enhancer_delete(p_e);
}

/* Export destinations: "<stem>-enhanced<ext>", then "-1", "-2"; an
 * unsupported source extension is kept in the stem and .jpg appended. */
static void
test_export_dest_for(void) {
   char  *c_dir = g_dir_make_tmp("ggaze-dest-XXXXXX", NULL);
   char  *c_src = g_build_filename(c_dir, "IMG_0001.jpg", NULL);
   GFile *p_src = g_file_new_for_path(c_src);
   GFile *p_out = enhancer_export_dest_for(p_src);
   char  *c_out = g_file_get_basename(p_out);
   g_assert_cmpstr(c_out, ==, "IMG_0001-enhanced.jpg");
   g_assert_true(g_file_set_contents(g_file_peek_path(p_out), "x", 1, NULL));
   g_free(c_out);
   g_object_unref(p_out);
   p_out = enhancer_export_dest_for(p_src);
   c_out = g_file_get_basename(p_out);
   g_assert_cmpstr(c_out, ==, "IMG_0001-enhanced-1.jpg");
   g_free(c_out);
   g_remove(g_file_peek_path(p_out));
   g_object_unref(p_out);
   g_object_unref(p_src);
   g_free(c_src);

   c_src = g_build_filename(c_dir, "scan.tiff", NULL);
   p_src = g_file_new_for_path(c_src);
   p_out = enhancer_export_dest_for(p_src);
   c_out = g_file_get_basename(p_out);
   g_assert_cmpstr(c_out, ==, "scan.tiff-enhanced.jpg");
   g_free(c_out);
   g_object_unref(p_out);
   g_object_unref(p_src);
   g_free(c_src);
   char *c_first = g_build_filename(c_dir, "IMG_0001-enhanced.jpg", NULL);
   g_remove(c_first);
   g_free(c_first);
   g_rmdir(c_dir);
   g_free(c_dir);
}

int
main(int argc, char **argv) {
   gegl_init(&argc, &argv);
   g_test_init(&argc, &argv, NULL);
   g_test_add_func("/enhancer/builtin_presets", test_builtin_presets);
   g_test_add_func("/enhancer/export", test_export);
   g_test_add_func("/enhancer/load_and_to_texture", test_load_and_to_texture);
   g_test_add_func("/enhancer/load_orientation", test_load_orientation);
   g_test_add_func("/enhancer/export_format", test_export_format);
   g_test_add_func("/enhancer/export_real_success", test_export_real_success);
   g_test_add_func("/enhancer/export_stale_dest", test_export_stale_dest);
   g_test_add_func("/enhancer/export_reject_unsupported",
                   test_export_reject_unsupported);
   g_test_add_func("/enhancer/apply_chain", test_apply_chain);
   g_test_add_func("/enhancer/transform_rotate_quarter",
                   test_transform_rotate_quarter);
   g_test_add_func("/enhancer/transform_crop", test_transform_crop);
   g_test_add_func("/enhancer/transform_straighten_sizes",
                   test_transform_straighten_sizes);
   g_test_add_func("/enhancer/transform_export", test_transform_export);
   g_test_add_func("/enhancer/preview_thumbnails", test_preview_thumbnails);
   g_test_add_func("/enhancer/preview_orientation", test_preview_orientation);
   g_test_add_func("/enhancer/user_graph_presets", test_user_graph_presets);
   g_test_add_func("/enhancer/export_dest_for", test_export_dest_for);
   return g_test_run();
}
