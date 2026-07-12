/*:*
 * ggaze — image loader dispatcher
 *
 * Reads a short header, sniffs the format, and hands off to the first backend
 * whose can_load() accepts it. BACKENDS[] is ordered so format-specific
 * backends (JXL/AVIF/HEIF, M5) win over the GdkPixbuf fallback, which is last
 * and accepts GGAZE_FMT_UNKNOWN. M1 ships only the pixbuf backend.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "loader.h"

#include <gio/gio.h>
#include <glib.h>

#include "detect.h"

/* Registered backends, priority order (specific first, fallback LAST).
 * pixbuf_backend must remain last: it accepts GGAZE_FMT_UNKNOWN. */
static const GgazeLoaderBackend *BACKENDS[] = {
   /* jxl_backend, avif_backend, heif_backend land here in M5. */
   &pixbuf_backend,
};

#define GGAZE_SNIFF_LEN 64

static gsize
_read_header(GFile *p_file, GCancellable *p_cancel, guint8 *p_head, gsize u_max,
             GError **p_err) {
   GError           *p_sub = NULL;
   GFileInputStream *p_in  = g_file_read(p_file, p_cancel, &p_sub);
   if (p_in == NULL) {
      g_propagate_error(p_err, p_sub);
      return (0);
   }
   gssize n = g_input_stream_read(G_INPUT_STREAM(p_in), p_head, u_max, p_cancel,
                                  &p_sub);
   g_object_unref(p_in);
   if (n < 0) {
      g_propagate_error(p_err, p_sub);
      return (0);
   }
   return ((gsize)n);
}

GdkTexture *
loader_load(GFile *p_file, GCancellable *p_cancel, GError **p_err) {
   g_return_val_if_fail(G_IS_FILE(p_file), NULL);

   guint8 head[GGAZE_SNIFF_LEN];
   gsize  u_read = _read_header(p_file, p_cancel, head, GGAZE_SNIFF_LEN, p_err);
   if (u_read == 0 && p_err != NULL && *p_err != NULL) {
      return (NULL);
   }

   for (gsize u_i = 0; u_i < G_N_ELEMENTS(BACKENDS); u_i++) {
      if (BACKENDS[u_i]->can_load(head, u_read)) {
         return (BACKENDS[u_i]->load(p_file, p_cancel, p_err));
      }
   }

   g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "unsupported or unrecognized image format");
   return (NULL);
}