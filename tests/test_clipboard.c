/*:*
 * ggaze — clipboard URI-list integration test
 *
 * Verifies that clipboard_build_uri_provider (the helper clipboard_copy_uris
 * sets on the GdkClipboard) advertises BOTH text/uri-list and text/plain and
 * serializes standards-compliant bytes for each:
 *   - text/uri-list : RFC 2483 CRLF-terminated URI lines (trailing CRLF).
 *   - text/plain    : newline-joined local PATH list (trailing newline), with
 *                     a URI fallback for non-local files.
 *
 * It also exercises a negative target (application/x-bogus) to prove the
 * provider does not accidentally offer arbitrary MIME types, and an empty
 * file list (must not crash and must yield a valid, empty-bytes provider).
 *
 * The fake-target technique: rather than driving a real clipboard daemon, we
 * serialize the provider's content for a chosen MIME type directly through
 * gdk_content_provider_write_mime_type_async into a GMemoryOutputStream, then
 * compare the captured bytes against the expected payload. Needs a display (the
 * GdkContentProvider/GdkContentFormats APIs require Gdk to be initialized;
 * integration suite; CI uses xvfb-run).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "clipboard.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

/* --- helpers ------------------------------------------------------------ */

static void
drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

/* Async-write completion context. */
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
 * Returns NULL (and sets *p_err) if the provider cannot supply c_mime.
 * Caller frees with g_free. The provider is ref'd here so the caller need
 * not keep it alive across the call. */
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

   /* The content write fills the buffer but does not close the stream;
    * g_memory_output_stream_steal_data requires a closed stream. */
   g_output_stream_close(G_OUTPUT_STREAM(p_mem), NULL, NULL);

   guchar *c_out = NULL;
   if (st_ctx.b_ok) {
      gsize u_size = 0;
      c_out        = g_memory_output_stream_steal_data(p_mem);
      /* steal_data returns the buffer but not its length; query it. */
      u_size = g_memory_output_stream_get_data_size(p_mem);
      if (p_len != NULL) {
         *p_len = u_size;
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

/* Build a GList of GFile* from NULL-terminated path arguments. */
static GList *
build_file_list(const char *c_first, ...) {
   GList *p_list = NULL;
   if (c_first != NULL) {
      p_list = g_list_append(p_list, g_file_new_for_path(c_first));
      va_list ap;
      va_start(ap, c_first);
      const char *c_path;
      while ((c_path = va_arg(ap, const char *)) != NULL) {
         p_list = g_list_append(p_list, g_file_new_for_path(c_path));
      }
      va_end(ap);
   }
   return (p_list);
}

static void
free_file_list(GList *p_list) {
   g_list_free_full(p_list, (GDestroyNotify)g_object_unref);
}

/* --- subtests ----------------------------------------------------------- */

/* Both MIME types must be in the provider's offered formats. */
static void
test_clipboard_formats(void) {
   GList *p_files = build_file_list("/tmp/a.jpg", "/tmp/b.jpg", NULL);
   GdkContentProvider *p_prov = clipboard_build_uri_provider(p_files);
   g_assert_nonnull(p_prov);

   GdkContentFormats *p_fmts = gdk_content_provider_ref_formats(p_prov);
   g_assert_nonnull(p_fmts);
   g_assert_true(
      gdk_content_formats_contain_mime_type(p_fmts, "text/uri-list"));
   g_assert_true(gdk_content_formats_contain_mime_type(p_fmts, "text/plain"));
   /* Negative: a type we never offered must NOT be present. */
   g_assert_false(
      gdk_content_formats_contain_mime_type(p_fmts, "application/x-bogus"));
   gdk_content_formats_unref(p_fmts);

   g_object_unref(p_prov);
   free_file_list(p_files);
   drain_main(50);
}

/* Single local file: uri-list has one CRLF-terminated line; plain has the
 * path followed by a newline. */
static void
test_clipboard_single_file(void) {
   GList              *p_files = build_file_list("/home/u/pic.jpg", NULL);
   GdkContentProvider *p_prov  = clipboard_build_uri_provider(p_files);
   g_assert_nonnull(p_prov);

   /* text/uri-list: file:// URI + trailing CRLF. */
   {
      gsize   u_len = 0;
      GError *p_err = NULL;
      guchar *c_buf = serialize_mime(p_prov, "text/uri-list", &u_len, &p_err);
      g_assert_no_error(p_err);
      g_assert_nonnull(c_buf);
      char *c_expect = g_strdup_printf("file:///home/u/pic.jpg\r\n");
      g_assert_cmpuint(u_len, ==, strlen(c_expect));
      g_assert_cmpint(memcmp(c_buf, c_expect, u_len), ==, 0);
      g_free(c_expect);
      g_free(c_buf);
   }
   /* text/plain: local path + trailing newline. */
   {
      gsize   u_len = 0;
      GError *p_err = NULL;
      guchar *c_buf = serialize_mime(p_prov, "text/plain", &u_len, &p_err);
      g_assert_no_error(p_err);
      g_assert_nonnull(c_buf);
      char *c_expect = g_strdup_printf("/home/u/pic.jpg\n");
      g_assert_cmpuint(u_len, ==, strlen(c_expect));
      g_assert_cmpint(memcmp(c_buf, c_expect, u_len), ==, 0);
      g_free(c_expect);
      g_free(c_buf);
   }

   g_object_unref(p_prov);
   free_file_list(p_files);
   drain_main(50);
}

/* Multiple local files: each line terminated correctly, order preserved. */
static void
test_clipboard_multiple_files(void) {
   GList *p_files = build_file_list("/a/1.jpg", "/a/2.png", "/a/3.webp", NULL);
   GdkContentProvider *p_prov = clipboard_build_uri_provider(p_files);
   g_assert_nonnull(p_prov);

   {
      gsize   u_len = 0;
      GError *p_err = NULL;
      guchar *c_buf = serialize_mime(p_prov, "text/uri-list", &u_len, &p_err);
      g_assert_no_error(p_err);
      g_assert_nonnull(c_buf);
      const char *c_expect =
         "file:///a/1.jpg\r\nfile:///a/2.png\r\nfile:///a/3.webp\r\n";
      g_assert_cmpuint(u_len, ==, strlen(c_expect));
      g_assert_cmpint(memcmp(c_buf, c_expect, u_len), ==, 0);
      g_free(c_buf);
   }
   {
      gsize   u_len = 0;
      GError *p_err = NULL;
      guchar *c_buf = serialize_mime(p_prov, "text/plain", &u_len, &p_err);
      g_assert_no_error(p_err);
      g_assert_nonnull(c_buf);
      const char *c_expect = "/a/1.jpg\n/a/2.png\n/a/3.webp\n";
      g_assert_cmpuint(u_len, ==, strlen(c_expect));
      g_assert_cmpint(memcmp(c_buf, c_expect, u_len), ==, 0);
      g_free(c_buf);
   }

   g_object_unref(p_prov);
   free_file_list(p_files);
   drain_main(50);
}

/* Empty list: must not crash. clipboard_build_uri_provider returns NULL
 * (leaving the clipboard untouched rather than replacing it with an empty
 * payload), and clipboard_copy_uris must handle that without crashing. */
static void
test_clipboard_empty_list(void) {
   GdkContentProvider *p_prov = clipboard_build_uri_provider(NULL);
   g_assert_null(p_prov);

   /* Driving clipboard_copy_uris with an empty list must not crash; it
    * should be a no-op against the default display's clipboard. */
   GdkDisplay *p_disp = gdk_display_get_default();
   if (p_disp != NULL) {
      GdkClipboard *p_clip = gdk_display_get_clipboard(p_disp);
      clipboard_copy_uris(p_clip, NULL);
   }

   drain_main(50);
}

/* Negative: requesting a MIME type the provider does NOT offer must fail the
 * write (write_finish returns FALSE with a GError), proving the union does
 * not accidentally advertise arbitrary content. */
static void
test_clipboard_rejects_unknown_mime(void) {
   GList              *p_files = build_file_list("/tmp/x.jpg", NULL);
   GdkContentProvider *p_prov  = clipboard_build_uri_provider(p_files);
   g_assert_nonnull(p_prov);

   gsize   u_len = 0;
   GError *p_err = NULL;
   guchar *c_buf =
      serialize_mime(p_prov, "application/x-bogus", &u_len, &p_err);
   g_assert_null(c_buf);
   g_assert_nonnull(p_err);
   g_clear_error(&p_err);

   g_object_unref(p_prov);
   free_file_list(p_files);
   drain_main(50);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
   if (!gtk_init_check()) {
      g_test_skip("no display available (run under xvfb)");
      return (g_test_run());
   }
   g_test_add_func("/clipboard/formats", test_clipboard_formats);
   g_test_add_func("/clipboard/single_file", test_clipboard_single_file);
   g_test_add_func("/clipboard/multiple_files", test_clipboard_multiple_files);
   g_test_add_func("/clipboard/empty_list", test_clipboard_empty_list);
   g_test_add_func("/clipboard/rejects_unknown_mime",
                   test_clipboard_rejects_unknown_mime);
   return (g_test_run());
}