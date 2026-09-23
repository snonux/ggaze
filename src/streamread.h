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
 * copy.
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

G_END_DECLS

#endif /* GGAZE_STREAMREAD_H */
