/*:*
 * ggaze — a GFile served from memory, for driving the loader's stream path
 * without a disk file (AGENTS.md: "Shared helpers go in tests/helpers/")
 *
 * The loader reads through g_file_read() / g_file_load_contents() only, so
 * a GFile whose read_fn hands out a GFileInputStream over a byte buffer is
 * enough to feed it -- and, unlike a temp file, it lets a test act at an
 * exact point INSIDE a read: the stream can cancel a GCancellable the
 * moment it reports EOF, i.e. after the read's own pre-read cancel check
 * has passed and before the caller gets its bytes. That is the only
 * deterministic way to reach a backend's "cancelled after the read, before
 * the decode" branch (src/loader/backends/pixbuf.c), which a cancel from
 * another thread hits only by luck. Nothing else of GFile is implemented:
 * get_path() is NULL (non-native), query/enumerate/write are unsupported.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#ifndef GGAZE_TEST_MEM_FILE_H
#define GGAZE_TEST_MEM_FILE_H

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

/* A GFile serving a copy of p_bytes/u_len. Caller unrefs. */
GFile *ggtest_mem_file_new(const guint8 *p_bytes, gsize u_len);

/* Arm the file so that the u_nth stream opened on it (1-based) cancels
 * p_cancel right before it reports EOF. p_cancel is referenced until the
 * file is finalized. u_nth = 0 disarms. */
void ggtest_mem_file_cancel_at_eof(GFile *p_file, GCancellable *p_cancel,
                                   guint u_nth);

/* How many streams have been opened on p_file so far. */
guint ggtest_mem_file_opens(GFile *p_file);

G_END_DECLS

#endif /* GGAZE_TEST_MEM_FILE_H */
