#ifndef GGAZE_STREAMREAD_H
#define GGAZE_STREAMREAD_H

/*:*
 * ggaze — bounded reads on a GInputStream (plain C)
 *
 * The two primitives a header walker needs -- "exactly this many bytes" and
 * "skip this many bytes" -- with EOF told apart from an I/O error, because
 * the walkers treat them differently: EOF before the structure they look
 * for simply means "not there" (or "truncated"), while an I/O error is
 * reported as such. Shared by icc.c (the embedded-profile walk) and
 * loader/intact.c (the completeness check); each used to carry its own
 * copy -- of these and of the JPEG marker step below, which is here for
 * the same reason: two walkers that must agree with libjpeg on where the
 * next marker is -- and of the CRC-32 PNG chunks carry, which both check
 * (intact.c the critical chunks', icc.c the iCCP's).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef enum {
   STREAMREAD_OK,   /* every requested byte was read / skipped */
   STREAMREAD_EOF,  /* the stream ended first (p_err untouched) */
   STREAMREAD_ERROR /* an I/O failure, in p_err */
} StreamReadStatus;

/* Read exactly u_len bytes into p_buf (a read_all: a FIFO / GVFS short read
 * is not an EOF). */
StreamReadStatus streamread_exact(GInputStream *p_in, guint8 *p_buf,
                                  gsize u_len, GError **p_err);

/* Skip exactly u_len bytes. */
StreamReadStatus streamread_skip(GInputStream *p_in, gsize u_len,
                                 GError **p_err);

/* Advance to the next JPEG marker and store its code in *p_code, the way
 * libjpeg's next_marker() does: bytes that are not 0xFF ahead of the
 * marker (padding some writers leave between segments; libjpeg warns
 * "extraneous bytes before marker" and decodes on) are skipped, 0xFF fill
 * bytes are skipped, and FF 00 (a stuffed zero, not a marker) is skipped
 * too. So a walker does not refuse a file for padding libjpeg reads past.
 * The padding (stray and fill bytes together) is capped at
 * STREAMREAD_JPEG_MAX_PAD, far above the few bytes a writer leaves: more
 * than that is not a JPEG marker stream (INVALID_DATA), and without the
 * cap a file of junk after its SOI would be read to its end one byte at a
 * time. EOF before a marker is STREAMREAD_EOF. Read through a buffered
 * stream: this reads a byte at a time. */
#define STREAMREAD_JPEG_MAX_PAD 65536u

StreamReadStatus streamread_jpeg_marker(GInputStream *p_in, guint8 *p_code,
                                        GError **p_err);

/* The CRC-32 (ISO 3309) a PNG chunk carries over its type and data, as a
 * running value: start with STREAMREAD_CRC32_INIT, feed every block, and
 * compare the result XOR STREAMREAD_CRC32_INIT with the stored CRC. GLib
 * has none and ggaze links no zlib of its own (GIO's zlib converter does
 * the inflating), hence the 256-entry table, built once, thread-safe. */
#define STREAMREAD_CRC32_INIT 0xFFFFFFFFu

guint32 streamread_crc32(guint32 u_crc, const guint8 *p, gsize u_len);

G_END_DECLS

#endif /* GGAZE_STREAMREAD_H */
