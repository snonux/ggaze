/*:*
 * ggaze — bounded reads on a GInputStream
 *
 * See streamread.h.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "streamread.h"

#include <gio/gio.h>
#include <glib.h>

StreamReadStatus
streamread_exact(GInputStream *p_in, guint8 *p_buf, gsize u_len,
                 GError **p_err) {
   g_return_val_if_fail(G_IS_INPUT_STREAM(p_in), STREAMREAD_ERROR);
   gsize u_got = 0;
   if (!g_input_stream_read_all(p_in, p_buf, u_len, &u_got, NULL, p_err)) {
      return (STREAMREAD_ERROR);
   }
   return (u_got == u_len ? STREAMREAD_OK : STREAMREAD_EOF);
}

StreamReadStatus
streamread_skip(GInputStream *p_in, gsize u_len, GError **p_err) {
   g_return_val_if_fail(G_IS_INPUT_STREAM(p_in), STREAMREAD_ERROR);
   /* g_input_stream_skip() may skip less than asked (a seekable stream
    * seeks; a plain one reads and discards a buffer at a time): loop until
    * done, and treat a zero skip as the end of the stream. */
   while (u_len > 0) {
      gssize i_n = g_input_stream_skip(p_in, u_len, NULL, p_err);
      if (i_n < 0) {
         return (STREAMREAD_ERROR);
      }
      if (i_n == 0) {
         return (STREAMREAD_EOF);
      }
      u_len -= (gsize)i_n;
   }
   return (STREAMREAD_OK);
}

/* One byte of the marker search, counted against the padding cap: past
 * it the "padding" is no padding (a file of junk after its SOI) and the
 * walk stops instead of reading the whole file a byte at a time. */
static StreamReadStatus
_marker_byte(GInputStream *p_in, guint8 *p_byte, gsize *pu_seen,
             GError **p_err) {
   if (++*pu_seen > STREAMREAD_JPEG_MAX_PAD + 2) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "JPEG: no marker within %u bytes",
                  (unsigned)STREAMREAD_JPEG_MAX_PAD);
      return (STREAMREAD_ERROR);
   }
   return (streamread_exact(p_in, p_byte, 1, p_err));
}

StreamReadStatus
streamread_jpeg_marker(GInputStream *p_in, guint8 *p_code, GError **p_err) {
   g_return_val_if_fail(G_IS_INPUT_STREAM(p_in), STREAMREAD_ERROR);
   guint8           u_byte = 0;
   gsize            u_seen = 0;
   StreamReadStatus e_rd   = STREAMREAD_OK;
   do {
      /* Anything up to the next 0xFF is padding (libjpeg skips it). */
      do {
         e_rd = _marker_byte(p_in, &u_byte, &u_seen, p_err);
      } while (e_rd == STREAMREAD_OK && u_byte != 0xFF);
      /* Then any run of 0xFF fill bytes, up to the code. */
      while (e_rd == STREAMREAD_OK && u_byte == 0xFF) {
         e_rd = _marker_byte(p_in, &u_byte, &u_seen, p_err);
      }
   } while (e_rd == STREAMREAD_OK && u_byte == 0x00); /* FF 00: no marker */
   *p_code = u_byte;
   return (e_rd);
}

/* --- CRC-32 (see streamread.h) ------------------------------------------ */

static guint32 u_crc_table[256];

static void
_crc_init(void) {
   static gsize u_once = 0;
   if (g_once_init_enter(&u_once)) {
      for (guint32 u = 0; u < 256; u++) {
         guint32 u_c = u;
         for (int i = 0; i < 8; i++) {
            u_c = (u_c & 1) ? 0xEDB88320u ^ (u_c >> 1) : u_c >> 1;
         }
         u_crc_table[u] = u_c;
      }
      g_once_init_leave(&u_once, 1);
   }
}

guint32
streamread_crc32(guint32 u_crc, const guint8 *p, gsize u_len) {
   _crc_init();
   for (gsize u = 0; u < u_len; u++) {
      u_crc = u_crc_table[(u_crc ^ p[u]) & 0xFF] ^ (u_crc >> 8);
   }
   return (u_crc);
}
