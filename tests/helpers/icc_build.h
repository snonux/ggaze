#ifndef GGAZE_ICC_BUILD_H
#define GGAZE_ICC_BUILD_H

/*:*
 * ggaze tests — hand-built ICC profiles
 *
 * The C twin of tests/fixtures/gen.py's _icc_from_tags(): an ICC v2.1
 * profile from (signature, data) tags, so a test can build exactly the
 * profile a case needs -- a 'para' curve of any function type and
 * parameters, 'curv' tables of any length, tags a real profile would not
 * carry -- instead of patching a fixture in place. Tags given the SAME
 * data (the same GBytes pointer) share one offset, as rTRC / gTRC / bTRC
 * of real profiles often do. Plain GLib, no babl: the suites decide what
 * to do with the bytes.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <glib.h>

G_BEGIN_DECLS

/* One tag of a profile to build: a 4-character signature and its data
 * (type signature first). */
typedef struct {
   const char *c_sig;
   GBytes     *p_data;
} IccBuildTag;

/* A profile of class c_class ("mntr", "scnr", "prtr", ...), colour space
 * c_space ("RGB ", "GRAY", "CMYK") and PCS c_pcs ("XYZ ", "Lab ") holding
 * the u_n tags p_tags, each at a 4-byte-aligned offset after the table.
 * The size field is the byte count. Caller unrefs. */
GBytes *icc_build(const char *c_class, const char *c_space, const char *c_pcs,
                  const IccBuildTag *p_tags, gsize u_n);

/* Tag data (caller unrefs each). An XYZType of one colour. */
GBytes *icc_build_xyz(double f_x, double f_y, double f_z);

/* A curveType of u_n points sampling x^f_gamma (u_n 0 is the identity,
 * 1 a plain u8Fixed8 gamma). */
GBytes *icc_build_curv(guint32 u_n, double f_gamma);

/* A parametricCurveType of function type u_fn with the u_n parameters
 * p_params (s15Fixed16, in the order ICC.1 lists them: g, a, b, c, d, e,
 * f). u_n is written as given, so a short or long tag is buildable. */
GBytes *icc_build_para(guint16 u_fn, const double *p_params, gsize u_n);

/* A textDescriptionType naming the profile (so profiles otherwise alike
 * differ in their bytes). */
GBytes *icc_build_desc(const char *c_text);

/* An RGB display profile with the given curves (shared when the same
 * pointer is passed twice), the sRGB primaries with red and blue swapped
 * (swapped.png's: a matrix babl knows no built-in space for) and the D50
 * white point, described as c_desc. */
GBytes *icc_build_rgb(const char *c_desc, GBytes *p_r, GBytes *p_g,
                      GBytes *p_b);

/* icc_build_rgb() with the u_extra tags p_extra appended to its own (a
 * kTRC on an RGB profile, say). */
GBytes *icc_build_rgb_with(const char *c_desc, GBytes *p_r, GBytes *p_g,
                           GBytes *p_b, const IccBuildTag *p_extra,
                           gsize u_extra);

/* A grey display profile with the curve p_k, described as c_desc. */
GBytes *icc_build_gray(const char *c_desc, GBytes *p_k);

/* icc_build_gray() with the u_extra tags p_extra appended. */
GBytes *icc_build_gray_with(const char *c_desc, GBytes *p_k,
                            const IccBuildTag *p_extra, gsize u_extra);

G_END_DECLS

#endif /* GGAZE_ICC_BUILD_H */
