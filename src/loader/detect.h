#ifndef GGAZE_DETECT_H
#define GGAZE_DETECT_H

/*:*
 * ggaze — image format detection
 *
 * Content-sniffing (magic bytes), never extension-based. detect_format() takes
 * the first N bytes of a file and returns the detected GgazeFormat. The loader
 * uses this to dispatch to the right backend; see docs/architecture.md "Image
 * decode" and docs/tech-stack.md.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
   GGAZE_FMT_UNKNOWN = 0,
   GGAZE_FMT_JPEG, /* FF D8 FF */
   GGAZE_FMT_PNG,  /* 89 50 4E 47 0D 0A 1A 0A */
   GGAZE_FMT_GIF,  /* "GIF8" */
   GGAZE_FMT_WEBP, /* RIFF .... WEBP */
   GGAZE_FMT_TIFF, /* II 2A 00 | MM 00 2A */
   GGAZE_FMT_ICO,  /* 00 00 01 00 */
   GGAZE_FMT_JXL,  /* FF 0A | "....JXL " container */
   GGAZE_FMT_AVIF, /* ftyp avif/avis */
   GGAZE_FMT_HEIF  /* ftyp heic/heix/mif1 */
} GgazeFormat;

/* Sniff p_head (u_len bytes) and return the detected format. Never reads
 * past u_len. Returns GGAZE_FMT_UNKNOWN if the buffer is too short or
 * unrecognized. */
GgazeFormat detect_format(const guint8 *p_head, gsize u_len);

G_END_DECLS

#endif /* GGAZE_DETECT_H */