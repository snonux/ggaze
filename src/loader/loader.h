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
 * before its expensive decode. Every entry point first refuses an empty
 * file and a file shorter than the smallest complete file of its sniffed
 * format (detect_reject_truncated(), G_IO_ERROR_INVALID_DATA), and -- when
 * the jxl feature is off -- any JXL at all (G_IO_ERROR_NOT_SUPPORTED, "JXL
 * support is not built in"), so no JXL that gdk-pixbuf would forward to
 * glycin-jxl reaches it: that loader waits forever on garbage and cannot
 * be cancelled (task tb2). The guarantee is exact where the gated bytes
 * are the decoded bytes: loader_load()'s pixbuf backend re-runs the gate
 * on the buffer it decodes through loader_sniff_bytes_for_fallback(),
 * which also refuses every format a specific backend of this build
 * claims -- so a JXL swapped in between the dispatcher's sniff and the
 * backend's read is refused in EVERY build (by the not-built-in rule
 * without libjxl, by the dispatch rule with it), never decoded by
 * gdk-pixbuf. It is best-effort where a gdk-pixbuf call must take a PATH
 * (the scaled thumbnail decode, the header-only size peek): there the
 * sniff and the decode are two opens, and a file swapped in between -- a
 * rename under a running thumbnail pass -- is decoded unsniffed. See
 * docs/tech-stack.md "The decode gate".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

#include "detect.h"

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

/* The first u_max bytes of p_file (fewer only at EOF: a read_all, so a
 * FIFO / GVFS short read cannot pass a valid file off as truncated), for a
 * caller that runs the sniff and gate itself before choosing a decoder
 * (the enhancer's ICC-aware GEGL path, decision #45). Returns the byte
 * count (0 for an empty file) or -1 with p_err set on an I/O failure. */
gssize loader_read_header(GFile *p_file, GCancellable *p_cancel, guint8 *p_head,
                          gsize u_max, GError **p_err);

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
 * oversized-JPEG, empty/truncated-file and JXL-not-built-in guards the full
 * path uses run here, so a crafted header cannot stall the thumbnail pool
 * (best-effort for the at-scale path, which decodes by PATH after the
 * sniff's own open: see the top-of-file comment). Because the sniff opens
 * the file first, a missing or unreadable file is
 * reported as a G_IO_ERROR (NOT_FOUND, PERMISSION_DENIED, ...) from GIO,
 * no longer as the G_FILE_ERROR gdk-pixbuf's path-taking call used to
 * raise. Caller unrefs. */
GdkPixbuf *loader_load_pixbuf_scaled(GFile *p_file, int i_max_px,
                                     GCancellable *p_cancel, GError **p_err);

/* The decode gate on its own, on bytes already in memory -- the empty /
 * truncated / not-built-in refusals every entry point above runs first --
 * for a caller that decodes a buffer it read itself (thumbnail.c reading a
 * cache PNG that any other TMS app may have written; the pixbuf backend
 * re-gating what it loaded). Gating the very bytes that are then decoded
 * is what makes the guarantee exact: a sniff of one open followed by a
 * decode of another can be defeated by a swap in between. Only the first
 * GGAZE_DETECT_SNIFF_LEN bytes are inspected. TRUE when a decoder may see
 * the bytes, with the sniffed format in *p_format (may be NULL;
 * GGAZE_FMT_UNKNOWN for bytes carrying no signature, which the gate does
 * not constrain); FALSE with a G_IO_ERROR in p_err otherwise -- always,
 * including G_IO_ERROR_INVALID_ARGUMENT for a NULL p_bytes with a
 * non-zero u_len, so a caller that tests p_err never hangs on a silent
 * FALSE (the async loader's g_task_return_error(NULL) hazard). */
gboolean loader_sniff_bytes(const guint8 *p_bytes, gsize u_len,
                            GgazeFormat *p_format, GError **p_err);

/* The gate as the GdkPixbuf FALLBACK applies it to the bytes it loaded:
 * loader_sniff_bytes() plus the dispatch rule -- FALSE, with
 * G_IO_ERROR_BUSY ("<format> file changed while loading; try again", a
 * status-line message, and a code distinct from the gate's and from the
 * backends' generic FAILED), for bytes that a format-specific
 * backend of this build claims (jxl/avif/heif/jpeg, whichever are
 * compiled in). Such bytes reach the fallback only when the file changed
 * between the dispatcher's sniff and the backend's read, and decoding
 * them anyway would route around the backend that owns the format: with
 * libjxl, loader_sniff_bytes() alone admits a JXL (a complete one is
 * decodable there), so a garbage JXL swapped in would reach gdk-pixbuf and
 * hang glycin-jxl. The two rules together refuse a JXL in every build,
 * which is what makes loader_load()'s guarantee exact rather than
 * build-dependent. Only the first GGAZE_DETECT_SNIFF_LEN bytes are
 * inspected. */
gboolean loader_sniff_bytes_for_fallback(const guint8 *p_bytes, gsize u_len,
                                         GError **p_err);

/* The STORED pixel dimensions of p_file (before EXIF orientation, as an
 * EXIF card reports them) from the cheapest safe source: a JPEG's SOF
 * header (decoder-free, every build), a specific backend's decode for the
 * formats it claims (JXL/AVIF/HEIF; those are then the upright ones), and
 * gdk-pixbuf's header parse only for the rest -- never for a format a
 * backend claims, because on a glycin desktop that parse hangs on a garbage
 * JXL and spawns a sandbox for a valid one. FALSE when they cannot be
 * determined, including for an empty, truncated or not-built-in file (never
 * handed to gdk-pixbuf) and for a JPEG whose declared size is over the cap
 * or whose SOF lies past the scanned prefix (fails closed like the thumbnail
 * path, without asking gdk-pixbuf). A JPEG declaring a zero side
 * (DNL-deferred height) is never reported as sized. Callers gathering info
 * for arbitrary files should run in a worker (the decode path). */
gboolean loader_peek_dimensions(GFile *p_file, int *p_w, int *p_h);

G_END_DECLS

#endif /* GGAZE_LOADER_H */