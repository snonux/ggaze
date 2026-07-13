#ifndef GGAZE_LOADER_H
#define GGAZE_LOADER_H

/*:*
 * ggaze — image loader
 *
 * Synchronous load API for M1; M3 adds loader_load_async/_finish on top of the
 * same worker. The loader sniffs the format from the file header (detect.c)
 * and dispatches to the first registered backend whose can_load() accepts the
 * header. GdkPixbuf is the fallback backend (covers PNG/JPEG/GIF/WebP/TIFF/ICO
 * and anything GdkPixbuf happens to understand); JXL/AVIF/HEIF get specific
 * backends in M5. Every backend honors EXIF Orientation so the returned
 * GdkTexture is upright (decision #26). See docs/architecture.md "Image
 * decode".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

/* A loader backend. Compiled in conditionally (meson feature options) and
 * registered with the loader at link time. */
typedef struct {
   gboolean (*can_load)(const guint8 *p_head, gsize u_len);
   GdkTexture *(*load)(GFile *p_file, GCancellable *p_cancel, GError **p_err);
} GgazeLoaderBackend;

/* Backends register a const instance; the dispatcher (loader.c) iterates
 * BACKENDS[] in priority order. pixbuf_backend is the fallback and MUST stay
 * last (it accepts GGAZE_FMT_UNKNOWN). */
extern const GgazeLoaderBackend pixbuf_backend;

/* Synchronously load p_file into a GdkTexture (EXIF orientation applied).
 * Returns a new GdkTexture (caller owns it) or NULL with p_err set. Used by
 * tests and the clipboard helpers; the window uses the async variant below. */
GdkTexture *loader_load(GFile *p_file, GCancellable *p_cancel, GError **p_err);

/* Asynchronous load: runs the sync worker in a GTask thread, returns the
 * GdkTexture via p_cb on the main thread. The source object of the task is
 * p_file, so the finish callback can check it against navigator.current
 * (last-write-wins). */
void loader_load_async(GFile *p_file, GCancellable *p_cancel,
                       GAsyncReadyCallback p_cb, gpointer p_data);

/* Finish an async load; returns the GdkTexture (transfer full) or NULL. */
GdkTexture *loader_load_finish(GAsyncResult *p_res, GError **p_err);

G_END_DECLS

#endif /* GGAZE_LOADER_H */