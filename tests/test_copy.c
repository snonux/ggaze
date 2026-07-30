/*:*
 * ggaze — copy (Ctrl+c / win.copy) integration test
 *
 * Verifies the M8 clipboard wiring in the window through the public UI
 * surface, without depending on the (display-backend-dependent) system
 * clipboard: the copy DECISION is factored into ggaze_window_get_copy_provider,
 * which returns the GdkContentProvider win.copy would set. We inspect that
 * provider's offered MIME types and serialize the chosen MIME type through
 * gdk_content_provider_write_mime_type_async into a GMemoryOutputStream — the
 * same "fake clipboard target" technique as test_clipboard.c.
 *   - No marks  -> image/png (the DISPLAYED texture's pixels), a real PNG
 *     byte stream, and NOT text/uri-list.
 *   - Marks     -> text/uri-list AND text/plain (the marked files' URIs/paths),
 *     and NOT image/png.
 * A lighter check fires win.copy directly to confirm the action is registered
 * and runs without crashing. Needs a display (integration suite; CI xvfb).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "viewer.h"
#include "window.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

/* --- helpers ------------------------------------------------------------ */

/* Windows built here are torn down with gtk_window_destroy(), never a plain
 * g_object_unref(): GTK4 hands the caller's reference to its internal
 * toplevel list and only destroy() takes the entry back out (it drops that
 * reference too, so the window still finalizes). Full rationale in
 * tests/helpers/gtk_helpers.h, "window teardown". */
static GgazeWindow *
new_window(void) {
   return (GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL)));
}

static GdkTexture *
viewer_texture(GgazeWindow *p_win) {
   GtkStack  *p_stack = ggaze_window_get_stack(p_win);
   GtkWidget *p_large = gtk_stack_get_child_by_name(p_stack, "large");
   return (ggaze_viewer_get_texture(GGAZE_VIEWER(p_large)));
}

static void
drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

static void
copy_fixture(const char *c_dir, const char *c_name) {
   const gchar *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char   *c_src = g_build_filename(c_fx, c_name, NULL);
   char   *c_dst = g_build_filename(c_dir, c_name, NULL);
   GFile  *p_src = g_file_new_for_path(c_src);
   GFile  *p_dst = g_file_new_for_path(c_dst);
   GError *p_err = NULL;
   g_assert_true(g_file_copy(p_src, p_dst, G_FILE_COPY_OVERWRITE, NULL, NULL,
                             NULL, &p_err));
   g_assert_no_error(p_err);
   g_object_unref(p_src);
   g_object_unref(p_dst);
   g_free(c_src);
   g_free(c_dst);
}

static void
cleanup_temp_dir(char *c_dir) {
   GFile           *p_dir = g_file_new_for_path(c_dir);
   GFileEnumerator *p_e =
      g_file_enumerate_children(p_dir, "standard::name,standard::type",
                                G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (p_e != NULL) {
      GFileInfo *p_info;
      while ((p_info = g_file_enumerator_next_file(p_e, NULL, NULL)) != NULL) {
         GFile *p_child = g_file_get_child(p_dir, g_file_info_get_name(p_info));
         if (g_file_info_get_file_type(p_info) == G_FILE_TYPE_DIRECTORY) {
            GFileEnumerator *p_e2 = g_file_enumerate_children(
               p_child, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (p_e2 != NULL) {
               GFileInfo *p_i2;
               while ((p_i2 = g_file_enumerator_next_file(p_e2, NULL, NULL)) !=
                      NULL) {
                  GFile *p_c2 =
                     g_file_get_child(p_child, g_file_info_get_name(p_i2));
                  g_file_delete(p_c2, NULL, NULL);
                  g_object_unref(p_c2);
                  g_object_unref(p_i2);
               }
               g_object_unref(p_e2);
            }
         }
         g_file_delete(p_child, NULL, NULL);
         g_object_unref(p_child);
         g_object_unref(p_info);
      }
      g_object_unref(p_e);
   }
   g_file_delete(p_dir, NULL, NULL);
   g_object_unref(p_dir);
   g_free(c_dir);
}

static void
fire(GgazeWindow *p_win, const char *c_action) {
   gtk_widget_activate_action(GTK_WIDGET(p_win), c_action, NULL);
}

/* Wait for the current image to fully load into the viewer. */
static void
wait_for_load(GgazeWindow *p_win) {
   for (guint u = 0; u < 3000 && viewer_texture(p_win) == NULL; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(viewer_texture(p_win));
   drain_main(200);
}

/* --- fake-target serialization (mirrors test_clipboard.c) --------------- */

typedef struct {
   GMainLoop *p_loop;
   GError    *p_err;
   gboolean   b_ok;
} WriteCtx;

static void
_write_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   WriteCtx *p_ctx = (WriteCtx *)p_data;
   p_ctx->b_ok     = gdk_content_provider_write_mime_type_finish(
      GDK_CONTENT_PROVIDER(p_src), p_res, &p_ctx->p_err);
   g_main_loop_quit(p_ctx->p_loop);
}

/* Serialize p_prov's content for c_mime into freshly-allocated bytes.
 * Returns NULL (and sets *p_err) if the provider cannot supply c_mime. */
static guchar *
serialize_mime(GdkContentProvider *p_prov, const char *c_mime, gsize *p_len,
               GError **p_err) {
   GMemoryOutputStream *p_mem =
      G_MEMORY_OUTPUT_STREAM(g_memory_output_stream_new_resizable());
   GMainLoop *p_loop = g_main_loop_new(NULL, FALSE);
   WriteCtx   st_ctx = {.p_loop = p_loop, .p_err = NULL, .b_ok = FALSE};

   gdk_content_provider_write_mime_type_async(
      p_prov, c_mime, G_OUTPUT_STREAM(p_mem), G_PRIORITY_DEFAULT, NULL,
      _write_done_cb, &st_ctx);
   g_main_loop_run(p_loop);
   g_output_stream_close(G_OUTPUT_STREAM(p_mem), NULL, NULL);

   guchar *c_out = NULL;
   if (st_ctx.b_ok) {
      c_out = g_memory_output_stream_steal_data(p_mem);
      if (p_len != NULL) {
         *p_len = g_memory_output_stream_get_data_size(p_mem);
      }
   } else {
      if (p_err != NULL) {
         *p_err = st_ctx.p_err;
      } else {
         g_clear_error(&st_ctx.p_err);
      }
   }
   g_object_unref(p_mem);
   g_main_loop_unref(p_loop);
   return (c_out);
}

/* TRUE iff p_prov advertises c_mime -- i.e. a target requesting c_mime could
 * be satisfied. This mirrors what gdk_clipboard_set_content advertises: the
 * provider's formats unioned with the GTypes' serializable MIME types (a
 * GdkTexture provider has empty ref_formats but serializes to image/png). */
static gboolean
provider_offers(GdkContentProvider *p_prov, const char *c_mime) {
   g_assert_nonnull(p_prov);
   GdkContentFormats *p_fmts = gdk_content_provider_ref_formats(p_prov);
   p_fmts        = gdk_content_formats_union_serialize_mime_types(p_fmts);
   gboolean b_in = gdk_content_formats_contain_mime_type(p_fmts, c_mime);
   gdk_content_formats_unref(p_fmts);
   return (b_in);
}

/* --- subtests ----------------------------------------------------------- */

/* No marks: ggaze_window_get_copy_provider returns an image/png provider for
 * the DISPLAYED texture; the serialized body is a real PNG stream (PNG
 * signature), and text/uri-list is NOT offered. */
static void
test_copy_image_png(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-copy-img-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   char  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_file);
   wait_for_load(p_win);
   g_assert_nonnull(viewer_texture(p_win));

   GdkContentProvider *p_prov = ggaze_window_get_copy_provider(p_win);
   g_assert_nonnull(p_prov);
   g_assert_true(provider_offers(p_prov, "image/png"));
   /* No uri-list leak in the no-marks (image) case. */
   g_assert_false(provider_offers(p_prov, "text/uri-list"));

   gsize   u_len  = 0;
   GError *p_serr = NULL;
   guchar *c_buf  = serialize_mime(p_prov, "image/png", &u_len, &p_serr);
   g_assert_no_error(p_serr);
   g_assert_nonnull(c_buf);
   /* PNG signature: \x89PNG\r\n\x1a\n. */
   static const guchar PNG_SIG[8] = {0x89, 'P',  'N',  'G',
                                     0x0D, 0x0A, 0x1A, 0x0A};
   g_assert_cmpuint(u_len, >, 8);
   g_assert_cmpint(memcmp(c_buf, PNG_SIG, 8), ==, 0);
   g_free(c_buf);
   g_object_unref(p_prov);

   /* Lighter wiring check: firing win.copy must not crash (it sets the
    * system clipboard, which is display-backend-dependent, so we only assert
    * survival here). */
   fire(p_win, "win.copy");
   drain_main(200);

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* Marks: ggaze_window_get_copy_provider returns a provider offering
 * text/uri-list AND text/plain (not image/png), with the marked files' URIs in
 * the uri-list body and their local paths in the text/plain body. */
static void
test_copy_marks_uri_list(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-copy-marks-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   copy_fixture(c_dir, "small.png");
   char  *c_plain_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file       = g_file_new_for_path(c_plain_path);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_file);
   wait_for_load(p_win);

   /* Mark the current image, then advance and mark a second one. */
   fire(p_win, "win.mark");
   fire(p_win, "win.next");
   drain_main(100);
   fire(p_win, "win.mark");
   drain_main(100);

   GdkContentProvider *p_prov = ggaze_window_get_copy_provider(p_win);
   g_assert_nonnull(p_prov);
   g_assert_true(provider_offers(p_prov, "text/uri-list"));
   g_assert_true(provider_offers(p_prov, "text/plain"));
   g_assert_false(provider_offers(p_prov, "image/png"));

   char  *c_uri1     = g_file_get_uri(p_file);
   char  *c_png_path = g_build_filename(c_dir, "small.png", NULL);
   GFile *p_f2       = g_file_new_for_path(c_png_path);
   char  *c_uri2     = g_file_get_uri(p_f2);

   /* text/uri-list body contains both marked files' file:// URIs. */
   {
      gsize   u_len  = 0;
      GError *p_serr = NULL;
      guchar *c_buf  = serialize_mime(p_prov, "text/uri-list", &u_len, &p_serr);
      g_assert_no_error(p_serr);
      g_assert_nonnull(c_buf);
      char *c_body = g_strndup((const char *)c_buf, u_len);
      g_assert_nonnull(g_strstr_len(c_body, -1, c_uri1));
      g_assert_nonnull(g_strstr_len(c_body, -1, c_uri2));
      g_free(c_body);
      g_free(c_buf);
   }
   /* text/plain body contains both local paths. */
   {
      gsize   u_len  = 0;
      GError *p_serr = NULL;
      guchar *c_buf  = serialize_mime(p_prov, "text/plain", &u_len, &p_serr);
      g_assert_no_error(p_serr);
      g_assert_nonnull(c_buf);
      char *c_body = g_strndup((const char *)c_buf, u_len);
      g_assert_nonnull(g_strstr_len(c_body, -1, c_plain_path));
      g_assert_nonnull(g_strstr_len(c_body, -1, c_png_path));
      g_free(c_body);
      g_free(c_buf);
   }

   g_object_unref(p_prov);
   fire(p_win, "win.copy"); /* wiring survival check */
   drain_main(200);

   g_free(c_uri1);
   g_free(c_png_path);
   g_free(c_uri2);
   g_free(c_plain_path);
   g_object_unref(p_f2);
   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* win.copy with nothing open is a safe no-op (the action guards on a NULL
 * provider and g_warnings rather than crashing). */
static void
test_copy_noop_when_empty(void) {
   GgazeWindow *p_win = new_window();
   g_assert_null(ggaze_window_get_copy_provider(p_win));
   fire(p_win, "win.copy");
   drain_main(100);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(200);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
   if (!gtk_init_check()) {
      g_test_skip("no display available (run under xvfb)");
      return (g_test_run());
   }
   g_test_add_func("/copy/image_png", test_copy_image_png);
   g_test_add_func("/copy/marks_uri_list", test_copy_marks_uri_list);
   g_test_add_func("/copy/noop_when_empty", test_copy_noop_when_empty);
   return (g_test_run());
}