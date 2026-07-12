/*:*
 * ggaze — image format detection
 *
 * Magic-byte sniffing. Pure function, no I/O, no GTK -> unit-testable.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "detect.h"

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