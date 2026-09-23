#ifndef GGAZE_INTACT_H
#define GGAZE_INTACT_H

/*:*
 * ggaze — will GEGL's PNG / JPEG loader get through this file? (plain C,
 * no pixel decode)
 *
 * The enhance path decodes a profiled PNG or JPEG through GEGL's own
 * loaders for their ICC awareness (decision #45), and those loaders --
 * gegl:png-load and gegl:jpg-load, measured on gegl 0.4.72 -- fail badly
 * where ggaze's loader fails cleanly:
 *
 *   - on a file that ends early their stream reader starts the file over
 *     on EOF, so libpng sees the iCCP chunk twice ("iCCP: duplicate"),
 *     libjpeg sees "two SOF markers", and the load spins forever;
 *   - on a PNG whose image data is corrupt (a bad CRC, a deflate stream
 *     that does not inflate or ends short, a bad row filter) libpng's
 *     error is logged as "failed to open file" and the op yields a
 *     header-sized buffer of black / partial rows, no error at all;
 *   - on a JPEG libjpeg gives up on (two SOF markers, a bogus Huffman
 *     table, ...) gegl:jpg-load EXITS THE PROCESS: it installs
 *     jpeg_std_error() without a longjmp handler, and libjpeg's default
 *     error_exit() calls exit(1).
 *
 * So the enhancer hands a file to GEGL only when this module vouches for
 * it, and takes ggaze's own loader (which reports "could not decode"
 * exactly as before xb2) otherwise. A PNG is walked chunk by chunk -- the
 * critical chunks' CRCs checked, the IDAT stream inflated (into a scratch
 * buffer, never kept) to the size IHDR promises, every row's filter byte
 * checked -- which is the whole of what libpng can fail on short of a
 * broken IHDR / PLTE, which the header checks and the decoded-extent check
 * in the enhancer cover. A JPEG is walked marker by marker to SOS and then
 * read once for the EOI (intact_jpeg), and then decoded once, at 1/8
 * scale, by the same libjpeg under a longjmp handler (intact_jpeg_decodes):
 * whatever would make libjpeg give up inside GEGL makes it give up here
 * first, harmlessly. Both walks report the stored size from IHDR / the
 * first SOF, so the caller needs no separate header peek (and opens the
 * file once less).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

/* The stored (not EXIF-oriented) size the walk found in the header: a
 * PNG's IHDR, a JPEG's first SOFn. 0 x 0 when the walk failed before it. */
typedef struct {
   guint32 u_w;
   guint32 u_h;
} IntactSize;

/* TRUE iff p_file is a PNG whose chunks all lie within the file and end
 * with IEND, whose critical chunks' CRCs match, and whose IDAT data
 * inflates to at least the image data IHDR declares (Adam7 included) with
 * a valid filter type on every row. FALSE with G_IO_ERROR_INVALID_DATA
 * ("truncated" for a file that ends early, "corrupt" otherwise), or with
 * the GIO error of a read failure. p_size (nullable) receives IHDR's
 * size either way. */
gboolean intact_png(GFile *p_file, IntactSize *p_size, GError **p_err);

/* TRUE iff p_file is a JPEG whose marker segments up to the first SOS all
 * lie within the file and whose bytes after that SOS contain an EOI marker
 * (FF D9) -- the one thing libjpeg needs to find before the end of the
 * data. An EOI inside an APPn segment ahead of SOS (an EXIF thumbnail's)
 * does not count; padding between segments is skipped as libjpeg does
 * (streamread_jpeg_marker). FALSE with G_IO_ERROR_INVALID_DATA otherwise,
 * or with the GIO error of a read failure. p_size (nullable) receives the
 * first SOF's size either way (0 x 0 when none precedes SOS; a SOF may
 * declare 0 rows, a DNL file, which libjpeg does not decode). */
gboolean intact_jpeg(GFile *p_file, IntactSize *p_size, GError **p_err);

/* TRUE iff libjpeg decodes all of the local file p_file without giving up
 * (warnings allowed), read at 1/8 scale into a single row -- run it after
 * intact_jpeg() and the size caps, never on a file they refused. FALSE
 * with G_IO_ERROR_INVALID_DATA carrying libjpeg's message, FAILED when the
 * file cannot be opened (or is not local), NOT_SUPPORTED in a build
 * without the `jpeg` feature (no libjpeg to decode with: the caller must
 * then keep the file away from gegl:jpg-load). */
gboolean intact_jpeg_decodes(GFile *p_file, GError **p_err);

G_END_DECLS

#endif /* GGAZE_INTACT_H */
