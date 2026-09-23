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
 * Before any backend -- or, on the thumbnail and info paths, any gdk-pixbuf
 * call that takes a path -- sees the file, the sniffed header goes through
 * _sniff_header(), which refuses, in this order:
 *
 *   1. an empty file                       G_IO_ERROR_INVALID_DATA
 *   2. a file shorter than the smallest complete file of its signature's
 *      format (detect_reject_truncated())  G_IO_ERROR_INVALID_DATA
 *   3. a JXL file when the jxl feature is off (any length)
 *                                          G_IO_ERROR_NOT_SUPPORTED
 *
 * 1 and 2 are properties of the bytes and hold in every build, so they run
 * first and a truncated file gets the same answer whatever is compiled in;
 * 3 is a property of the build and only decides what happens to a file that
 * could still be complete. All of it is the dispatcher's job rather than the
 * pixbuf backend's because all three entry points share it. The reason for
 * 2 and 3 is the same (task tb2): on a glycin desktop (Fedora >= 41)
 * gdk-pixbuf forwards JXL/AVIF/HEIF to sandboxed loader subprocesses with
 * no cancellable or timeout reachable from here, and glycin-jxl was measured
 * to wait forever on EVERY garbage or truncated JXL it is fed -- 8 bytes,
 * 60 bytes, a box-wrapped container alike -- through gdk_pixbuf_loader_close()
 * and gdk_pixbuf_get_file_info() both. The length gate closes the truncated
 * class; without libjxl the only bound on the rest is to never hand a JXL
 * to gdk-pixbuf at all, so a valid JXL costs a "not built in" toast instead
 * of a decode, and a garbage one costs nothing instead of a hung worker
 * that can never be cancelled. glycin-avif/heif return promptly on bad
 * input, so AVIF/HEIF stay on the fallback.
 *
 * How exact "never" is depends on whether the gated bytes are the decoded
 * bytes. The gate on bytes is loader_sniff_bytes(); _sniff_header() is the
 * same gate on one open's first 64 bytes. loader_load() is exact: the
 * pixbuf backend loads the whole file once and runs the gate on THAT
 * buffer before decoding it -- as loader_sniff_bytes_for_fallback(), which
 * adds the dispatch rule: whatever a specific backend of this build claims
 * is refused, because the fallback only ever sees such bytes when the file
 * changed between the two opens. That addition is what keeps the JXL
 * refusal build-independent: with libjxl the plain gate admits a JXL (the
 * jxl backend decodes complete ones), so a garbage JXL swapped in after
 * the sniff would otherwise reach the GdkPixbufLoader and hang glycin-jxl
 * -- refused now by rule 3 without libjxl and by the dispatch rule with it.
 * The thumbnail cache read (thumbnail.c) gates its entry the same way and
 * decodes a PNG only, so a file swapped between two opens cannot slip an
 * ungated byte into a GdkPixbufLoader anywhere. The two
 * calls that must hand gdk-pixbuf a PATH -- gdk_pixbuf_new_from_file_at_
 * scale() in loader_load_pixbuf_scaled() and gdk_pixbuf_get_file_info() in
 * loader_peek_dimensions() -- are best-effort by construction: the sniff
 * is one open and the decode another, and a file replaced in between (a
 * rename racing a thumbnail pass) reaches gdk-pixbuf unsniffed. That
 * window is a rename away from the sniff on a local disk, not something
 * an attacker steers, and the alternative (loading the whole file to
 * decode a thumbnail) would cost the at-scale JPEG path its 1/8 DCT
 * decode; it is documented rather than closed.
 *
 * GGAZE_HAVE_ANY_BACKEND (ggaze-config.h) is 0 in the minimal build (every
 * loader feature off, the lane CI's coverage gate measures): the backend
 * table, the dispatch through a backend and the two thumbnail/info paths
 * that only a claiming backend can take are compiled out there, so the
 * minimal build carries no code that no input could ever reach. Each such
 * block is a self-contained #if with the pixbuf fallback as its #else.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "loader.h"

#include <gio/gio.h>
#include <glib.h>

#include "detect.h"
#include "ggaze-config.h"
#include "pixbuf-util.h"

#if GGAZE_HAVE_ANY_BACKEND
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
   NULL, /* sentinel */
};
#endif

/* Read the first u_max bytes of p_file, or all of it when it is shorter.
 * Returns the byte count (0 for an empty file) or -1 with p_err set on an
 * I/O failure. g_input_stream_read_all(), not a single read: a single read
 * may return fewer bytes than are available on a FIFO, a pipe or a GVFS
 * stream, and a short read here would make detect_reject_truncated() take
 * a valid file delivered in two writes for a truncated one. Only EOF may
 * end the header early, which is exactly what makes the byte count
 * min(file length, u_max) and the gate's reasoning sound. Public since
 * xb2: the enhancer sniffs the same way before picking GEGL's ICC-aware
 * loader for a PNG / JPEG (loader.h). */
gssize
loader_read_header(GFile *p_file, GCancellable *p_cancel, guint8 *p_head,
                   gsize u_max, GError **p_err) {
   GError           *p_sub = NULL;
   GFileInputStream *p_in  = g_file_read(p_file, p_cancel, &p_sub);
   if (p_in == NULL) {
      g_propagate_error(p_err, p_sub);
      return (-1);
   }
   gsize    u_read = 0;
   gboolean b_ok = g_input_stream_read_all(G_INPUT_STREAM(p_in), p_head, u_max,
                                           &u_read, p_cancel, &p_sub);
   g_object_unref(p_in);
   if (!b_ok) {
      g_propagate_error(p_err, p_sub);
      return (-1);
   }
   return ((gssize)u_read);
}

/* Pick the backend for the sniffed header: first specific match, else the
 * GdkPixbuf fallback (the only one there is in the minimal build). */
static const GgazeLoaderBackend *
_backend_for(const guint8 *p_head, gsize u_len) {
#if GGAZE_HAVE_ANY_BACKEND
   for (gsize u_i = 0; BACKENDS[u_i] != NULL; u_i++) {
      if (BACKENDS[u_i]->can_load(p_head, u_len)) {
         return (BACKENDS[u_i]);
      }
   }
#else
   (void)p_head;
   (void)u_len;
#endif
   return (&pixbuf_backend);
}

/* Refuse a format this build has no decoder for and that the GdkPixbuf
 * fallback must not be trusted with. Only JXL today, and only without the
 * jxl feature: see the top-of-file comment for why a missing decode beats
 * an uncancellable hang. Both JXL spellings (bare codestream and the
 * box-wrapped container) sniff as GGAZE_FMT_JXL, so one check covers both.
 * TRUE to proceed; FALSE with G_IO_ERROR_NOT_SUPPORTED (p_err may be NULL). */
static gboolean
_refuse_unbuilt_format(const guint8 *p_head, gsize u_len, GError **p_err) {
#if GGAZE_HAVE_JXL
   (void)p_head;
   (void)u_len;
   (void)p_err;
   return (TRUE);
#else
   if (detect_format(p_head, u_len) != GGAZE_FMT_JXL) {
      return (TRUE);
   }
   g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "JXL support is not built in (enable the jxl feature)");
   return (FALSE);
#endif
}

/* The gate itself, on bytes: refuse what no decoder should see, in the
 * order the top-of-file comment gives: an empty file (reported as
 * G_IO_ERROR_INVALID_DATA rather than handed on -- the sync path used to
 * return NULL with NO error for it, which made downstream
 * g_task_return_error(NULL) callers hang their GTask forever), a file
 * shorter than its signature's minimum, and a format the build cannot
 * decode. Only the first GGAZE_DETECT_SNIFF_LEN bytes matter: every rule
 * in detect's minimum table is <= that, so a longer buffer is gated
 * exactly like its own sniff-length prefix would be. A NULL buffer with a
 * length is a caller bug, but it is reported through p_err like every
 * other FALSE (G_IO_ERROR_INVALID_ARGUMENT) rather than through a
 * g_return_val_if_fail(): the header promises p_err on every FALSE, and
 * the async wrapper's "NULL error leaves the GTask incomplete" hazard is
 * exactly what a silent FALSE would reintroduce. */
gboolean
loader_sniff_bytes(const guint8 *p_bytes, gsize u_len, GgazeFormat *p_format,
                   GError **p_err) {
   if (G_UNLIKELY(p_bytes == NULL && u_len > 0)) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "no bytes to sniff (NULL buffer, %" G_GSIZE_FORMAT
                  " bytes claimed)",
                  u_len);
      return (FALSE);
   }
   gsize u_head = MIN(u_len, (gsize)GGAZE_DETECT_SNIFF_LEN);
   if (u_head == 0) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "empty file (0 bytes)");
      return (FALSE);
   }
   if (!detect_reject_truncated(p_bytes, u_head, p_err)) {
      return (FALSE);
   }
   if (!_refuse_unbuilt_format(p_bytes, u_head, p_err)) {
      return (FALSE);
   }
   if (p_format != NULL) {
      *p_format = detect_format(p_bytes, u_head);
   }
   return (TRUE);
}

/* The gate plus the dispatch rule, for the pixbuf backend (loader.h): the
 * bytes must be ones _dispatch() would have handed to the fallback, i.e.
 * ones no specific backend claims. Whatever a backend claims here is a
 * file that changed between the dispatcher's sniff and the backend's read
 * -- refuse it and let the caller's reload dispatch it properly; decoding
 * it through gdk-pixbuf would bypass the backend's own guards and, for a
 * garbage JXL in a libjxl build, hang glycin-jxl. The refusal is
 * G_IO_ERROR_BUSY: a transient verdict about the file's state, distinct
 * from the gate's INVALID_DATA/NOT_SUPPORTED and from the backends'
 * generic FAILED, so a caller could tell "reload" from "broken" -- none
 * matches it today (viewload.c prints the message and moves on; nothing
 * retries). Its message is the user's, since viewload.c prints it on
 * the status line as is: the format name says what the file turned
 * into, the rest is the advice. A future auto-reload caller must not key
 * on the code alone: G_IO_ERROR_BUSY is also what g_io_error_from_errno()
 * makes of a kernel EBUSY (an open refused for a busy device, say), so a
 * retry loop keyed on BUSY would spin on that too; match this refusal
 * specifically (its message, or give it a code of its own then) and
 * bound the retries. In the minimal build _backend_for() can only name the
 * fallback, so the rule is compiled out there (GGAZE_HAVE_ANY_BACKEND,
 * top-of-file comment) and this IS the plain gate. */
gboolean
loader_sniff_bytes_for_fallback(const guint8 *p_bytes, gsize u_len,
                                GError **p_err) {
   GgazeFormat e_format = GGAZE_FMT_UNKNOWN;
   if (!loader_sniff_bytes(p_bytes, u_len, &e_format, p_err)) {
      return (FALSE);
   }
#if GGAZE_HAVE_ANY_BACKEND
   gsize u_head = MIN(u_len, (gsize)GGAZE_DETECT_SNIFF_LEN);
   if (_backend_for(p_bytes, u_head) != &pixbuf_backend) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_BUSY,
                  "%s file changed while loading; try again",
                  detect_format_name(e_format));
      return (FALSE);
   }
#else
   (void)e_format;
#endif
   return (TRUE);
}

/* Read one open's sniff header and run loader_sniff_bytes() on it: the
 * gate as every path-taking entry point applies it. Returns the byte count
 * read into p_head, or -1 with p_err set. */
static gssize
_sniff_header(GFile *p_file, GCancellable *p_cancel, guint8 *p_head,
              GError **p_err) {
   gssize i_read = loader_read_header(p_file, p_cancel, p_head,
                                      GGAZE_DETECT_SNIFF_LEN, p_err);
   if (i_read < 0) {
      return (-1);
   }
   if (!loader_sniff_bytes(p_head, (gsize)i_read, NULL, p_err)) {
      return (-1);
   }
   return (i_read);
}

/* The one sniff-and-dispatch path behind both loader_load() and the async
 * worker. p_progress may be NULL; a backend without load_progressive() is
 * used through load() regardless. */
static GdkTexture *
_dispatch(GFile *p_file, GCancellable *p_cancel, LoaderProgressCb p_progress,
          gpointer p_progress_data, GError **p_err) {
   guint8 head[GGAZE_DETECT_SNIFF_LEN];
   gssize i_read = _sniff_header(p_file, p_cancel, head, p_err);
   if (i_read < 0) {
      return (NULL);
   }
   const GgazeLoaderBackend *p_be = _backend_for(head, (gsize)i_read);
#if GGAZE_HAVE_ANY_BACKEND
   if (p_be->load_progressive != NULL && p_progress != NULL) {
      return (p_be->load_progressive(p_file, p_cancel, p_progress,
                                     p_progress_data, p_err));
   }
#else
   /* The pixbuf fallback has no progressive load, so the callback can never
    * fire here; the async wrapper still accepts and carries it so the
    * window's calling code is the same in every build. */
   (void)p_progress;
   (void)p_progress_data;
#endif
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

/* --- scaled decode + dimension peek (thumbnail / info) ------------------- */

#if GGAZE_HAVE_ANY_BACKEND
/* TRUE iff a specific (non-pixbuf) backend claims the sniffed header. */
static gboolean
_specific_backend_claims(const guint8 *p_head, gsize u_len) {
   return (_backend_for(p_head, u_len) != &pixbuf_backend);
}
#endif

/* Reject c_path if it is a JPEG whose declared header dimensions exceed the
 * caps, before any GdkPixbuf call sized off them (gdk-pixbuf/glycin
 * pre-allocates off the declared size and stalls ~28 s before its own cap
 * rejects the file). An inconclusive peek (a filler marker pushed the SOF
 * past the scanned prefix) is rejected like an oversized header, or a
 * padded file bypasses the guard. */
static gboolean
_reject_oversized_jpeg_path(const char *c_path, GError **p_err) {
   guint32             u_w, u_h;
   GgazeJpegPeekStatus e_status =
      detect_jpeg_peek_dims_from_path(c_path, &u_w, &u_h);
   if (e_status == GGAZE_JPEG_PEEK_NOT_JPEG) {
      return (TRUE);
   }
   if (e_status == GGAZE_JPEG_PEEK_INCONCLUSIVE) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "jpeg: could not determine declared dimensions within the "
                  "scanned header prefix; refusing to decode");
      return (FALSE);
   }
   return (detect_jpeg_dims_within_bounds(u_w, u_h, p_err));
}

#if GGAZE_HAVE_ANY_BACKEND
/* Full decode through the matching backend, then a pixbuf scaled to fit
 * i_max_px (the texture is already upright). */
static GdkPixbuf *
_scaled_via_backend(GFile *p_file, int i_max_px, GCancellable *p_cancel,
                    GError **p_err) {
   GdkTexture *p_tex = loader_load(p_file, p_cancel, p_err);
   if (p_tex == NULL) {
      return (NULL);
   }
   int        i_w    = gdk_texture_get_width(p_tex);
   int        i_h    = gdk_texture_get_height(p_tex);
   gdouble    d_s    = MIN(1.0, (gdouble)i_max_px / (gdouble)MAX(i_w, i_h));
   int        i_tw   = MAX(1, (int)(i_w * d_s));
   int        i_th   = MAX(1, (int)(i_h * d_s));
   GdkPixbuf *p_full = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, i_w, i_h);
   gdk_texture_download(p_tex, gdk_pixbuf_get_pixels(p_full),
                        (gsize)gdk_pixbuf_get_rowstride(p_full));
   g_object_unref(p_tex);
   /* gdk_texture_download writes premultiplied BGRA (CAIRO_FORMAT_ARGB32);
    * swap to the RGBA GdkPixbuf expects, straight-alpha is close enough for
    * a thumbnail of an opaque photo. */
   guchar *p_px     = gdk_pixbuf_get_pixels(p_full);
   gsize   u_stride = (gsize)gdk_pixbuf_get_rowstride(p_full);
   for (int y = 0; y < i_h; y++) {
      guchar *p_row = p_px + (gsize)y * u_stride;
      for (int x = 0; x < i_w; x++) {
         guchar u_b       = p_row[x * 4 + 0];
         p_row[x * 4 + 0] = p_row[x * 4 + 2];
         p_row[x * 4 + 2] = u_b;
      }
   }
   GdkPixbuf *p_small =
      (i_tw == i_w && i_th == i_h)
         ? GDK_PIXBUF(g_object_ref(p_full))
         : gdk_pixbuf_scale_simple(p_full, i_tw, i_th, GDK_INTERP_BILINEAR);
   g_object_unref(p_full);
   return (p_small);
}
#endif

GdkPixbuf *
loader_load_pixbuf_scaled(GFile *p_file, int i_max_px, GCancellable *p_cancel,
                          GError **p_err) {
   g_return_val_if_fail(G_IS_FILE(p_file), NULL);
   g_return_val_if_fail(i_max_px > 0, NULL);
   /* The same empty/truncated gate as the full load runs before the
    * path-taking gdk_pixbuf_new_from_file_at_scale() below, which would
    * otherwise stall the thumbnail pool on a truncated JXL exactly like the
    * large view (task tb2). Best-effort, not exact: the at-scale call
    * reopens the path (top-of-file comment). */
   guint8 head[GGAZE_DETECT_SNIFF_LEN];
   gssize i_read = _sniff_header(p_file, p_cancel, head, p_err);
   if (i_read < 0) {
      return (NULL);
   }
#if GGAZE_HAVE_ANY_BACKEND
   if (_specific_backend_claims(head, (gsize)i_read)) {
      return (_scaled_via_backend(p_file, i_max_px, p_cancel, p_err));
   }
#endif
   char *c_path = g_file_get_path(p_file);
   if (c_path == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "cannot scale a non-local file");
      return (NULL);
   }
   if (!_reject_oversized_jpeg_path(c_path, p_err)) {
      g_free(c_path);
      return (NULL);
   }
   GdkPixbuf *p_pix = gdk_pixbuf_new_from_file_at_scale(c_path, i_max_px,
                                                        i_max_px, TRUE, p_err);
   g_free(c_path);
   if (p_pix == NULL) {
      return (NULL);
   }
   GdkPixbuf *p_up = pixbuf_util_upright(p_pix);
   g_object_unref(p_pix);
   return (p_up);
}

/* Header-only dimensions for anything GdkPixbuf can parse (the STORED
 * dimensions, before orientation, which is what an EXIF card reports next
 * to its Orientation line): no pixel decode, so no oversized-header stall.
 * TRUE with *p_w and *p_h set when GdkPixbuf parsed a header whose size the
 * decoders would accept; a header they would refuse (oversized) has no
 * honest size to report, so it is FALSE rather than an echoed crafted
 * 65500x65500. On a glycin desktop this call spawns a sandboxed loader, so
 * it is the LAST resort in loader_peek_dimensions(), never the first. */
static gboolean
_peek_via_pixbuf_header(const char *c_path, int *p_w, int *p_h) {
   *p_w = 0;
   *p_h = 0;
   if (gdk_pixbuf_get_file_info(c_path, p_w, p_h) == NULL || *p_w <= 0 ||
       *p_h <= 0) {
      return (FALSE);
   }
   if (!detect_dims_within_bounds("info", (guint64)*p_w, (guint64)*p_h, NULL,
                                  NULL)) {
      *p_w = 0;
      *p_h = 0;
   }
   return (*p_w > 0);
}

/* What the decoder-free JPEG peek decided for loader_peek_dimensions():
 * gdk-pixbuf's header parse is consulted for UNDECIDED only. */
typedef enum {
   JPEG_PEEK_SIZED,    /* p_w and p_h hold a size within the caps */
   JPEG_PEEK_REFUSED,  /* oversized, or the SOF lies past the scanned
                        * prefix: no size, and no gdk-pixbuf either */
   JPEG_PEEK_UNDECIDED /* no SOF (malformed marker stream, SOS first) or a
                        * zero side (DNL-deferred height): gdk-pixbuf's
                        * header parse may still know */
} JpegPeekVerdict;

/* Decoder-free JPEG dimensions: the SOF scan detect.c already does for the
 * oversized-header guard, no gdk-pixbuf (no glycin sandbox spawn) and no
 * libjpeg (whose only way to learn a size is a full decode). Three
 * outcomes, because two of the peek's failures must NOT fall through:
 *   - a declared size over the cap is REFUSED outright. There is no honest
 *     size to report (same as _peek_via_pixbuf_header()), and asking
 *     gdk-pixbuf would spawn a glycin sandbox only to reject its answer
 *     again;
 *   - a SOF past the scanned prefix (GGAZE_JPEG_PEEK_INCONCLUSIVE) is
 *     REFUSED too: fail closed, exactly as loader_load_pixbuf_scaled() does
 *     for the same file, so a padded JPEG the thumbnail refuses is not one
 *     the info card sizes. That gdk-pixbuf's header parse decodes no pixels
 *     does not make it free -- on a glycin desktop it is a sandbox spawn;
 *   - a zero side is UNDECIDED, not SIZED: a DNL-deferred height of 0 is
 *     legal JPEG that detect_jpeg_dims_within_bounds() deliberately lets
 *     through, and reporting 0 as a dimension would have been TRUE with a
 *     meaningless size. Malformed marker streams (GGAZE_JPEG_PEEK_NOT_JPEG)
 *     are UNDECIDED for the same reason the scaled path lets them through:
 *     the real parser produces the definitive verdict, at least as fast. */
static JpegPeekVerdict
_peek_via_jpeg_header(const char *c_path, int *p_w, int *p_h) {
   guint32             u_w, u_h;
   GgazeJpegPeekStatus e_status =
      detect_jpeg_peek_dims_from_path(c_path, &u_w, &u_h);
   if (e_status == GGAZE_JPEG_PEEK_INCONCLUSIVE) {
      return (JPEG_PEEK_REFUSED);
   }
   if (e_status != GGAZE_JPEG_PEEK_OK) {
      return (JPEG_PEEK_UNDECIDED);
   }
   if (u_w == 0 || u_h == 0) {
      return (JPEG_PEEK_UNDECIDED);
   }
   if (!detect_jpeg_dims_within_bounds(u_w, u_h, NULL)) {
      return (JPEG_PEEK_REFUSED);
   }
   *p_w = (int)u_w;
   *p_h = (int)u_h;
   return (JPEG_PEEK_SIZED);
}

#if GGAZE_HAVE_ANY_BACKEND
/* Dimensions through the claiming backend's full decode (upright ones, the
 * header comment says so): the only way to size a JXL/AVIF/HEIF without
 * gdk-pixbuf, and the only acceptable one, because gdk_pixbuf_get_file_info()
 * on such a file goes to a glycin loader that hangs on garbage JXL and pays
 * a sandbox spawn on a valid one (task tb2). */
static gboolean
_peek_via_backend(GFile *p_file, int *p_w, int *p_h) {
   GdkTexture *p_tex = loader_load(p_file, NULL, NULL);
   if (p_tex == NULL) {
      return (FALSE);
   }
   *p_w = gdk_texture_get_width(p_tex);
   *p_h = gdk_texture_get_height(p_tex);
   g_object_unref(p_tex);
   return (TRUE);
}
#endif

/* Route an already-sniffed (and gate-cleared) header to the cheapest SAFE
 * size source, in this order:
 *   - JPEG: the decoder-free SOF peek, whatever backends are built (the
 *     jpeg backend's only way to learn a size is a full decode). It
 *     decides for an oversized or prefix-exceeding header (FALSE, nothing
 *     else asked) and defers only what it could not parse;
 *   - a format a specific backend claims (JXL/AVIF/HEIF): that backend,
 *     never gdk-pixbuf (see _peek_via_backend());
 *   - everything else: the gdk-pixbuf header parse.
 * The previous order tried gdk-pixbuf FIRST and only fell back to a
 * backend, which is what let a garbage JXL hang the info worker even in a
 * build with libjxl. */
static gboolean
_peek_dims_sniffed(GFile *p_file, const char *c_path, const guint8 *p_head,
                   gsize u_len, int *p_w, int *p_h) {
   GgazeFormat e_format = detect_format(p_head, u_len);
   if (e_format == GGAZE_FMT_JPEG) {
      JpegPeekVerdict e_verdict = _peek_via_jpeg_header(c_path, p_w, p_h);
      if (e_verdict != JPEG_PEEK_UNDECIDED) {
         return (e_verdict == JPEG_PEEK_SIZED);
      }
   }
#if GGAZE_HAVE_ANY_BACKEND
   else if (_specific_backend_claims(p_head, u_len)) {
      return (_peek_via_backend(p_file, p_w, p_h));
   }
#else
   (void)p_file;
#endif
   return (_peek_via_pixbuf_header(c_path, p_w, p_h));
}

gboolean
loader_peek_dimensions(GFile *p_file, int *p_w, int *p_h) {
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   g_return_val_if_fail(p_w != NULL && p_h != NULL, FALSE);
   /* No path, no gdk-pixbuf header parse and no info card worth a decode:
    * decided before any I/O. */
   char *c_path = g_file_get_path(p_file);
   if (c_path == NULL) {
      return (FALSE);
   }
   /* An empty, truncated or not-built-in file has no dimensions and must
    * not reach any decoder (glycin stall, task tb2). Every size source
    * below reopens the path, so this is best-effort against a swap in
    * between (top-of-file comment). */
   guint8   head[GGAZE_DETECT_SNIFF_LEN];
   gssize   i_read = _sniff_header(p_file, NULL, head, NULL);
   gboolean b_ok   = FALSE;
   if (i_read > 0) {
      b_ok = _peek_dims_sniffed(p_file, c_path, head, (gsize)i_read, p_w, p_h);
   }
   g_free(c_path);
   return (b_ok);
}
