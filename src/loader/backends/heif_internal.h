/*:*
 * ggaze — HEIF backend internals exposed for direct unit testing (task 7u0)
 *
 * _heif_check_plane() and _heif_check_plane_data() validate decoder-reported
 * geometry and pointers before the memcpy in _heif_build_texture(). Both are
 * pure functions of primitives (ints/pointers in, GError/gboolean out) with
 * no libheif object dependency, so they can be exercised directly from a
 * unit test without decoding a real HEIF file or mocking libheif — every
 * malformed-input branch (zero/negative dims, zero/negative stride, stride
 * narrower than one row, NULL plane pointer) gets real, non-tautological
 * coverage this way. They stay out of loader.h: this header is not part of
 * the public loader API, only a test seam for tests/test_loader_heif.c.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#ifndef GGAZE_HEIF_INTERNAL_H
#define GGAZE_HEIF_INTERNAL_H

#include <glib.h>
#include <stdint.h>

gboolean _heif_check_plane(int i_w, int i_h, int i_stride, gsize *p_len,
                           GError **p_err);

gboolean _heif_check_plane_data(const uint8_t *p_data, GError **p_err);

#endif /* GGAZE_HEIF_INTERNAL_H */
