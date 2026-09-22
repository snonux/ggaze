/*:*
 * ggaze — clipboard content providers
 *
 * Builders for what Ctrl+c puts on the GdkClipboard: the DISPLAYED texture
 * as image/png when nothing is marked (already decoded, so the PNG encode is
 * the only work), or the marked files as text/uri-list + text/plain. The
 * builders return providers without touching the clipboard, so the copy
 * decision is testable without a display-backend round trip. No state.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "clipboard.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

/* --- displayed-texture PNG copy (no-marks Ctrl+c) ----------------------- */

/* Build (but do not set) a content provider offering the DISPLAYED texture's
 * pixels as image/png. The texture is already decoded (it is the one shown in
 * the viewer), so encoding it to PNG bytes with gdk_texture_save_to_png_bytes
 * is a CPU-only step done here on the caller's thread; per docs/ui-and-
 * interactions.md this is fast enough to run synchronously (no large decode is
 * re-triggered). Returns a new ref the caller must unref, or NULL when p_tex
 * is NULL or the PNG encode fails. Useful for testing without a clipboard. */
GdkContentProvider *
clipboard_build_texture_provider(GdkTexture *p_tex) {
   g_return_val_if_fail(p_tex == NULL || GDK_IS_TEXTURE(p_tex), NULL);
   if (p_tex == NULL) {
      return (NULL);
   }
   GBytes *p_bytes = gdk_texture_save_to_png_bytes(p_tex);
   if (p_bytes == NULL) {
      return (NULL);
   }
   GdkContentProvider *p_prov =
      gdk_content_provider_new_for_bytes("image/png", p_bytes);
   /* new_for_bytes copies/refs the bytes; release our ref. */
   g_bytes_unref(p_bytes);
   return (p_prov);
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
   for (GList *p_it = p_files; p_it != NULL; p_it = p_it->next) {
      GFile *p_f    = G_FILE(p_it->data);
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
   /* Hand each body straight over to a GBytes. g_string_free(str, FALSE)
    * frees only the GString struct and RETURNS the buffer, which
    * g_bytes_new_take then owns (and g_free()s with the GBytes) -- so the
    * returned pointer is used rather than dropped, which is what glib's
    * warn_unused_result on the steal flavour of that macro asks for. The
    * lengths are read into locals first: the struct the ->len lives in is
    * gone the moment the buffer is stolen, and the order in which a call's
    * arguments are evaluated is unspecified in C. */
   gsize   u_uri_len   = p_uris->len;
   gsize   u_plain_len = p_plain->len;
   GBytes *p_uri_bytes =
      g_bytes_new_take(g_string_free(p_uris, FALSE), u_uri_len);
   GBytes *p_plain_bytes =
      g_bytes_new_take(g_string_free(p_plain, FALSE), u_plain_len);
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