#ifndef GGAZE_CLIPBOARD_H
#define GGAZE_CLIPBOARD_H

/*:*
 * ggaze — clipboard content providers
 *
 * What Ctrl+c offers: the displayed texture as image/png (no marks) or the
 * marked files as text/uri-list + text/plain. The builders never touch the
 * clipboard themselves. See clipboard.c.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

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