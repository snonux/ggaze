/*:*
 * ggaze tests — hand-built ICC profiles
 *
 * See icc_build.h. Big-endian writers over a GByteArray, the header laid
 * out as gen.py lays it out (version 2.1, D50 illuminant, 'acsp').
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "icc_build.h"

#include <glib.h>
#include <math.h>
#include <string.h>

/* sRGB primaries adapted to D50 (gen.py's SRGB_R / SRGB_G / SRGB_B) and
 * the D50 white. */
static const double SRGB_R[3] = {0.4361, 0.2225, 0.0139};
static const double SRGB_G[3] = {0.3851, 0.7169, 0.0971};
static const double SRGB_B[3] = {0.1431, 0.0606, 0.7141};
static const double D50[3]    = {0.9642, 1.0, 0.8249};

static void
_put32(guint8 *p, guint32 u) {
   p[0] = (guint8)(u >> 24);
   p[1] = (guint8)(u >> 16);
   p[2] = (guint8)(u >> 8);
   p[3] = (guint8)u;
}

static void
_append32(GByteArray *p_a, guint32 u) {
   guint8 c[4];
   _put32(c, u);
   g_byte_array_append(p_a, c, 4);
}

/* u_n zero bytes (g_byte_array_set_size leaves new bytes undefined). */
static void
_append_zeros(GByteArray *p_a, guint u_n) {
   guint8 c_zero[64] = {0};
   while (u_n > 0) {
      guint u_step = MIN(u_n, (guint)sizeof(c_zero));
      g_byte_array_append(p_a, c_zero, u_step);
      u_n -= u_step;
   }
}

static void
_append_s15f16(GByteArray *p_a, double f_v) {
   _append32(p_a, (guint32)(gint32)lround(f_v * 65536.0));
}

/* The 128-byte header; the size field is patched once the body is known. */
static void
_append_header(GByteArray *p_a, const char *c_class, const char *c_space,
               const char *c_pcs) {
   guint8 c_hdr[128] = {0};
   _put32(c_hdr + 8, 0x02100000u); /* version 2.1 */
   memcpy(c_hdr + 12, c_class, 4);
   memcpy(c_hdr + 16, c_space, 4);
   memcpy(c_hdr + 20, c_pcs, 4);
   memcpy(c_hdr + 36, "acsp", 4);
   _put32(c_hdr + 68, (guint32)lround(D50[0] * 65536.0));
   _put32(c_hdr + 72, (guint32)lround(D50[1] * 65536.0));
   _put32(c_hdr + 76, (guint32)lround(D50[2] * 65536.0));
   g_byte_array_append(p_a, c_hdr, sizeof(c_hdr));
}

/* The offset an earlier tag with the same data got, or 0. */
static guint32
_shared_offset(const IccBuildTag *p_tags, gsize u_i, const guint32 *p_offs) {
   for (gsize u = 0; u < u_i; u++) {
      if (p_tags[u].p_data == p_tags[u_i].p_data) {
         return (p_offs[u]);
      }
   }
   return (0);
}

GBytes *
icc_build(const char *c_class, const char *c_space, const char *c_pcs,
          const IccBuildTag *p_tags, gsize u_n) {
   GByteArray *p_a = g_byte_array_new();
   _append_header(p_a, c_class, c_space, c_pcs);
   _append32(p_a, (guint32)u_n);
   _append_zeros(p_a, 12 * (guint)u_n); /* the table, filled below */
   guint32 *p_offs = g_new0(guint32, u_n);
   for (gsize u = 0; u < u_n; u++) {
      gsize         u_len  = 0;
      const guint8 *p_data = g_bytes_get_data(p_tags[u].p_data, &u_len);
      guint32       u_off  = _shared_offset(p_tags, u, p_offs);
      if (u_off == 0) {
         u_off = p_a->len;
         g_byte_array_append(p_a, p_data, (guint)u_len);
         _append_zeros(p_a, (4 - p_a->len % 4) % 4);
      }
      p_offs[u]     = u_off;
      guint8 *p_ent = p_a->data + 132 + 12 * u;
      memcpy(p_ent, p_tags[u].c_sig, 4);
      _put32(p_ent + 4, u_off);
      _put32(p_ent + 8, (guint32)u_len);
   }
   g_free(p_offs);
   _put32(p_a->data, p_a->len);
   return (g_byte_array_free_to_bytes(p_a));
}

GBytes *
icc_build_xyz(double f_x, double f_y, double f_z) {
   GByteArray *p_a = g_byte_array_new();
   g_byte_array_append(p_a, (const guint8 *)"XYZ \0\0\0\0", 8);
   _append_s15f16(p_a, f_x);
   _append_s15f16(p_a, f_y);
   _append_s15f16(p_a, f_z);
   return (g_byte_array_free_to_bytes(p_a));
}

GBytes *
icc_build_curv(guint32 u_n, double f_gamma) {
   GByteArray *p_a = g_byte_array_new();
   g_byte_array_append(p_a, (const guint8 *)"curv\0\0\0\0", 8);
   _append32(p_a, u_n);
   if (u_n == 1) {
      guint16 u_g   = (guint16)lround(f_gamma * 256.0);
      guint8  c2[2] = {(guint8)(u_g >> 8), (guint8)u_g};
      g_byte_array_append(p_a, c2, 2);
   }
   for (guint32 u = 0; u_n > 1 && u < u_n; u++) {
      double  f_y   = pow((double)u / (double)(u_n - 1), f_gamma);
      guint16 u_y   = (guint16)lround(f_y * 65535.0);
      guint8  c2[2] = {(guint8)(u_y >> 8), (guint8)u_y};
      g_byte_array_append(p_a, c2, 2);
   }
   return (g_byte_array_free_to_bytes(p_a));
}

GBytes *
icc_build_para(guint16 u_fn, const double *p_params, gsize u_n) {
   GByteArray *p_a    = g_byte_array_new();
   guint8      c_fn[] = {(guint8)(u_fn >> 8), (guint8)u_fn, 0, 0};
   g_byte_array_append(p_a, (const guint8 *)"para\0\0\0\0", 8);
   g_byte_array_append(p_a, c_fn, 4);
   for (gsize u = 0; u < u_n; u++) {
      _append_s15f16(p_a, p_params[u]);
   }
   return (g_byte_array_free_to_bytes(p_a));
}

GBytes *
icc_build_desc(const char *c_text) {
   GByteArray *p_a = g_byte_array_new();
   guint32     u_n = (guint32)strlen(c_text) + 1;
   g_byte_array_append(p_a, (const guint8 *)"desc\0\0\0\0", 8);
   _append32(p_a, u_n);
   g_byte_array_append(p_a, (const guint8 *)c_text, u_n);
   _append_zeros(p_a, 8 + 3 + 67); /* empty Unicode and ScriptCode */
   return (g_byte_array_free_to_bytes(p_a));
}

/* icc_build() of p_own followed by p_extra. */
static GBytes *
_build_joined(const char *c_space, const IccBuildTag *p_own, gsize u_own,
              const IccBuildTag *p_extra, gsize u_extra) {
   IccBuildTag *p_all = g_new(IccBuildTag, u_own + u_extra);
   memcpy(p_all, p_own, u_own * sizeof(IccBuildTag));
   if (u_extra > 0) {
      memcpy(p_all + u_own, p_extra, u_extra * sizeof(IccBuildTag));
   }
   GBytes *p_icc = icc_build("mntr", c_space, "XYZ ", p_all, u_own + u_extra);
   g_free(p_all);
   return (p_icc);
}

GBytes *
icc_build_rgb_with(const char *c_desc, GBytes *p_r, GBytes *p_g, GBytes *p_b,
                   const IccBuildTag *p_extra, gsize u_extra) {
   GBytes     *p_desc   = icc_build_desc(c_desc);
   GBytes     *p_wtpt   = icc_build_xyz(D50[0], D50[1], D50[2]);
   GBytes     *p_rxyz   = icc_build_xyz(SRGB_B[0], SRGB_B[1], SRGB_B[2]);
   GBytes     *p_gxyz   = icc_build_xyz(SRGB_G[0], SRGB_G[1], SRGB_G[2]);
   GBytes     *p_bxyz   = icc_build_xyz(SRGB_R[0], SRGB_R[1], SRGB_R[2]);
   IccBuildTag t_tags[] = {
      {"desc", p_desc}, {"wtpt", p_wtpt}, {"rXYZ", p_rxyz}, {"gXYZ", p_gxyz},
      {"bXYZ", p_bxyz}, {"rTRC", p_r},    {"gTRC", p_g},    {"bTRC", p_b},
   };
   GBytes *p_icc =
      _build_joined("RGB ", t_tags, G_N_ELEMENTS(t_tags), p_extra, u_extra);
   g_bytes_unref(p_desc);
   g_bytes_unref(p_wtpt);
   g_bytes_unref(p_rxyz);
   g_bytes_unref(p_gxyz);
   g_bytes_unref(p_bxyz);
   return (p_icc);
}

GBytes *
icc_build_rgb(const char *c_desc, GBytes *p_r, GBytes *p_g, GBytes *p_b) {
   return (icc_build_rgb_with(c_desc, p_r, p_g, p_b, NULL, 0));
}

GBytes *
icc_build_gray_with(const char *c_desc, GBytes *p_k, const IccBuildTag *p_extra,
                    gsize u_extra) {
   GBytes     *p_desc   = icc_build_desc(c_desc);
   GBytes     *p_wtpt   = icc_build_xyz(D50[0], D50[1], D50[2]);
   IccBuildTag t_tags[] = {{"desc", p_desc}, {"wtpt", p_wtpt}, {"kTRC", p_k}};
   GBytes     *p_icc =
      _build_joined("GRAY", t_tags, G_N_ELEMENTS(t_tags), p_extra, u_extra);
   g_bytes_unref(p_desc);
   g_bytes_unref(p_wtpt);
   return (p_icc);
}

GBytes *
icc_build_gray(const char *c_desc, GBytes *p_k) {
   return (icc_build_gray_with(c_desc, p_k, NULL, 0));
}
