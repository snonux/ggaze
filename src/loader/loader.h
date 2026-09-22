#ifndef GGAZE_LOADER_H
#define GGAZE_LOADER_H

/*:*
 * ggaze — image loader
 *
 * Synchronous loader_load() plus loader_load_async/_finish on top of the same
 * worker. The loader sniffs the format from the file header (detect.c) and
 * dispatches to the first format-specific backend whose can_load() accepts
 * the header (JXL/AVIF/HEIF/JPEG, each optional); when none claims the file
 * the dispatcher falls back to the GdkPixbuf backend (PNG/GIF/WebP/TIFF/ICO,
 * JPEG when libjpeg is off, and anything else GdkPixbuf understands). Every
 * backend honors EXIF Orientation so the returned GdkTexture is upright
 * (decision #26) and honors the GCancellable so a superseded load stops
 * before its expensive decode. See docs/architecture.md "Image decode".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

/* Optional progressive-load callback (called from the worker thread with a
 * low-res partial texture before the full decode completes). */
typedef void (*LoaderProgressCb)(GdkTexture *p_partial, gpointer p_data);

/* A loader backend. Compiled in conditionally (meson feature options) and
 * registered with the loader at link time. load()/load_progressive() MUST
 * honour p_cancel: pass it to the file read and call
 * g_cancellable_set_error_if_cancelled() before the decode, so the window's
 * one-active-load cancel is not a no-op on the slowest formats. On failure
 * they return NULL and MUST set *p_err. */
typedef struct {
   gboolean (*can_load)(const guint8 *p_head, gsize u_len);
   GdkTexture *(*load)(GFile *p_file, GCancellable *p_cancel, GError **p_err);
   /* Optional: two-phase load (low-res first via p_progress, then full). */
   GdkTexture *(*load_progressive)(GFile *p_file, GCancellable *p_cancel,
                                   LoaderProgressCb p_progress,
                                   gpointer p_progress_data, GError **p_err);
} GgazeLoaderBackend;

/* Backends register a const instance; the dispatcher (loader.c) tries the
 * format-specific ones in priority order and then this GdkPixbuf fallback
 * unconditionally. */
extern const GgazeLoaderBackend pixbuf_backend;

/* Synchronously load p_file into a GdkTexture (EXIF orientation applied).
 * Returns a new GdkTexture (caller owns it) or NULL with p_err set (always,
 * including for an empty file). Used by tests and the GEGL enhancer; the
 * window uses the async variant below. */
GdkTexture *loader_load(GFile *p_file, GCancellable *p_cancel, GError **p_err);

/* Asynchronous load: runs the sync worker in a GTask thread, returns the
 * GdkTexture via p_cb on the main thread. The source object of the task is
 * p_file, so the finish callback can check it against navigator.current
 * (last-write-wins). */
void loader_load_async(GFile *p_file, GCancellable *p_cancel,
                       LoaderProgressCb p_progress, gpointer p_progress_data,
                       GAsyncReadyCallback p_cb, gpointer p_data);

/* Finish an async load; returns the GdkTexture (transfer full) or NULL. */
GdkTexture *loader_load_finish(GAsyncResult *p_res, GError **p_err);

/* Decode p_file to an upright GdkPixbuf no larger than i_max_px on either
 * side (aspect kept), for the thumbnail cache. Formats GdkPixbuf knows take
 * the fast at-scale path (a JPEG decodes at 1/8 straight from DCT); formats
 * only a specific backend decodes (JXL/AVIF/HEIF) go through that backend's
 * full decode and are scaled afterwards -- which is what keeps the grid from
 * staying blank for exactly the formats the large view can show. The same
 * oversized-JPEG guard the full path uses runs here, so a crafted header
 * cannot stall the thumbnail pool. Caller unrefs. */
GdkPixbuf *loader_load_pixbuf_scaled(GFile *p_file, int i_max_px,
                                     GCancellable *p_cancel, GError **p_err);

/* The STORED pixel dimensions of p_file (before EXIF orientation, as an
 * EXIF card reports them) without decoding it when GdkPixbuf can parse the
 * header, else -- for a format only a specific backend decodes (JXL/AVIF/
 * HEIF) -- through that backend's decode, in which case they are the
 * upright ones. FALSE when they cannot be determined. Callers gathering
 * info for arbitrary files should run in a worker (the decode path). */
gboolean loader_peek_dimensions(GFile *p_file, int *p_w, int *p_h);

G_END_DECLS

#endif /* GGAZE_LOADER_H */