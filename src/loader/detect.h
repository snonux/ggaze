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

/* Per-dimension / total-pixel caps applied to a JPEG's declared header
 * dimensions before either backend that can decode JPEG (jpeg.c's
 * libjpeg-driven decode, pixbuf.c's GdkPixbuf-driven decode) commits to an
 * allocation sized off them. Defined once here so the two backends cannot
 * drift out of sync (mu0). 32768 per side / 100M total
 * pixels (~400 MB RGBA) comfortably admits real camera photos. */
#define GGAZE_JPEG_MAX_SIDE 32768
#define GGAZE_JPEG_MAX_PIXELS 100000000ULL

/* Bounded prefix read by detect_jpeg_peek_dims_from_path() below. Generous
 * enough for real-world APPn/EXIF/ICC segments ahead of the SOF marker while
 * staying far cheaper than reading a whole multi-megabyte photo just to peek
 * its declared dimensions. */
#define GGAZE_JPEG_PEEK_LEN 65536

/* Scan a JPEG byte buffer (from its start, FF D8 SOI) for the first SOF0-
 * SOF15 frame marker and extract its declared width/height, without
 * decoding any pixel data. APPn/EXIF/quantization/Huffman segments ahead of
 * the SOF are skipped via their own declared segment length, so this never
 * touches entropy-coded scan data -- it is exactly the header parsing a real
 * JPEG decoder performs to size its output buffers, minus the decode.
 * Callers use it to reject an oversized declared header before handing the
 * buffer to a real decoder: GdkPixbuf's own JPEG loader (glycin, gdk-pixbuf
 * >=2.42) was found to pre-allocate off the declared size before validating
 * against real scan data, so an unbounded declared size is a multi-second
 * latency stall even though GdkPixbuf itself does not crash (mu0 review).
 * Returns TRUE and stores the dimensions in *p_w and *p_h if a well-formed SOF
 * marker is found within p_buf; FALSE (dims left untouched) if none is
 * found, the buffer is truncated, or it does not start with a JPEG SOI --
 * callers should then let the real decoder produce the real error. */
gboolean detect_jpeg_peek_dims(const guint8 *p_buf, gsize u_len, guint32 *p_w,
                               guint32 *p_h);

/* Same as detect_jpeg_peek_dims(), but for a file on disk that has not
 * already been read into memory: opens c_path and scans only a bounded
 * prefix (GGAZE_JPEG_PEEK_LEN), never the whole file. Used by call sites that
 * pass a path straight to a decoder (thumbnail.c, info.c) rather than
 * loading bytes themselves first (mu0 review round 2). Returns FALSE (dims
 * untouched) if the file cannot be opened, is shorter than a JPEG SOI, or no
 * SOF marker is found within the prefix -- callers should then let the real
 * decoder run as normal; a legitimate header that happens to exceed the
 * prefix is a missed optimization, not a security regression, since the real
 * decoder still enforces its own limits. */
gboolean detect_jpeg_peek_dims_from_path(const char *c_path, guint32 *p_w,
                                         guint32 *p_h);

/* Compare a JPEG's declared u_w/u_h against GGAZE_JPEG_MAX_SIDE/MAX_PIXELS.
 * Returns TRUE if within bounds. On an oversized violation, returns FALSE
 * and (if p_err is non-NULL) sets *p_err to a G_IO_ERROR describing the
 * limit -- callers that don't propagate a GError (e.g. info.c, which simply
 * omits dimensions on any failure) may pass NULL. Centralizing the bound
 * comparison and message here keeps the four JPEG decode call sites
 * (pixbuf.c, jpeg.c, thumbnail.c, info.c) from drifting out of sync (mu0). */
gboolean detect_jpeg_dims_within_bounds(guint32 u_w, guint32 u_h,
                                        GError **p_err);

G_END_DECLS

#endif /* GGAZE_DETECT_H */