/* clipboard.c — copy image (PNG) or URIs to GdkClipboard. */
#include "clipboard.h"
#include "loader/loader.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

static void
_copy_image_thread(GTask *p_task, gpointer p_src, gpointer p_data,
                   GCancellable *p_cancel) {
   (void)p_data;
   GError     *p_err = NULL;
   GdkTexture *p_tex = loader_load((GFile *)p_src, p_cancel, &p_err);
   if (p_tex == NULL) {
      g_task_return_error(p_task, p_err);
   } else {
      g_task_return_pointer(p_task, p_tex, (GDestroyNotify)g_object_unref);
   }
}

void
clipboard_copy_image_async(GdkClipboard *p_clip, GFile *p_file,
                           GCancellable *p_cancel, GAsyncReadyCallback p_cb,
                           gpointer p_data) {
   g_return_if_fail(GDK_IS_CLIPBOARD(p_clip));
   g_return_if_fail(G_IS_FILE(p_file));
   GTask *p_task = g_task_new(p_file, p_cancel, p_cb, p_data);
   g_task_run_in_thread(p_task, _copy_image_thread);
   g_object_unref(p_task);
}

static void
_clip_image_finish_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   GdkClipboard *p_clip = GDK_CLIPBOARD(p_data);
   GError       *p_err  = NULL;
   GdkTexture   *p_tex  = g_task_propagate_pointer(G_TASK(p_res), &p_err);
   if (p_tex != NULL) {
      GdkContentProvider *p_prov =
         gdk_content_provider_new_typed(GDK_TYPE_TEXTURE, p_tex);
      gdk_clipboard_set_content(p_clip, p_prov);
      g_object_unref(p_prov);
      g_object_unref(p_tex);
   } else {
      g_clear_error(&p_err);
   }
   g_object_unref(p_data);
}

/* Convenience: start the copy and set the clipboard when done. */
void
ggaze_clipboard_copy_image(GdkClipboard *p_clip, GFile *p_file) {
   clipboard_copy_image_async(p_clip, p_file, NULL, _clip_image_finish_cb,
                              g_object_ref(p_clip));
}

gboolean
clipboard_copy_image_finish(GAsyncResult *p_res, GError **p_err) {
   g_return_val_if_fail(G_IS_TASK(p_res), FALSE);
   GdkTexture *p_tex = g_task_propagate_pointer(G_TASK(p_res), p_err);
   if (p_tex) {
      g_object_unref(p_tex);
      return TRUE;
   }
   return FALSE;
}

/* Build a GdkContentProvider that offers the marked files as BOTH
 * `text/uri-list` (RFC 2483: CRLF-terminated URI lines, the format file
 * managers request) and `text/plain` (a newline-joined list of local PATHS
 * so pasting into a text field yields readable paths rather than raw URIs;
 * if a file has no local path, its URI is used for that line instead). The
 * two byte-buffers are wrapped in a union so a target requesting either MIME
 * type is satisfied. Returns a new ref the caller must unref, or NULL when
 * the file list is empty/NULL.
 *
 * An empty list returns NULL rather than an empty provider: in practice
 * clipboard_copy_uris is only invoked when marks exist, and returning NULL
 * lets the caller leave the previous clipboard content untouched instead of
 * replacing it with an empty payload (which GDK's bytes provider refuses to
 * serialize anyway). */
GdkContentProvider *
clipboard_build_uri_provider(GList *p_files) {
   if (p_files == NULL) {
      return (NULL);
   }
   GString *p_uris  = g_string_new(NULL); /* text/uri-list body   */
   GString *p_plain = g_string_new(NULL); /* text/plain body      */
   for (GList *it = p_files; it; it = it->next) {
      GFile *p_f    = G_FILE(it->data);
      char  *c_uri  = g_file_get_uri(p_f);
      char  *c_path = g_file_get_path(p_f);
      /* RFC 2483: each record terminated by CRLF, incl. the last. */
      g_string_append_printf(p_uris, "%s\r\n", c_uri);
      /* Prefer the local path for human-readable text/plain paste;
       * fall back to the URI for non-local (e.g. trash://) files. */
      g_string_append_printf(p_plain, "%s\n", c_path != NULL ? c_path : c_uri);
      g_free(c_uri);
      g_free(c_path);
   }
   GBytes *p_uri_bytes   = g_bytes_new_take(p_uris->str, p_uris->len);
   GBytes *p_plain_bytes = g_bytes_new_take(p_plain->str, p_plain->len);
   /* g_bytes_new_take freed the GString buffers; only the structs remain. */
   g_string_free(p_uris, FALSE);
   g_string_free(p_plain, FALSE);
   GdkContentProvider *p_provs[2] = {
      gdk_content_provider_new_for_bytes("text/uri-list", p_uri_bytes),
      gdk_content_provider_new_for_bytes("text/plain", p_plain_bytes),
   };
   /* new_for_bytes refs/copies the bytes; we can release our ref now. */
   g_bytes_unref(p_uri_bytes);
   g_bytes_unref(p_plain_bytes);
   GdkContentProvider *p_union =
      gdk_content_provider_new_union(p_provs, G_N_ELEMENTS(p_provs));
   /* new_union "takes ownership" of the sub-providers: it steals our refs
    * and frees them when the union is disposed, so we must NOT unref them
    * here (doing so would double-free them on the union's dispose). */
   return (p_union);
}

void
clipboard_copy_uris(GdkClipboard *p_clip, GList *p_files) {
   g_return_if_fail(GDK_IS_CLIPBOARD(p_clip));
   GdkContentProvider *p_prov = clipboard_build_uri_provider(p_files);
   if (p_prov == NULL) {
      return; /* empty list: leave the clipboard untouched. */
   }
   /* gdk_clipboard_set_content takes its own ref on the provider; we still
    * own our initial ref from clipboard_build_uri_provider, so drop it. */
   gdk_clipboard_set_content(p_clip, p_prov);
   g_object_unref(p_prov);
}