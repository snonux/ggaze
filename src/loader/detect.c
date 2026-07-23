/*:*
 * ggaze — image format detection
 *
 * Magic-byte sniffing. No GTK -> unit-testable. detect_format() and
 * detect_jpeg_peek_dims() are pure functions (no I/O) operating on bytes
 * already in memory. detect_jpeg_peek_dims_from_path() is the one exception:
 * it does a small bounded read (GGAZE_JPEG_PEEK_LEN) for call sites that
 * hand a path straight to a decoder rather than loading bytes themselves
 * (thumbnail.c, info.c). detect_jpeg_peek_dims()/_from_path() plus
 * detect_jpeg_dims_within_bounds() let every JPEG-decoding call site
 * (pixbuf.c, jpeg.c, thumbnail.c, info.c) reject an oversized declared
 * header before decoding, without duplicating the bound comparison or error
 * message (mu0).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "detect.h"

#include <gio/gio.h>
#include <string.h>

GgazeFormat
detect_format(const guint8 *p_head, gsize u_len) {
   if (p_head == NULL || u_len == 0) {
      return (GGAZE_FMT_UNKNOWN);
   }

   /* JPEG: FF D8 FF */
   if (u_len >= 3 && p_head[0] == 0xFF && p_head[1] == 0xD8 &&
       p_head[2] == 0xFF) {
      return (GGAZE_FMT_JPEG);
   }

   /* PNG: 89 50 4E 47 0D 0A 1A 0A */
   if (u_len >= 8 && p_head[0] == 0x89 && p_head[1] == 'P' &&
       p_head[2] == 'N' && p_head[3] == 'G' && p_head[4] == 0x0D &&
       p_head[5] == 0x0A && p_head[6] == 0x1A && p_head[7] == 0x0A) {
      return (GGAZE_FMT_PNG);
   }

   /* GIF: "GIF8" */
   if (u_len >= 4 && p_head[0] == 'G' && p_head[1] == 'I' && p_head[2] == 'F' &&
       p_head[3] == '8') {
      return (GGAZE_FMT_GIF);
   }

   /* WebP: RIFF .... WEBP */
   if (u_len >= 12 && memcmp(p_head, "RIFF", 4) == 0 &&
       memcmp(p_head + 8, "WEBP", 4) == 0) {
      return (GGAZE_FMT_WEBP);
   }

   /* TIFF: II 2A 00 (little) | MM 00 2A (big) */
   if (u_len >= 4 && ((p_head[0] == 'I' && p_head[1] == 'I' &&
                       p_head[2] == 0x2A && p_head[3] == 0x00) ||
                      (p_head[0] == 'M' && p_head[1] == 'M' &&
                       p_head[2] == 0x00 && p_head[3] == 0x2A))) {
      return (GGAZE_FMT_TIFF);
   }

   /* ICO: 00 00 01 00 */
   if (u_len >= 4 && p_head[0] == 0x00 && p_head[1] == 0x00 &&
       p_head[2] == 0x01 && p_head[3] == 0x00) {
      return (GGAZE_FMT_ICO);
   }

   /* JPEG XL: codestream FF 0A, or container 00 00 00 0C "JXL " */
   if (u_len >= 2 && p_head[0] == 0xFF && p_head[1] == 0x0A) {
      return (GGAZE_FMT_JXL);
   }
   if (u_len >= 12 && p_head[0] == 0x00 && p_head[1] == 0x00 &&
       p_head[2] == 0x00 && p_head[3] == 0x0C &&
       memcmp(p_head + 4, "JXL ", 4) == 0) {
      return (GGAZE_FMT_JXL);
   }

   /* AVIF / HEIF: ISO BMFF ftyp box at offset 4; brand at offset 8. */
   if (u_len >= 12 && memcmp(p_head + 4, "ftyp", 4) == 0) {
      const guint8 *p_brand = p_head + 8;
      if (memcmp(p_brand, "avif", 4) == 0 || memcmp(p_brand, "avis", 4) == 0) {
         return (GGAZE_FMT_AVIF);
      }
      if (memcmp(p_brand, "heic", 4) == 0 || memcmp(p_brand, "heix", 4) == 0 ||
          memcmp(p_brand, "mif1", 4) == 0) {
         return (GGAZE_FMT_HEIF);
      }
   }

   return (GGAZE_FMT_UNKNOWN);
}

/* JPEG marker codes relevant to the SOF scan below. SOF0-SOF15 span
 * 0xC0-0xCF, but three codes in that range are reused for non-frame
 * segments and must be skipped rather than misread as a frame header. */
#define GGAZE_JPEG_MARKER_DHT 0xC4
#define GGAZE_JPEG_MARKER_JPG 0xC8
#define GGAZE_JPEG_MARKER_DAC 0xCC
#define GGAZE_JPEG_MARKER_SOS 0xDA

static gboolean
_is_sof_marker(guint8 u_marker) {
   return (u_marker >= 0xC0 && u_marker <= 0xCF &&
           u_marker != GGAZE_JPEG_MARKER_DHT &&
           u_marker != GGAZE_JPEG_MARKER_JPG &&
           u_marker != GGAZE_JPEG_MARKER_DAC);
}

/* Read a SOF segment's height/width at u_i (the segment's marker byte
 * offset) into *p_w and *p_h. Caller has already verified u_i+9 <= u_len, so no
 * further bounds check is needed: length(2) precision(1) height(2)
 * width(2), starting right after the 2-byte marker. */
static void
_read_sof_dims(const guint8 *p_buf, gsize u_i, guint32 *p_w, guint32 *p_h) {
   *p_h = ((guint32)p_buf[u_i + 5] << 8) | p_buf[u_i + 6];
   *p_w = ((guint32)p_buf[u_i + 7] << 8) | p_buf[u_i + 8];
}

gboolean
detect_jpeg_peek_dims(const guint8 *p_buf, gsize u_len, guint32 *p_w,
                      guint32 *p_h) {
   if (p_buf == NULL || u_len < 4 || p_buf[0] != 0xFF || p_buf[1] != 0xD8) {
      return (FALSE);
   }
   gsize u_i = 2;
   while (u_i + 4 <= u_len) {
      if (p_buf[u_i] != 0xFF) {
         u_i++; /* resync on a stray non-marker byte */
         continue;
      }
      guint8 u_marker = p_buf[u_i + 1];
      /* 0xFF fill bytes may pad before the real marker code; TEM (0x01) and
       * RSTn/SOI/EOI (0xD0-0xD9) carry no length segment and never precede
       * the first SOF in a well-formed stream, so skip past just the marker
       * pair rather than misreading the next two bytes as a length. */
      if (u_marker == 0xFF) {
         u_i++;
         continue;
      }
      if (u_marker == 0x01 || (u_marker >= 0xD0 && u_marker <= 0xD9)) {
         u_i += 2;
         continue;
      }
      guint32 u_seglen = ((guint32)p_buf[u_i + 2] << 8) | p_buf[u_i + 3];
      if (u_seglen < 2) {
         return (FALSE); /* malformed: length must include itself */
      }
      if (_is_sof_marker(u_marker)) {
         if (u_i + 9 > u_len) {
            return (FALSE); /* truncated SOF segment */
         }
         _read_sof_dims(p_buf, u_i, p_w, p_h);
         return (TRUE);
      }
      if (u_marker == GGAZE_JPEG_MARKER_SOS) {
         return (FALSE); /* scan data reached, no SOF seen first */
      }
      u_i += 2 + (gsize)u_seglen;
   }
   return (FALSE);
}

gboolean
detect_jpeg_peek_dims_from_path(const char *c_path, guint32 *p_w,
                                guint32 *p_h) {
   if (c_path == NULL) {
      return (FALSE);
   }
   GFile            *p_file = g_file_new_for_path(c_path);
   GFileInputStream *p_in   = g_file_read(p_file, NULL, NULL);
   g_object_unref(p_file);
   if (p_in == NULL) {
      return (FALSE);
   }
   guint8 buf[GGAZE_JPEG_PEEK_LEN];
   gssize n =
      g_input_stream_read(G_INPUT_STREAM(p_in), buf, sizeof(buf), NULL, NULL);
   g_object_unref(p_in);
   if (n <= 0) {
      return (FALSE);
   }
   return (detect_jpeg_peek_dims(buf, (gsize)n, p_w, p_h));
}

gboolean
detect_jpeg_dims_within_bounds(guint32 u_w, guint32 u_h, GError **p_err) {
   if (u_w > GGAZE_JPEG_MAX_SIDE || u_h > GGAZE_JPEG_MAX_SIDE ||
       (guint64)u_w * u_h > GGAZE_JPEG_MAX_PIXELS) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "jpeg: image too large (%ux%u, max %d per side / %llu "
                  "pixels)",
                  u_w, u_h, GGAZE_JPEG_MAX_SIDE,
                  (unsigned long long)GGAZE_JPEG_MAX_PIXELS);
      return (FALSE);
   }
   return (TRUE);
}