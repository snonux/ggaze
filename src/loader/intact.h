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
 *     error_exit() calls exit(1). A JPEG cut short is one of those: at
 *     EOF the loader's reader starts the file over, and libjpeg meets
 *     "two SOI markers" -- also for a progressive file cut between scans
 *     right after a segment whose bytes happen to read FF D9.
 *
 * So the enhancer hands a file to GEGL only when this module vouches for
 * it, and takes ggaze's own loader (which reports "could not decode"
 * exactly as before xb2) otherwise. A PNG is walked chunk by chunk -- the
 * critical chunks' CRCs checked, the IDAT stream inflated (into a scratch
 * buffer, never kept) to the size IHDR promises, every row's filter byte
 * checked, IHDR's size held against the loader's caps before any of it --
 * which is the whole of what libpng can fail on short of a broken IHDR /
 * PLTE, which the header checks and the decoded-extent check in the
 * enhancer cover. A JPEG's header segments are walked marker by marker to
 * SOS (intact_jpeg), and then the file is decoded once, at 1/8 scale, by
 * the same libjpeg under a longjmp handler and with a source that makes
 * EOF fatal as GEGL's does (intact_jpeg_decodes): whatever would make
 * libjpeg give up inside GEGL makes it give up here first, harmlessly.
 * Both walks report the stored size from IHDR / the first SOF, so the
 * caller needs no separate header peek (and opens the file once less).
 * The walks that read a whole file (the PNG inflate, the libjpeg pass)
 * take the caller's GCancellable and stop between blocks when it fires
 * (G_IO_ERROR_CANCELLED).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

/* The stored (not EXIF-oriented) size the walk found in the header: a
 * PNG's IHDR, a JPEG's first SOFn. 0 x 0 when the walk failed before it.
 * u_comps is the number of COLOUR components the image stores, alpha not
 * counted: a PNG's 1 (grey, grey + alpha) or 3 (RGB, RGBA, palette), a
 * JPEG's SOF component count (1 grey, 3 YCbCr / RGB, 4 CMYK / YCCK); 0
 * with the size. The enhancer holds an embedded profile's colour space
 * against it: a profile for another number of components describes some
 * other image, and applying it would shift every colour. */
typedef struct {
   guint32 u_w;
   guint32 u_h;
   guint   u_comps;
} IntactSize;

/* TRUE iff p_file is a PNG whose chunks all lie within the file and end
 * with IEND, whose critical chunks' CRCs match, whose IHDR size is within
 * the loader's caps (detect_dims_within_bounds, checked before any image
 * data is inflated), and whose IDAT data -- one run of consecutive IDAT
 * chunks, as libpng reads it -- inflates to at least the image data IHDR
 * declares (Adam7 included) with a valid filter type on every row. FALSE
 * with G_IO_ERROR_INVALID_DATA ("truncated" for a file that ends early,
 * "corrupt" otherwise), the caps' error for an oversized IHDR,
 * G_IO_ERROR_CANCELLED when p_cancel (nullable) fires, or the GIO error of
 * a read failure. p_size (nullable) receives IHDR's size either way. */
gboolean intact_png(GFile *p_file, GCancellable *p_cancel, IntactSize *p_size,
                    GError **p_err);

/* The cheap half of intact_png(): TRUE iff p_file starts with the PNG
 * signature and a sound IHDR (a valid type / depth / method, a size
 * within the caps), with its size and component count in p_size
 * (nullable). Nothing after IHDR is read, so it vouches for NOTHING a
 * decoder needs; it is for a caller that only asks what the image
 * declares (the info card's "will the enhancer manage this" question,
 * enhancer_would_manage). FALSE with G_IO_ERROR_INVALID_DATA or the caps'
 * or a read's error otherwise. */
gboolean intact_png_header(GFile *p_file, IntactSize *p_size, GError **p_err);

/* TRUE iff p_file is a JPEG whose marker segments up to the first SOS all
 * lie within the file: the header walk that finds the stored size before
 * the caps are applied. It says nothing about the data after SOS -- that
 * is intact_jpeg_decodes()'s verdict (a byte scan for an EOI there cannot
 * tell it from FF D9 inside a segment between progressive scans). EOI
 * before any SOS (no image), or EOF, is "truncated"; padding between
 * segments is skipped as libjpeg does (streamread_jpeg_marker). FALSE with
 * G_IO_ERROR_INVALID_DATA otherwise, or with the GIO error of a read
 * failure (CANCELLED when p_cancel, nullable, fires before the open).
 * p_size (nullable) receives the first SOF's size either way (0 x 0 when
 * none precedes SOS; a SOF may declare 0 rows, a DNL file, which libjpeg
 * does not decode). */
gboolean intact_jpeg(GFile *p_file, GCancellable *p_cancel, IntactSize *p_size,
                     GError **p_err);

/* TRUE iff libjpeg decodes all of the local file p_file, through its EOI,
 * without giving up (warnings allowed) and without reading past the end
 * of the file -- EOF is fatal here, as it is inside gegl:jpg-load, where
 * libjpeg's leniency (a fake EOI) would have vouched for a cut file. Read
 * at 1/8 scale into a single row; run it after intact_jpeg() and the size
 * caps, never on a file they refused. FALSE with G_IO_ERROR_INVALID_DATA
 * carrying libjpeg's message, CANCELLED when p_cancel (nullable) fires
 * between blocks, FAILED when the file cannot be opened (or is not local),
 * NOT_SUPPORTED in a build without the `jpeg` feature (no libjpeg to
 * decode with: the caller must then keep the file away from
 * gegl:jpg-load). */
gboolean intact_jpeg_decodes(GFile *p_file, GCancellable *p_cancel,
                             GError **p_err);

G_END_DECLS

#endif /* GGAZE_INTACT_H */
