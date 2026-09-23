/*:*
 * ggaze — read and set a file's stamp in the test suites (AGENTS.md:
 * "Shared helpers go in tests/helpers/")
 *
 * The texture cache stamps an entry with the file's mtime (whole seconds
 * plus the sub-second part in nanoseconds), byte count and inode
 * (src/texturecache.c). test_texturecache and test_enhance_flow both read
 * that stamp and set the mtime back to a chosen value to drive the cache
 * into a hit or a miss deterministically; this is the one copy of that
 * code.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#ifndef GGAZE_TEST_FILE_STAMP_H
#define GGAZE_TEST_FILE_STAMP_H

#include <glib.h>

/* c_path's stamp as the texture cache reads it. */
typedef struct {
   guint64 u_sec;   /* mtime, whole seconds */
   guint32 u_nsec;  /* mtime, sub-second part in nanoseconds */
   goffset i_size;  /* byte count */
   guint64 u_inode; /* 0 where the backend has none */
} GgtestFileStamp;

/* Read c_path's stamp in one query; asserts the query succeeds. */
void ggtest_read_stamp(const char *c_path, GgtestFileStamp *p_st);

/* Set c_path's mtime to u_sec + u_nsec, both parts in one call (setting the
 * seconds alone zeroes the sub-second part), and check it took. A
 * filesystem that keeps whole seconds only drops u_nsec: that fails the
 * test with "filesystem lacks sub-second mtimes" instead of letting a
 * sub-second subtest pass vacuously. */
void ggtest_set_mtime(const char *c_path, guint64 u_sec, guint32 u_nsec);

/* Fail the test with "filesystem lacks sub-second mtimes" unless p_st (read
 * from c_path) has a non-zero sub-second part -- the premise of any subtest
 * that moves the mtime within its second. A real sub-second clock reads
 * .000000000 once in a billion writes. */
void ggtest_require_subsecond_mtime(const char            *c_path,
                                    const GgtestFileStamp *p_st);

#endif /* GGAZE_TEST_FILE_STAMP_H */
