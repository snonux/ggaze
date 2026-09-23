/*:*
 * ggaze — is this PNG / JPEG container complete?
 *
 * See intact.h. Both walks run on a GFileInputStream through streamread.c
 * and report a short file as G_IO_ERROR_INVALID_DATA with the word
 * "truncated", so the status line names the cause.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "intact.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

#include "streamread.h"

static gboolean
_truncated(const char *c_what, GError **p_err) {
   g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "%s is truncated (ends before its %s)", c_what,
               c_what[0] == 'P' ? "IEND chunk" : "EOI marker");
   return (FALSE);
}

/* The PNG walk after the signature: chunk header (length, type), skip data
 * + CRC, until IEND. */
static gboolean
_png_walk(GInputStream *p_in, GError **p_err) {
   guint8 c_sig[8];
   if (streamread_exact(p_in, c_sig, sizeof(c_sig), p_err) != STREAMREAD_OK) {
      return (*p_err != NULL ? FALSE : _truncated("PNG", p_err));
   }
   for (;;) {
      guint8           c_hdr[8];
      StreamReadStatus e_rd = streamread_exact(p_in, c_hdr, 8, p_err);
      if (e_rd != STREAMREAD_OK) {
         return (e_rd == STREAMREAD_ERROR ? FALSE : _truncated("PNG", p_err));
      }
      gsize u_len = ((gsize)c_hdr[0] << 24) | ((gsize)c_hdr[1] << 16) |
                    ((gsize)c_hdr[2] << 8) | c_hdr[3];
      e_rd        = streamread_skip(p_in, u_len + 4, p_err); /* data + CRC */
      if (e_rd != STREAMREAD_OK) {
         return (e_rd == STREAMREAD_ERROR ? FALSE : _truncated("PNG", p_err));
      }
      if (memcmp(c_hdr + 4, "IEND", 4) == 0) {
         return (TRUE);
      }
   }
}

/* Read the next marker code after its 0xFF prefix (fill bytes allowed).
 * A byte that is not 0xFF where a marker must start is INVALID_DATA. */
static StreamReadStatus
_jpeg_next_marker(GInputStream *p_in, guint8 *p_code, GError **p_err) {
   guint8           u_byte;
   StreamReadStatus e_rd = streamread_exact(p_in, &u_byte, 1, p_err);
   if (e_rd != STREAMREAD_OK) {
      return (e_rd);
   }
   if (u_byte != 0xFF) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "JPEG marker expected, found 0x%02x", u_byte);
      return (STREAMREAD_ERROR);
   }
   do {
      e_rd = streamread_exact(p_in, &u_byte, 1, p_err);
   } while (e_rd == STREAMREAD_OK && u_byte == 0xFF);
   *p_code = u_byte;
   return (e_rd);
}

/* Skip one length-prefixed segment. */
static StreamReadStatus
_jpeg_skip_segment(GInputStream *p_in, GError **p_err) {
   guint8           c_len[2];
   StreamReadStatus e_rd = streamread_exact(p_in, c_len, 2, p_err);
   if (e_rd != STREAMREAD_OK) {
      return (e_rd);
   }
   gsize u_len = ((gsize)c_len[0] << 8) | c_len[1];
   if (u_len < 2) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "JPEG segment length %" G_GSIZE_FORMAT " is invalid", u_len);
      return (STREAMREAD_ERROR);
   }
   return (streamread_skip(p_in, u_len - 2, p_err));
}

/* Walk the marker segments after SOI up to the first SOS (0xDA). TEM,
 * RSTn and SOI carry no length; EOI before any SOS is a file with no
 * image at all, which counts as truncated for the caller's purpose. */
static StreamReadStatus
_jpeg_walk_to_sos(GInputStream *p_in, GError **p_err) {
   guint8           c_soi[2];
   StreamReadStatus e_rd = streamread_exact(p_in, c_soi, 2, p_err);
   for (;;) {
      guint8 u_code = 0;
      if (e_rd != STREAMREAD_OK) {
         return (e_rd);
      }
      e_rd = _jpeg_next_marker(p_in, &u_code, p_err);
      if (e_rd != STREAMREAD_OK) {
         return (e_rd);
      }
      if (u_code == 0xDA) {
         return (STREAMREAD_OK);
      }
      if (u_code == 0xD9) {
         return (STREAMREAD_EOF);
      }
      if (u_code != 0x01 && (u_code < 0xD0 || u_code > 0xD8)) {
         e_rd = _jpeg_skip_segment(p_in, p_err);
      }
   }
}

/* Sequential scan of the rest of the stream for FF D9, carrying the last
 * byte across block boundaries. In entropy-coded data every 0xFF is byte-
 * stuffed (FF 00) or a marker, so FF D9 there can only be the EOI. */
static StreamReadStatus
_jpeg_find_eoi(GInputStream *p_in, GError **p_err) {
   guint8   c_buf[65536];
   gboolean b_ff = FALSE;
   for (;;) {
      gssize i_n = g_input_stream_read(p_in, c_buf, sizeof(c_buf), NULL, p_err);
      if (i_n < 0) {
         return (STREAMREAD_ERROR);
      }
      if (i_n == 0) {
         return (STREAMREAD_EOF);
      }
      for (gssize i = 0; i < i_n; i++) {
         if (b_ff && c_buf[i] == 0xD9) {
            return (STREAMREAD_OK);
         }
         b_ff = (c_buf[i] == 0xFF);
      }
   }
}

/* Open p_file and run fn_walk on it; a walk that ends in EOF is reported
 * as truncated, an error as itself. */
static gboolean
_check(GFile *p_file, const char                                       *c_what,
       StreamReadStatus (*fn_walk)(GInputStream *, GError **), GError **p_err) {
   GError           *p_local = NULL;
   GFileInputStream *p_in    = g_file_read(p_file, NULL, &p_local);
   if (p_in == NULL) {
      g_propagate_error(p_err, p_local);
      return (FALSE);
   }
   StreamReadStatus e_rd = fn_walk(G_INPUT_STREAM(p_in), &p_local);
   g_input_stream_close(G_INPUT_STREAM(p_in), NULL, NULL);
   g_object_unref(p_in);
   if (p_local != NULL) {
      g_propagate_error(p_err, p_local);
      return (FALSE);
   }
   return (e_rd == STREAMREAD_OK ? TRUE : _truncated(c_what, p_err));
}

static StreamReadStatus
_png_check(GInputStream *p_in, GError **p_err) {
   return (_png_walk(p_in, p_err) ? STREAMREAD_OK : STREAMREAD_ERROR);
}

gboolean
intact_png(GFile *p_file, GError **p_err) {
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   return (_check(p_file, "PNG", _png_check, p_err));
}

static StreamReadStatus
_jpeg_check(GInputStream *p_in, GError **p_err) {
   StreamReadStatus e_rd = _jpeg_walk_to_sos(p_in, p_err);
   if (e_rd != STREAMREAD_OK) {
      return (e_rd);
   }
   return (_jpeg_find_eoi(p_in, p_err));
}

gboolean
intact_jpeg(GFile *p_file, GError **p_err) {
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   return (_check(p_file, "JPEG", _jpeg_check, p_err));
}
