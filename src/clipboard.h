#ifndef GGAZE_CLIPBOARD_H
#define GGAZE_CLIPBOARD_H

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

/* Copy a single image as PNG pixels to the clipboard (decodes in a GTask). */
void     clipboard_copy_image_async(GdkClipboard *p_clip, GFile *p_file,
                                    GCancellable       *p_cancel,
                                    GAsyncReadyCallback p_cb, gpointer p_data);
gboolean clipboard_copy_image_finish(GAsyncResult *p_res, GError **p_err);

/* Build (but do not set) a content provider offering p_tex's pixels as
 * image/png. The texture is already decoded, so the PNG encode
 * (gdk_texture_save_to_png_bytes) runs synchronously here on the caller's
 * thread. Returns a new ref the caller must unref, or NULL when p_tex is NULL
 * or the encode fails. Useful for testing without a clipboard. */
GdkContentProvider *clipboard_build_texture_provider(GdkTexture *p_tex);

/* Copy a list of files as text/uri-list to the clipboard. */
void clipboard_copy_uris(GdkClipboard *p_clip, GList *p_files);

/* Build (but do not set) a content provider offering the given files as
 * text/uri-list (CRLF) and text/plain (newline-joined local paths). Returns
 * a new ref; caller must unref. Useful for testing without a clipboard. */
GdkContentProvider *clipboard_build_uri_provider(GList *p_files);

G_END_DECLS

#endif