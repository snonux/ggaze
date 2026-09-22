/*:*
 * ggaze — image loader dispatcher
 *
 * Reads a short header, sniffs the format, and hands off to the first
 * format-specific backend whose can_load() accepts it (JXL/AVIF/HEIF/JPEG,
 * each compiled in only when its meson feature is on). When none claims the
 * file the dispatcher itself falls back to the GdkPixbuf backend, which
 * covers PNG/GIF/WebP/TIFF/ICO, any JPEG when libjpeg is off, and whatever
 * else GdkPixbuf understands. The fallback is explicit here rather than a
 * "must stay last" entry in the table, so adding a backend is a one-place
 * edit and the pixbuf backend never has to know which formats it does not
 * own.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "loader.h"

#include <gio/gio.h>
#include <glib.h>

#include "detect.h"
#include "ggaze-config.h"

/* Format-specific backends, priority order. */
#if GGAZE_HAVE_JXL
extern const GgazeLoaderBackend jxl_backend;
#endif
#if GGAZE_HAVE_AVIF
extern const GgazeLoaderBackend avif_backend;
#endif
#if GGAZE_HAVE_HEIF
extern const GgazeLoaderBackend heif_backend;
#endif
#if GGAZE_HAVE_JPEG
extern const GgazeLoaderBackend jpeg_backend;
#endif
static const GgazeLoaderBackend *BACKENDS[] = {
#if GGAZE_HAVE_JXL
   &jxl_backend,
#endif
#if GGAZE_HAVE_AVIF
   &avif_backend,
#endif
#if GGAZE_HAVE_HEIF
   &heif_backend,
#endif
#if GGAZE_HAVE_JPEG
   &jpeg_backend,
#endif
   NULL, /* sentinel: keeps the array non-empty in the minimal build */
};

#define GGAZE_SNIFF_LEN 64

/* Read up to u_max header bytes. Returns the byte count (0 for an empty
 * file) or -1 with p_err set on an I/O failure. */
static gssize
_read_header(GFile *p_file, GCancellable *p_cancel, guint8 *p_head, gsize u_max,
             GError **p_err) {
   GError           *p_sub = NULL;
   GFileInputStream *p_in  = g_file_read(p_file, p_cancel, &p_sub);
   if (p_in == NULL) {
      g_propagate_error(p_err, p_sub);
      return (-1);
   }
   gssize n = g_input_stream_read(G_INPUT_STREAM(p_in), p_head, u_max, p_cancel,
                                  &p_sub);
   g_object_unref(p_in);
   if (n < 0) {
      g_propagate_error(p_err, p_sub);
      return (-1);
   }
   return (n);
}

/* Pick the backend for the sniffed header: first specific match, else the
 * GdkPixbuf fallback. */
static const GgazeLoaderBackend *
_backend_for(const guint8 *p_head, gsize u_len) {
   for (gsize u_i = 0; BACKENDS[u_i] != NULL; u_i++) {
      if (BACKENDS[u_i]->can_load(p_head, u_len)) {
         return (BACKENDS[u_i]);
      }
   }
   return (&pixbuf_backend);
}

/* The one sniff-and-dispatch path behind both loader_load() and the async
 * worker. An empty file is reported as G_IO_ERROR_INVALID_DATA rather than
 * handed to a decoder (the sync path used to return NULL with NO error for
 * it, which made downstream g_task_return_error(NULL) callers hang their
 * GTask forever). p_progress may be NULL; a backend without
 * load_progressive() is used through load() regardless. */
static GdkTexture *
_dispatch(GFile *p_file, GCancellable *p_cancel, LoaderProgressCb p_progress,
          gpointer p_progress_data, GError **p_err) {
   guint8 head[GGAZE_SNIFF_LEN];
   gssize i_read = _read_header(p_file, p_cancel, head, GGAZE_SNIFF_LEN, p_err);
   if (i_read < 0) {
      return (NULL);
   }
   if (i_read == 0) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "empty file (0 bytes)");
      return (NULL);
   }
   const GgazeLoaderBackend *p_be = _backend_for(head, (gsize)i_read);
   if (p_be->load_progressive != NULL && p_progress != NULL) {
      return (p_be->load_progressive(p_file, p_cancel, p_progress,
                                     p_progress_data, p_err));
   }
   return (p_be->load(p_file, p_cancel, p_err));
}

GdkTexture *
loader_load(GFile *p_file, GCancellable *p_cancel, GError **p_err) {
   g_return_val_if_fail(G_IS_FILE(p_file), NULL);
   return (_dispatch(p_file, p_cancel, NULL, NULL, p_err));
}

/* --- async wrapper (M3) -------------------------------------------------- */

typedef struct {
   LoaderProgressCb p_cb;
   gpointer         p_data;
} ProgressPair;

static void
_load_task_thread(GTask *p_task, gpointer p_src, gpointer p_task_data,
                  GCancellable *p_cancel) {
   ProgressPair *p_pair = (ProgressPair *)p_task_data;
   GError       *p_err  = NULL;
   GdkTexture   *p_tex =
      _dispatch((GFile *)p_src, p_cancel, p_pair != NULL ? p_pair->p_cb : NULL,
                p_pair != NULL ? p_pair->p_data : NULL, &p_err);
   if (p_tex == NULL) {
      /* Every backend sets p_err on failure; guard the contract anyway so a
       * NULL error can never leave the task incomplete. */
      if (p_err == NULL) {
         g_set_error(&p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "image decode failed (no detail)");
      }
      g_task_return_error(p_task, p_err);
   } else {
      g_task_return_pointer(p_task, p_tex, (GDestroyNotify)g_object_unref);
   }
}

void
loader_load_async(GFile *p_file, GCancellable *p_cancel,
                  LoaderProgressCb p_progress, gpointer p_progress_data,
                  GAsyncReadyCallback p_cb, gpointer p_data) {
   g_return_if_fail(G_IS_FILE(p_file));
   GTask *p_task = g_task_new(p_file, p_cancel, p_cb, p_data);
   if (p_progress != NULL) {
      ProgressPair *p_pair = g_new(ProgressPair, 1);
      p_pair->p_cb         = p_progress;
      p_pair->p_data       = p_progress_data;
      g_task_set_task_data(p_task, p_pair, (GDestroyNotify)g_free);
   }
   g_task_run_in_thread(p_task, _load_task_thread);
   g_object_unref(p_task);
}

GdkTexture *
loader_load_finish(GAsyncResult *p_res, GError **p_err) {
   g_return_val_if_fail(G_IS_TASK(p_res), NULL);
   return ((GdkTexture *)g_task_propagate_pointer((GTask *)p_res, p_err));
}
