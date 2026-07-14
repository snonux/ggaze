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

void
clipboard_copy_uris(GdkClipboard *p_clip, GList *p_files) {
   g_return_if_fail(GDK_IS_CLIPBOARD(p_clip));
   GString *p_str = g_string_new(NULL);
   for (GList *it = p_files; it; it = it->next) {
      char *c_uri = g_file_get_uri(G_FILE(it->data));
      g_string_append_printf(p_str, "%s\n", c_uri);
      g_free(c_uri);
   }
   GValue st_val = G_VALUE_INIT;
   g_value_init(&st_val, G_TYPE_STRING);
   g_value_take_string(&st_val, g_strdup(p_str->str));
   GdkContentProvider *p_prov = gdk_content_provider_new_for_value(&st_val);
   gdk_clipboard_set_content(p_clip, p_prov);
   g_value_unset(&st_val);
   g_object_unref(p_prov);
   g_string_free(p_str, TRUE);
}