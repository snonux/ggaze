/*:*
 * ggaze — embedded ICC profile extraction
 *
 * See icc.h. Two container walkers (PNG chunks, JPEG marker segments) over
 * one bounded stream reader, plus the 'desc' tag parser. Everything is
 * bounds-checked against the bytes actually read: a file's own length
 * fields are hints that decide what to read next, never trusted offsets.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "icc.h"

#include <gio/gio.h>
#include <glib.h>
#include <math.h>
#include <string.h>

#include "streamread.h"

/* --- bounded stream reads -------------------------------------------------
 *
 * streamread.c's three outcomes, because the walkers treat them
 * differently: EOF before the pixel data simply means "no profile found"
 * (a truncated file is the decoder's problem, not this module's), while an
 * I/O error is reported. Every function below takes a non-NULL p_err (the
 * public entry points substitute a local one), so a walker can test *p_err
 * directly. */

/* A declared payload of u_len bytes as a new GBytes. The cap is checked
 * first so a lying length allocates nothing; a payload the file cannot
 * deliver (EOF inside it) is INVALID_DATA, not "no profile", because the
 * container claimed it was there. */
static GBytes *
_read_payload(GInputStream *p_in, gsize u_len, GError **p_err) {
   if (u_len > ICC_MAX_PROFILE_LEN) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "icc: embedded profile declares %" G_GSIZE_FORMAT
                  " bytes, over the %u byte cap",
                  u_len, ICC_MAX_PROFILE_LEN);
      return (NULL);
   }
   guint8 *p_buf = g_try_malloc(u_len > 0 ? u_len : 1);
   if (p_buf == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "icc: cannot allocate %" G_GSIZE_FORMAT " bytes", u_len);
      return (NULL);
   }
   StreamReadStatus e_rd = streamread_exact(p_in, p_buf, u_len, p_err);
   if (e_rd != STREAMREAD_OK) {
      g_free(p_buf);
      if (e_rd == STREAMREAD_EOF) {
         g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "icc: embedded profile segment is truncated");
      }
      return (NULL);
   }
   return (g_bytes_new_take(p_buf, u_len));
}

static guint32
_be32(const guint8 *p) {
   return (((guint32)p[0] << 24) | ((guint32)p[1] << 16) |
           ((guint32)p[2] << 8) | (guint32)p[3]);
}

/* --- PNG: the iCCP chunk --------------------------------------------------
 *
 * "<name>\0<method>\0<zlib stream>", where the name is 1-79 bytes and the
 * only defined compression method is 0 (deflate). The stream is inflated
 * through GIO's zlib decompressor in one pass over the in-memory payload,
 * so no zlib dependency of ggaze's own is needed. */

/* Inflate p_z (u_zlen bytes of zlib data) into a new GBytes, bounded by the
 * profile cap. Any converter failure -- short input, a corrupt stream --
 * is INVALID_DATA: the chunk exists but does not inflate. */
static GBytes *
_inflate(const guint8 *p_z, gsize u_zlen, GError **p_err) {
   GZlibDecompressor *p_dec =
      g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB);
   GByteArray      *p_out = g_byte_array_new();
   guint8           c_buf[8192];
   gsize            u_in   = 0;
   GConverterResult e_res  = G_CONVERTER_CONVERTED;
   GError          *p_conv = NULL;
   while (e_res != G_CONVERTER_FINISHED && p_conv == NULL) {
      gsize u_read = 0, u_written = 0;
      e_res = g_converter_convert(
         G_CONVERTER(p_dec), p_z + u_in, u_zlen - u_in, c_buf, sizeof(c_buf),
         G_CONVERTER_INPUT_AT_END, &u_read, &u_written, &p_conv);
      u_in += u_read;
      g_byte_array_append(p_out, c_buf, (guint)u_written);
      if (p_conv == NULL && p_out->len > ICC_MAX_PROFILE_LEN) {
         g_set_error(&p_conv, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "inflates past the %u byte cap", ICC_MAX_PROFILE_LEN);
      }
   }
   g_object_unref(p_dec);
   if (p_conv != NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "icc: PNG iCCP chunk does not inflate: %s", p_conv->message);
      g_error_free(p_conv);
      g_byte_array_unref(p_out);
      return (NULL);
   }
   return (g_byte_array_free_to_bytes(p_out));
}

static GBytes *
_png_inflate_iccp(GBytes *p_chunk, GError **p_err) {
   gsize         u_len;
   const guint8 *p     = g_bytes_get_data(p_chunk, &u_len);
   const guint8 *p_nul = u_len > 0 ? memchr(p, 0, MIN(u_len, 80)) : NULL;
   if (p_nul == NULL || p_nul == p || (gsize)(p_nul - p) + 2 > u_len ||
       p_nul[1] != 0) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "icc: malformed PNG iCCP chunk");
      return (NULL);
   }
   gsize u_off = (gsize)(p_nul - p) + 2;
   return (_inflate(p + u_off, u_len - u_off, p_err));
}

/* Walk the chunks after the 8-byte signature (already consumed). The iCCP
 * chunk must precede IDAT, so the walk stops there: EOF or IDAT/IEND first
 * means no profile. */
static GBytes *
_png_walk(GInputStream *p_in, GError **p_err) {
   for (;;) {
      guint8 c_hdr[8];
      if (streamread_exact(p_in, c_hdr, sizeof(c_hdr), p_err) !=
          STREAMREAD_OK) {
         return (NULL);
      }
      gsize u_len = _be32(c_hdr);
      if (memcmp(c_hdr + 4, "IDAT", 4) == 0 ||
          memcmp(c_hdr + 4, "IEND", 4) == 0) {
         return (NULL);
      }
      if (memcmp(c_hdr + 4, "iCCP", 4) == 0) {
         GBytes *p_chunk = _read_payload(p_in, u_len, p_err);
         if (p_chunk == NULL) {
            return (NULL);
         }
         GBytes *p_icc = _png_inflate_iccp(p_chunk, p_err);
         g_bytes_unref(p_chunk);
         return (p_icc);
      }
      if (streamread_skip(p_in, u_len + 4, p_err) !=
          STREAMREAD_OK) { /* data + CRC */
         return (NULL);
      }
   }
}

/* --- JPEG: the APP2 ICC_PROFILE segments ----------------------------------
 *
 * A profile larger than one 64 KiB segment is split over several APP2
 * segments, each "ICC_PROFILE\0" + sequence number (1-based) + count +
 * data, in any order. They are collected by sequence number and joined once
 * the marker walk reaches SOS (the entropy-coded data, where no APPn can
 * follow). */

typedef struct {
   GPtrArray *p_parts; /* GBytes* per sequence slot, NULL until seen */
   guint      u_count; /* declared segment count, 0 until the first */
} IccParts;

static void
_bytes_free(gpointer p_data) {
   if (p_data != NULL) {
      g_bytes_unref(p_data);
   }
}

/* Slot p_seg (a whole APP2 payload starting with the tag) by its sequence
 * number. The count must agree across segments, the sequence must be
 * within it and unseen: anything else is a broken container. */
static gboolean
_jpeg_add_part(IccParts *p_parts, GBytes *p_seg, GError **p_err) {
   gsize         u_len;
   const guint8 *p       = g_bytes_get_data(p_seg, &u_len);
   guint         u_seq   = p[12];
   guint         u_count = p[13];
   if (u_seq == 0 || u_count == 0 || u_seq > u_count ||
       (p_parts->u_count != 0 && p_parts->u_count != u_count)) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "icc: inconsistent JPEG ICC segment numbering (%u of %u)",
                  u_seq, u_count);
      return (FALSE);
   }
   if (p_parts->u_count == 0) {
      p_parts->u_count = u_count;
      g_ptr_array_set_size(p_parts->p_parts, (gint)u_count);
   }
   if (g_ptr_array_index(p_parts->p_parts, u_seq - 1) != NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "icc: duplicate JPEG ICC segment %u", u_seq);
      return (FALSE);
   }
   g_ptr_array_index(p_parts->p_parts, u_seq - 1) =
      g_bytes_new_from_bytes(p_seg, 14, u_len - 14);
   return (TRUE);
}

/* Concatenate the slots in order; NULL without an error when no ICC segment
 * was seen at all, INVALID_DATA for a gap in the sequence. No size cap is
 * checked here because none can be exceeded: a segment carries at most
 * 65533 - 14 bytes of profile and the count is one byte, so the join is at
 * most 255 * 65519 bytes (~15.9 MiB), under ICC_MAX_PROFILE_LEN. */
G_STATIC_ASSERT(255u * (65533u - 14u) <= ICC_MAX_PROFILE_LEN);
static GBytes *
_jpeg_join_parts(IccParts *p_parts, GError **p_err) {
   if (p_parts->u_count == 0) {
      return (NULL);
   }
   GByteArray *p_out = g_byte_array_new();
   for (guint u = 0; u < p_parts->u_count; u++) {
      GBytes *p_b = g_ptr_array_index(p_parts->p_parts, u);
      if (p_b == NULL) {
         g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "icc: JPEG ICC segment %u of %u is missing", u + 1,
                     p_parts->u_count);
         g_byte_array_unref(p_out);
         return (NULL);
      }
      gsize         u_n = 0;
      const guint8 *p_d = g_bytes_get_data(p_b, &u_n);
      g_byte_array_append(p_out, p_d, (guint)u_n);
   }
   return (g_byte_array_free_to_bytes(p_out));
}

/* Markers that carry no length field: TEM, RSTn, SOI. */
static gboolean
_jpeg_is_standalone(guint8 u_code) {
   return (u_code == 0x01 || (u_code >= 0xD0 && u_code <= 0xD8));
}

/* A segment's payload length (its 2-byte length field minus itself). */
static StreamReadStatus
_jpeg_read_length(GInputStream *p_in, gsize *pu_payload, GError **p_err) {
   guint8           c_len[2];
   StreamReadStatus e_rd = streamread_exact(p_in, c_len, 2, p_err);
   if (e_rd != STREAMREAD_OK) {
      return (e_rd);
   }
   gsize u_len = ((gsize)c_len[0] << 8) | c_len[1];
   if (u_len < 2) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "icc: JPEG segment length %" G_GSIZE_FORMAT " is invalid",
                  u_len);
      return (STREAMREAD_ERROR);
   }
   *pu_payload = u_len - 2;
   return (STREAMREAD_OK);
}

/* Read an APP2 payload and slot it when it is an ICC_PROFILE segment; any
 * other APP2 (FlashPix, ...) is read and dropped. FALSE with p_err set. */
static gboolean
_jpeg_take_app2(GInputStream *p_in, gsize u_payload, IccParts *p_parts,
                GError **p_err) {
   GBytes *p_seg = _read_payload(p_in, u_payload, p_err);
   if (p_seg == NULL) {
      return (FALSE);
   }
   gboolean      b_ok = TRUE;
   const guint8 *p    = g_bytes_get_data(p_seg, NULL);
   if (u_payload >= 14 && memcmp(p, "ICC_PROFILE\0", 12) == 0) {
      b_ok = _jpeg_add_part(p_parts, p_seg, p_err);
   }
   g_bytes_unref(p_seg);
   return (b_ok);
}

/* Walk the marker segments after SOI (already consumed) up to SOS / EOI. */
static GBytes *
_jpeg_walk(GInputStream *p_in, GError **p_err) {
   IccParts t_parts = {g_ptr_array_new_with_free_func(_bytes_free), 0};
   for (;;) {
      guint8 u_code = 0;
      if (streamread_jpeg_marker(p_in, &u_code, p_err) != STREAMREAD_OK ||
          u_code == 0xDA || u_code == 0xD9) {
         break;
      }
      if (_jpeg_is_standalone(u_code)) {
         continue;
      }
      gsize u_payload = 0;
      if (_jpeg_read_length(p_in, &u_payload, p_err) != STREAMREAD_OK) {
         break;
      }
      if (u_code != 0xE2) {
         if (streamread_skip(p_in, u_payload, p_err) != STREAMREAD_OK) {
            break;
         }
         continue;
      }
      if (!_jpeg_take_app2(p_in, u_payload, &t_parts, p_err)) {
         break;
      }
   }
   GBytes *p_icc = *p_err == NULL ? _jpeg_join_parts(&t_parts, p_err) : NULL;
   g_ptr_array_unref(t_parts.p_parts);
   return (p_icc);
}

/* --- the walk, dispatched on the signature ------------------------------- */

/* Sniff the stream's first bytes and hand it to the matching walker. Two
 * bytes decide a JPEG (SOI), eight a PNG; anything else -- or a file too
 * short for either -- carries no profile this module can find. */
static GBytes *
_walk_stream(GInputStream *p_in, GError **p_err) {
   guint8 c_head[8];
   if (streamread_exact(p_in, c_head, 2, p_err) != STREAMREAD_OK) {
      return (NULL);
   }
   if (c_head[0] == 0xFF && c_head[1] == 0xD8) {
      return (_jpeg_walk(p_in, p_err));
   }
   if (streamread_exact(p_in, c_head + 2, 6, p_err) != STREAMREAD_OK) {
      return (NULL);
   }
   if (memcmp(c_head, "\x89PNG\r\n\x1a\n", 8) == 0) {
      return (_png_walk(p_in, p_err));
   }
   return (NULL);
}

GBytes *
icc_read_embedded(GFile *p_file, GError **p_err) {
   g_return_val_if_fail(G_IS_FILE(p_file), NULL);
   GError           *p_local = NULL;
   GFileInputStream *p_in    = g_file_read(p_file, NULL, &p_local);
   GBytes           *p_icc   = NULL;
   if (p_in != NULL) {
      /* Buffered: the marker search reads a byte at a time. */
      GInputStream *p_buf = g_buffered_input_stream_new(G_INPUT_STREAM(p_in));
      p_icc               = _walk_stream(p_buf, &p_local);
      g_input_stream_close(p_buf, NULL, NULL);
      g_object_unref(p_buf);
      g_object_unref(p_in);
   }
   if (p_local != NULL) {
      g_propagate_error(p_err, p_local);
   }
   return (p_icc);
}

gboolean
icc_container_searched(GFile *p_file) {
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   GFileInputStream *p_in = g_file_read(p_file, NULL, NULL);
   if (p_in == NULL) {
      return (FALSE);
   }
   /* The same signatures _walk_stream() dispatches on; a read failure or
    * a file shorter than a signature is simply "not searched". */
   guint8 c_head[8];
   gsize  u_got = 0;
   g_input_stream_read_all(G_INPUT_STREAM(p_in), c_head, sizeof(c_head), &u_got,
                           NULL, NULL);
   g_input_stream_close(G_INPUT_STREAM(p_in), NULL, NULL);
   g_object_unref(p_in);
   return ((u_got >= 2 && c_head[0] == 0xFF && c_head[1] == 0xD8) ||
           (u_got == 8 && memcmp(c_head, "\x89PNG\r\n\x1a\n", 8) == 0));
}

GBytes *
icc_extract(const guint8 *p_data, gsize u_len, GError **p_err) {
   g_return_val_if_fail(p_data != NULL || u_len == 0, NULL);
   GError       *p_local = NULL;
   GInputStream *p_in =
      g_memory_input_stream_new_from_data(p_data, (gssize)u_len, NULL);
   GBytes *p_icc = _walk_stream(p_in, &p_local);
   g_object_unref(p_in);
   if (p_local != NULL) {
      g_propagate_error(p_err, p_local);
   }
   return (p_icc);
}

/* --- the iCCP libpng keeps (xb2 reviews 5 and 6) --------------------------
 *
 * gegl:png-load (0.4.58 and 0.4.72 alike, gegl_png_space) tags its buffer
 * from what libpng hands it, in this order: the iCCP profile when libpng
 * kept one -- and then nothing else, even when babl declines it -- else
 * sRGB for an sRGB chunk, else, with a gAMA chunk, a space babl builds
 * from gAMA / cHRM: a new space and curve per file OUTSIDE the enhancer's
 * slot cap (110 such files filled babl's tables and the next profile
 * crashed it). libpng drops an iCCP for reasons babl does not share, so a
 * PNG is managed only when the profile ggaze vets is the one GEGL will
 * use, which is when every rule below holds; then the gAMA / cHRM fallback
 * is never reached. They are libpng 1.6's, the fatal ones, as 1.6.40
 * (fedora:40) and 1.6.58 apply them (png.c png_icc_check_length / _header
 * / _tag_table, png_colorspace_set_gamma / _check_xy, pngrutil.c
 * png_handle_iCCP / _gAMA / _cHRM / _sRGB, 1.6.58's png_handle_chunk);
 * where they differ the stricter one is taken, and the warnings are left
 * out.
 *   - one iCCP, before PLTE (a second one is "duplicate", one after PLTE
 *     "out of place"), with its stored CRC right (whatever a given libpng
 *     does with a damaged ancillary chunk, it is not vouched for);
 *   - no sRGB chunk. 1.6.40 allows one sRGB-or-iCCP: an sRGB before the
 *     iCCP makes it skip the iCCP, one after marks the colour space
 *     invalid, which throws the iCCP away (1.6.58 keeps both);
 *   - at most one gAMA and one cHRM, before PLTE, of their exact lengths
 *     (4 and 32 bytes), CRCs right, and values 1.6.40 accepts: a gamma of
 *     16 to 625 000 000, chromaticities its round trip passes (below).
 *     1.6.40 invalidates the colour space -- dropping the iCCP, before or
 *     after it -- for a duplicate or an out-of-range value, and a cHRM
 *     overflowing its arithmetic fails the load. gegl:png-save writes
 *     gAMA and cHRM beside the iCCP (so do GIMP and ImageMagick), so
 *     ggaze's own exports rely on this;
 *   - the profile: 132 bytes or more, at most ICC_PNG_MAX_PROFILE_LEN;
 *     a v4 or later one a multiple of 4 long; 'acsp'; a tag table inside
 *     it (12 bytes an entry) whose tags lie inside it; a rendering intent
 *     under 0xFFFF; a colour space of 'RGB ' on a colour PNG, 'GRAY' on a
 *     grey one, nothing else; a class other than 'abst' and 'link'; a PCS
 *     of 'XYZ ' or 'Lab '. (libpng's own size check, header against the
 *     inflated length, is icc_profile_is_sane's too.) */

/* libpng's default limit on a profile's length, PNG_USER_CHUNK_MALLOC_MAX
 * in upstream's pnglibconf.h (a distribution may raise it: Fedora 44
 * builds 1 000 000 000). The smallest a stock libpng applies, so a profile
 * over it is dropped by some libpng and declined here. */
#define ICC_PNG_MAX_PROFILE_LEN 8000000u

/* What the walk up to the image data found. */
typedef struct {
   guint8   u_ctype; /* IHDR's colour type */
   gboolean b_ihdr;  /* IHDR was read */
   gboolean b_plte;  /* a PLTE was seen: an iCCP now is out of place */
   gboolean b_gama;  /* a gAMA was taken: a second one is a duplicate */
   gboolean b_chrm;  /* a cHRM was taken: likewise */
   gboolean b_bad;   /* a chunk that costs the iCCP on some libpng: stop */
   GBytes  *p_iccp;  /* the iCCP chunk's data, CRC checked */
} PngColour;

/* --- libpng 1.6.40's cHRM check, ported ----------------------------------
 *
 * libpng 1.6.40 runs a cHRM's chromaticities through png_colorspace_check_
 * xy (png.c): xy -> XYZ -> xy in its own fixed point (1.0 = 100000), with
 * png_muldiv's floating-point build (the default, and Fedora's), and the
 * round trip within 5. A set that fails marks the colour space invalid,
 * which throws the iCCP away (read before or after it), and an internal
 * overflow is a png_error that fails the whole load; 1.6.58 checks
 * nothing at read time. So a cHRM beside an iCCP is vouched for only when
 * this port of 1.6.40's arithmetic -- int32 wrap-around included -- passes
 * it. x[] is the chunk's order: white, red, green, blue, x then y. */

#define PNG_FP_1 100000

/* png_muldiv: a * times / divisor, rounded; FALSE on overflow or a zero
 * divisor. */
static gboolean
_png_muldiv(gint32 *p_res, gint32 a, gint32 times, gint32 divisor) {
   if (divisor == 0) {
      return (FALSE);
   }
   if (a == 0 || times == 0) {
      *p_res = 0;
      return (TRUE);
   }
   double r = a;
   r *= times;
   r /= divisor;
   r = floor(r + .5);
   if (r > 2147483647. || r < -2147483648.) {
      return (FALSE);
   }
   *p_res = (gint32)r;
   return (TRUE);
}

/* png_reciprocal: 1E10 / a, rounded; 0 on overflow. */
static gint32
_png_reciprocal(gint32 a) {
   double r = floor(1E10 / a + .5);
   return (r <= 2147483647. && r >= -2147483648. ? (gint32)r : 0);
}

/* libpng's int32 sums and differences, which wrap (as two's complement). */
static gint32
_png_add(gint32 a, gint32 b) {
   return ((gint32)((guint32)a + (guint32)b));
}

static gint32
_png_sub(gint32 a, gint32 b) {
   return ((gint32)((guint32)a - (guint32)b));
}

/* png_XYZ_from_xy's first half: the red and green inverse scales and the
 * blue scale (c_s[0..2]). 0, or libpng's 1 (invalid) / 2 (png_error). */
static int
_png_xy_scales(const gint32 *x, gint32 *c_s) {
   gint32 l, r;
   if (!_png_muldiv(&l, x[4] - x[6], x[3] - x[7], 7) ||
       !_png_muldiv(&r, x[5] - x[7], x[2] - x[6], 7)) {
      return (2);
   }
   gint32 u_den = _png_sub(l, r);
   if (!_png_muldiv(&l, x[4] - x[6], x[1] - x[7], 7) ||
       !_png_muldiv(&r, x[5] - x[7], x[0] - x[6], 7)) {
      return (2);
   }
   if (!_png_muldiv(&c_s[0], x[1], u_den, _png_sub(l, r)) || c_s[0] <= x[1]) {
      return (1);
   }
   if (!_png_muldiv(&l, x[3] - x[7], x[0] - x[6], 7) ||
       !_png_muldiv(&r, x[2] - x[6], x[1] - x[7], 7)) {
      return (2);
   }
   if (!_png_muldiv(&c_s[1], x[1], u_den, _png_sub(l, r)) || c_s[1] <= x[1]) {
      return (1);
   }
   c_s[2] = _png_sub(_png_sub(_png_reciprocal(x[1]), _png_reciprocal(c_s[0])),
                     _png_reciprocal(c_s[1]));
   return (c_s[2] <= 0 ? 1 : 0);
}

/* png_XYZ_from_xy: the primaries' X, Y, Z (c_xyz, red first) of x. */
static int
_png_xyz_from_xy(const gint32 *x, gint32 *c_xyz) {
   for (int i = 0; i < 8; i += 2) {
      gint32 u_lim = i == 0 ? 5 : 0; /* white y must be 5 or more */
      if (x[i] < 0 || x[i] > PNG_FP_1 || x[i + 1] < u_lim ||
          x[i + 1] > PNG_FP_1 - x[i]) {
         return (1);
      }
   }
   gint32 c_s[3];
   int    i_rc = _png_xy_scales(x, c_s);
   for (int c = 0; c < 3 && i_rc == 0; c++) {
      gint32 c_v[3] = {x[2 + 2 * c], x[3 + 2 * c],
                       PNG_FP_1 - x[2 + 2 * c] - x[3 + 2 * c]};
      for (int k = 0; k < 3 && i_rc == 0; k++) {
         gboolean b_ok =
            c < 2 ? _png_muldiv(&c_xyz[3 * c + k], c_v[k], PNG_FP_1, c_s[c])
                  : _png_muldiv(&c_xyz[3 * c + k], c_v[k], c_s[2], PNG_FP_1);
         i_rc = b_ok ? 0 : 1;
      }
   }
   return (i_rc);
}

/* png_xy_from_XYZ: c_xyz back to chromaticities y (chunk order). */
static gboolean
_png_xy_from_xyz(const gint32 *c_xyz, gint32 *y) {
   gint32 u_dw = 0, u_wx = 0, u_wy = 0;
   for (int c = 0; c < 3; c++) {
      const gint32 *v = c_xyz + 3 * c;
      gint32        d = _png_add(_png_add(v[0], v[1]), v[2]);
      if (!_png_muldiv(&y[2 + 2 * c], v[0], PNG_FP_1, d) ||
          !_png_muldiv(&y[3 + 2 * c], v[1], PNG_FP_1, d)) {
         return (FALSE);
      }
      u_dw = _png_add(u_dw, d);
      u_wx = _png_add(u_wx, v[0]);
      u_wy = _png_add(u_wy, v[1]);
   }
   return (_png_muldiv(&y[0], u_wx, PNG_FP_1, u_dw) &&
           _png_muldiv(&y[1], u_wy, PNG_FP_1, u_dw));
}

/* TRUE iff libpng 1.6.40 takes the 32-byte cHRM data p: every value a
 * non-negative int32 (else "invalid values") and png_colorspace_check_xy
 * passing -- valid ranges, invertible, the round trip within 5. */
static gboolean
_libpng_chrm_ok(const guint8 *p) {
   gint32 x[8], y[8], c_xyz[9];
   for (int i = 0; i < 8; i++) {
      guint32 u = _be32(p + 4 * i);
      if (u > 0x7FFFFFFFu) {
         return (FALSE);
      }
      x[i] = (gint32)u;
   }
   if (_png_xyz_from_xy(x, c_xyz) != 0 || !_png_xy_from_xyz(c_xyz, y)) {
      return (FALSE);
   }
   for (int i = 0; i < 8; i++) {
      if (y[i] < x[i] - 5 || y[i] > x[i] + 5) {
         return (FALSE);
      }
   }
   return (TRUE);
}

/* TRUE iff libpng takes the 4-byte gAMA data p: 1.6.40's range, 16 to
 * 625 000 000 (outside it the colour space is invalid and the iCCP lost),
 * inside 1.6.58's (at most 2^31 - 1). */
static gboolean
_libpng_gama_ok(const guint8 *p) {
   guint32 u_g = _be32(p);
   return (u_g >= 16 && u_g <= 625000000u);
}

/* The data of a chunk of type c_t and u_len bytes (its header read), with
 * *pb_crc_ok whether its stored CRC is right; NULL on a read error. */
static GBytes *
_png_read_checked(GInputStream *p_in, const guint8 *c_t, gsize u_len,
                  gboolean *pb_crc_ok, GError **p_err) {
   GBytes *p_data = _read_payload(p_in, u_len, p_err);
   guint8  c_crc[4];
   if (p_data == NULL ||
       streamread_exact(p_in, c_crc, 4, p_err) != STREAMREAD_OK) {
      g_clear_pointer(&p_data, g_bytes_unref);
      return (NULL);
   }
   guint32 u_crc = streamread_crc32(STREAMREAD_CRC32_INIT, c_t, 4);
   u_crc      = streamread_crc32(u_crc, g_bytes_get_data(p_data, NULL), u_len);
   *pb_crc_ok = _be32(c_crc) == (u_crc ^ STREAMREAD_CRC32_INIT);
   return (p_data);
}

/* A gAMA or cHRM chunk (c_t) of u_len data bytes, its header read: taken
 * when it is the first of its kind, before PLTE, of its exact length, its
 * CRC right and its values ones both libpngs keep (above); anything else
 * sets p_c->b_bad. Stricter than libpng, never looser: a chunk libpng
 * would merely ignore (a bad CRC, one after PLTE) is refused too. */
static StreamReadStatus
_png_take_gamut(GInputStream *p_in, const guint8 *c_t, gsize u_len,
                PngColour *p_c, GError **p_err) {
   gboolean  b_gama = memcmp(c_t, "gAMA", 4) == 0;
   gboolean *pb_had = b_gama ? &p_c->b_gama : &p_c->b_chrm;
   if (*pb_had || p_c->b_plte || u_len != (b_gama ? 4u : 32u)) {
      p_c->b_bad = TRUE;
      return (STREAMREAD_OK);
   }
   gboolean b_crc  = FALSE;
   GBytes  *p_data = _png_read_checked(p_in, c_t, u_len, &b_crc, p_err);
   if (p_data == NULL) {
      return (STREAMREAD_ERROR);
   }
   const guint8 *p = g_bytes_get_data(p_data, NULL);
   *pb_had         = TRUE;
   p_c->b_bad = !b_crc || !(b_gama ? _libpng_gama_ok(p) : _libpng_chrm_ok(p));
   g_bytes_unref(p_data);
   return (STREAMREAD_OK);
}

/* libpng's fatal checks of the profile header (section comment above)
 * for a PNG of colour type u_ctype. */
static gboolean
_libpng_header_ok(const guint8 *p, gsize u_len, guint8 u_ctype) {
   guint32  u_tags  = _be32(p + 128);
   gboolean b_color = (u_ctype & 2) != 0; /* PNG_COLOR_MASK_COLOR */
   if (u_len > ICC_PNG_MAX_PROFILE_LEN || (p[8] > 3 && u_len % 4 != 0) ||
       u_tags > (u_len - 132) / 12 || _be32(p + 64) >= 0xFFFFu ||
       memcmp(p + 36, "acsp", 4) != 0) {
      return (FALSE);
   }
   gboolean b_space = memcmp(p + 16, "RGB ", 4) == 0   ? b_color
                      : memcmp(p + 16, "GRAY", 4) == 0 ? !b_color
                                                       : FALSE;
   return (b_space && memcmp(p + 12, "abst", 4) != 0 &&
           memcmp(p + 12, "link", 4) != 0 &&
           (memcmp(p + 20, "XYZ ", 4) == 0 || memcmp(p + 20, "Lab ", 4) == 0));
}

/* libpng's fatal checks of a whole profile p_icc on a PNG of colour type
 * u_ctype: the header's and every tag inside the profile. */
static gboolean
_libpng_takes(GBytes *p_icc, guint8 u_ctype) {
   gsize         u_len = 0;
   const guint8 *p     = g_bytes_get_data(p_icc, &u_len);
   if (u_len < 132 || _be32(p) != u_len ||
       !_libpng_header_ok(p, u_len, u_ctype)) {
      return (FALSE);
   }
   for (guint32 u = 0; u < _be32(p + 128); u++) {
      guint32 u_off  = _be32(p + 132 + 12 * u + 4);
      guint32 u_size = _be32(p + 132 + 12 * u + 8);
      if (u_off > u_len || u_size > u_len - u_off) {
         return (FALSE);
      }
   }
   return (TRUE);
}

/* An iCCP chunk of u_len data bytes (its header read): kept in p_c when it
 * is the first, before PLTE, and its CRC is right; otherwise p_c->b_bad. */
static StreamReadStatus
_png_take_iccp(GInputStream *p_in, const guint8 *c_t, gsize u_len,
               PngColour *p_c, GError **p_err) {
   if (p_c->p_iccp != NULL || p_c->b_plte || u_len > ICC_MAX_PROFILE_LEN) {
      p_c->b_bad = TRUE;
      return (STREAMREAD_OK);
   }
   gboolean b_crc  = FALSE;
   GBytes  *p_data = _png_read_checked(p_in, c_t, u_len, &b_crc, p_err);
   if (p_data == NULL) {
      return (STREAMREAD_ERROR);
   }
   if (!b_crc) {
      p_c->b_bad = TRUE;
      g_bytes_unref(p_data);
      return (STREAMREAD_OK);
   }
   p_c->p_iccp = p_data;
   return (STREAMREAD_OK);
}

/* One chunk (its 8-byte header c_hdr read) of the walk to the image data:
 * IHDR's colour type (IHDR must come first), the iCCP, gAMA and cHRM
 * checked, an sRGB the end of it; the rest skipped (data + CRC). */
static StreamReadStatus
_png_colour_chunk(GInputStream *p_in, const guint8 *c_hdr, PngColour *p_c,
                  GError **p_err) {
   gsize         u_len = _be32(c_hdr);
   const guint8 *c_t   = c_hdr + 4;
   if (!p_c->b_ihdr && memcmp(c_t, "IHDR", 4) != 0) {
      p_c->b_bad = TRUE; /* libpng: "missing IHDR" */
      return (STREAMREAD_OK);
   }
   if (memcmp(c_t, "iCCP", 4) == 0) {
      return (_png_take_iccp(p_in, c_t, u_len, p_c, p_err));
   }
   if (memcmp(c_t, "gAMA", 4) == 0 || memcmp(c_t, "cHRM", 4) == 0) {
      return (_png_take_gamut(p_in, c_t, u_len, p_c, p_err));
   }
   if (memcmp(c_t, "sRGB", 4) == 0) {
      p_c->b_bad = TRUE; /* 1.6.40 drops the iCCP for it (section comment) */
      return (STREAMREAD_OK);
   }
   if (memcmp(c_t, "IHDR", 4) == 0 && u_len == 13 && !p_c->b_ihdr) {
      guint8 c_ihdr[13 + 4] = {0};
      p_c->b_ihdr =
         streamread_exact(p_in, c_ihdr, sizeof(c_ihdr), p_err) == STREAMREAD_OK;
      p_c->u_ctype = c_ihdr[9];
      return (p_c->b_ihdr ? STREAMREAD_OK : STREAMREAD_ERROR);
   }
   p_c->b_plte |= memcmp(c_t, "PLTE", 4) == 0;
   return (streamread_skip(p_in, u_len + 4, p_err));
}

/* Walk p_in (a PNG, signature read) up to its image data into p_c. */
static void
_png_colour_walk(GInputStream *p_in, PngColour *p_c, GError **p_err) {
   while (!p_c->b_bad) {
      guint8 c_hdr[8];
      if (streamread_exact(p_in, c_hdr, sizeof(c_hdr), p_err) !=
             STREAMREAD_OK ||
          memcmp(c_hdr + 4, "IDAT", 4) == 0 ||
          memcmp(c_hdr + 4, "IEND", 4) == 0 ||
          _png_colour_chunk(p_in, c_hdr, p_c, p_err) != STREAMREAD_OK) {
         return;
      }
   }
}

GBytes *
icc_png_applied_profile(GFile *p_file) {
   g_return_val_if_fail(G_IS_FILE(p_file), NULL);
   GFileInputStream *p_fin = g_file_read(p_file, NULL, NULL);
   if (p_fin == NULL) {
      return (NULL);
   }
   GInputStream *p_in  = g_buffered_input_stream_new(G_INPUT_STREAM(p_fin));
   PngColour     t_c   = {0};
   GError       *p_err = NULL;
   guint8        c_sig[8];
   if (streamread_exact(p_in, c_sig, 8, &p_err) == STREAMREAD_OK &&
       memcmp(c_sig, "\x89PNG\r\n\x1a\n", 8) == 0) {
      _png_colour_walk(p_in, &t_c, &p_err);
   }
   g_input_stream_close(p_in, NULL, NULL);
   g_object_unref(p_in);
   g_object_unref(p_fin);
   GBytes *p_icc = NULL;
   if (p_err == NULL && t_c.b_ihdr && !t_c.b_bad && t_c.p_iccp != NULL) {
      p_icc = _png_inflate_iccp(t_c.p_iccp, &p_err);
   }
   if (p_icc != NULL && !_libpng_takes(p_icc, t_c.u_ctype)) {
      g_clear_pointer(&p_icc, g_bytes_unref);
   }
   g_clear_pointer(&t_c.p_iccp, g_bytes_unref);
   g_clear_error(&p_err);
   return (p_icc);
}

/* --- the profile itself: header + 'desc' tag ------------------------------
 *
 * ICC layout: a 128-byte header ('acsp' at offset 36, the profile size at
 * 0), the tag count at 128, then 12-byte table entries (signature, offset,
 * size) pointing at tagged data whose first 4 bytes name its type. */

gboolean
icc_is_profile(GBytes *p_icc) {
   if (p_icc == NULL) {
      return (FALSE);
   }
   gsize         u_len;
   const guint8 *p = g_bytes_get_data(p_icc, &u_len);
   if (u_len < 132 || memcmp(p + 36, "acsp", 4) != 0) {
      return (FALSE);
   }
   guint32 u_size = _be32(p);
   return (u_size >= 132 && u_size <= u_len);
}

/* Locate tag c_sig's data (*pp_tag, *pu_size), bounds-checked against the
 * real byte count. FALSE when absent, when the table itself overruns the
 * data, or when the entry points outside it. */
static gboolean
_find_tag(const guint8 *p, gsize u_len, const char *c_sig,
          const guint8 **pp_tag, gsize *pu_size) {
   guint32 u_count = _be32(p + 128);
   if (u_count > (u_len - 132) / 12) {
      return (FALSE);
   }
   for (guint32 u = 0; u < u_count; u++) {
      const guint8 *p_e = p + 132 + 12 * u;
      if (memcmp(p_e, c_sig, 4) != 0) {
         continue;
      }
      guint32 u_off  = _be32(p_e + 4);
      guint32 u_size = _be32(p_e + 8);
      if (u_off > u_len || u_size > u_len - u_off || u_size < 8) {
         return (FALSE);
      }
      *pp_tag  = p + u_off;
      *pu_size = u_size;
      return (TRUE);
   }
   return (FALSE);
}

/* v2 textDescriptionType: type, 4 reserved, ASCII count (NUL included),
 * the ASCII text. The Unicode / ScriptCode copies that follow are ignored.
 */
static char *
_desc_ascii(const guint8 *p_tag, gsize u_size) {
   if (u_size < 12) {
      return (NULL);
   }
   guint32 u_n = _be32(p_tag + 8);
   if (u_n == 0 || u_n > u_size - 12) {
      return (NULL);
   }
   char *c_text = g_strndup((const char *)p_tag + 12, u_n);
   if (!g_utf8_validate(c_text, -1, NULL)) {
      g_free(c_text);
      return (NULL);
   }
   return (c_text);
}

/* UTF-16BE code units to UTF-8; NULL on an invalid surrogate sequence. */
static char *
_utf16be_to_utf8(const guint8 *p_text, gsize u_units) {
   gunichar2 *p_u = g_new(gunichar2, u_units);
   for (gsize u = 0; u < u_units; u++) {
      p_u[u] = (gunichar2)(((guint)p_text[2 * u] << 8) | p_text[2 * u + 1]);
   }
   char *c_text = g_utf16_to_utf8(p_u, (glong)u_units, NULL, NULL, NULL);
   g_free(p_u);
   return (c_text);
}

/* v4 multiLocalizedUnicodeType: type, 4 reserved, record count, record
 * size, then records of (language 2, country 2, length 4, offset 4) whose
 * text is UTF-16BE at the offset from the tag's start. */
static char *
_desc_mluc(const guint8 *p_tag, gsize u_size) {
   if (u_size < 16) {
      return (NULL);
   }
   guint32 u_n   = _be32(p_tag + 8);
   guint32 u_rec = _be32(p_tag + 12);
   if (u_n == 0 || u_rec < 12 || u_n > (u_size - 16) / u_rec) {
      return (NULL);
   }
   const guint8 *p_pick = p_tag + 16;
   for (guint32 u = 0; u < u_n; u++) {
      const guint8 *p_r = p_tag + 16 + u * u_rec;
      if (memcmp(p_r, "en", 2) == 0) {
         p_pick = p_r;
         break;
      }
   }
   guint32 u_tlen = _be32(p_pick + 4);
   guint32 u_toff = _be32(p_pick + 8);
   if (u_tlen < 2 || u_tlen % 2 != 0 || u_toff > u_size ||
       u_tlen > u_size - u_toff) {
      return (NULL);
   }
   return (_utf16be_to_utf8(p_tag + u_toff, u_tlen / 2));
}

char *
icc_description(GBytes *p_icc) {
   if (!icc_is_profile(p_icc)) {
      return (NULL);
   }
   gsize         u_len;
   const guint8 *p      = g_bytes_get_data(p_icc, &u_len);
   const guint8 *p_tag  = NULL;
   gsize         u_size = 0;
   if (!_find_tag(p, u_len, "desc", &p_tag, &u_size)) {
      return (NULL);
   }
   char *c_text = NULL;
   if (memcmp(p_tag, "desc", 4) == 0) {
      c_text = _desc_ascii(p_tag, u_size);
   } else if (memcmp(p_tag, "mluc", 4) == 0) {
      c_text = _desc_mluc(p_tag, u_size);
   }
   if (c_text != NULL && *g_strstrip(c_text) == '\0') {
      g_free(c_text);
      c_text = NULL;
   }
   return (c_text);
}

/* --- the profile as babl will parse it ------------------------------------
 *
 * babl_space_from_icc() trusts the tag data it reads: a 'curv' count is a
 * loop bound and an allocation size with no check against the tag (a huge
 * count reads past the buffer or makes babl_fatal() exit the process), and
 * its tag lookup walks a tag count taken straight from the file. So a
 * profile goes to babl only when everything babl reads lies inside the
 * profile, sized for what babl reads from it -- and when what babl BUILDS
 * from it stays inside babl's own fixed buffers and assertions (the curve
 * rules below). All of it read off babl-icc.c, babl-core.c and
 * base/babl-trc.c of babl 0.1.128, and each abort / crash reproduced
 * there (xb2 reviews 3 and 4). */

/* babl asserts 0 <= x0 < 254.5 / 255 (~0.99804) for both ends of a
 * piecewise 'para' curve's polynomial approximation (babl-polynomial.c,
 * babl_polynomial_approximate_gamma, reached from base/babl-trc.c's
 * babl_trc_new for BABL_TRC_FORMULA_SRGB); 0.998 keeps well clear of float
 * rounding at the edge. */
#define ICC_PARA_X0_LIMIT 0.998f

/* babl writes the profile of every new RGB space it makes (babl-space.c,
 * babl_space_from_rgbxyz_matrix -> babl_space_get_icc ->
 * babl_space_to_icc_rgb) into a `char icc[65536]` on its stack and then
 * copies out as many bytes as it allocated: a 240-byte header and tag
 * table, 80 bytes of XYZ tags, a 'cprt' and a 'desc' (~130 bytes), and
 * each tone curve as 12 + 2 * points bytes (a formula curve as 512
 * points) -- one curve when r, g and b are the same babl curve, all three
 * otherwise, even when two of them match. Past 64 KiB the copy reads off
 * the stack (a shared 65536-point curve crashed it). With every curve at
 * most ICC_MAX_CURVE_POINTS points, three of them and the rest fit with
 * room to spare, whatever the curves share. */
G_STATIC_ASSERT(240u + 80u + 256u + 3u * (12u + 2u * ICC_MAX_CURVE_POINTS) <
                65536u);

/* A big-endian s15Fixed16Number, as babl reads it (read_s15f16: the high
 * half signed, the low half its fraction), as the float babl keeps it in.
 * Always finite: at most +-32768. */
static float
_s15f16(const guint8 *p) {
   return ((float)((gint32)_be32(p) / 65536.0));
}

/* Parameter count of each 'para' function type (ICC.1 10.18); 0 for a
 * type babl must not see. Types 1 and 2 (the CIE 122-1966 / IEC 61966-3
 * forms) are refused: babl_trc_formula_cie() packs 4 parameters into
 * float[4] and base/babl-trc.c then reads a fifth, lut[4], as the curve's
 * polynomial start -- stack garbage, an out-of-bounds read, and one bad
 * draw from babl's x0 assertion. Real profiles use type 0 (a gamma) or
 * type 3 (sRGB-like); any unknown type babl would replace with gamma 2.2
 * (and complain on stderr). */
static const guint PARA_PARAMS[] = {1, 0, 0, 5, 7};

/* How far a 'para' curve's parameters may go (xb2 review 5). Real curves
 * sit far inside: a gamma g of 1.8 to 2.6 (sRGB's piecewise 2.4, Rec.
 * 709's 1 / 0.45, DCI's 2.6), a scale a near 1, an offset b near 0.05 to
 * 0.1, a slope c of 1 / 12.92 to 1 / 4.5 and offsets e, f near 0. The
 * bound is what keeps babl working, not taste: babl NAMES a formula curve
 * after its seven parameters ("%i.%06i ...", babl-core.c), a space after
 * its primaries and three curves' names, and each format of that space
 * "<encoding>-<space>" in a 256-byte buffer (babl-format.c) -- an
 * s15Fixed16 at -32767 made the space's name so long that its format names
 * were cut short, two formats then share a name, and babl's fish search
 * between them spun forever in an uncancellable GEGL decode. g and a in
 * (0, ICC_PARA_MAX], every other parameter within +-ICC_PARA_MAX, keep
 * each printed parameter to eight characters; enhancer.c also checks the
 * name babl built (GGAZE_ENHANCER_MAX_SPACE_NAME). */
#define ICC_PARA_MAX 10.0f

/* Whether the u_n parameters at p_p (s15Fixed16) are in the bounds above:
 * the first two (g, a) positive. Type 0 has only g. */
static gboolean
_para_params_in_range(const guint8 *p_p, guint u_n) {
   for (guint u = 0; u < u_n; u++) {
      float f_v = _s15f16(p_p + 4 * u);
      if (f_v > ICC_PARA_MAX || f_v < -ICC_PARA_MAX || (u < 2 && f_v <= 0)) {
         return (FALSE);
      }
   }
   return (TRUE);
}

/* A 'para' curve babl can take (above): the reserved word zero -- babl
 * tells 'para' from 'curv' with strcmp(data, "para"), so a nonzero byte 4
 * turns the tag into a 'curv' whose "count" is the function type and
 * padding (up to 0x4FFFF points: the 64 KiB overrun, reproduced as a
 * SIGSEGV) -- a known function type with all its parameters, each within
 * ICC_PARA_MAX, and for the piecewise types 3 and 4 the curve's break
 * point d and its linear segment's end c * d both in [0,
 * ICC_PARA_X0_LIMIT): babl approximates the curve from x0 = d (to linear)
 * and x0 = c * d (from linear), each an assertion that aborts the process
 * when it fails. (babl would clamp a negative type 0 gamma to 0, a flat
 * curve: refused by the bounds and by _curve_is_tone.) */
static gboolean
_para_is_sane(const guint8 *p_tag, guint32 u_size) {
   guint u_fn = ((guint)p_tag[8] << 8) | p_tag[9];
   if (_be32(p_tag + 4) != 0 || u_fn >= G_N_ELEMENTS(PARA_PARAMS) ||
       PARA_PARAMS[u_fn] == 0 || 12u + 4u * PARA_PARAMS[u_fn] > u_size ||
       !_para_params_in_range(p_tag + 12, PARA_PARAMS[u_fn])) {
      return (FALSE);
   }
   if (u_fn < 3) {
      return (TRUE);
   }
   float f_c  = _s15f16(p_tag + 12 + 4 * 3);
   float f_d  = _s15f16(p_tag + 12 + 4 * 4);
   float f_cd = f_c * f_d; /* in float, as babl multiplies them */
   return (f_d >= 0.0f && f_d < ICC_PARA_X0_LIMIT && f_cd >= 0.0f &&
           f_cd < ICC_PARA_X0_LIMIT);
}

/* --- the curve's shape (xb2 review 5) -------------------------------------
 *
 * babl builds the conversion to every format it is asked for by trying
 * candidate paths until one converts its test pixels closely enough. For
 * a curve it cannot invert -- flat, or falling somewhere -- none does, the
 * search runs on into its deepest candidates, and one of those
 * (babl_conversion_planar_process, babl 0.1.128) overflows a buffer:
 * glibc's fortify check aborted the JPEG export of a file whose profile had
 * a constant 'curv' ([0, 0], [30000, 30000], [65535, 65535]), a 'curv'
 * with one spike, or a 'para' that never enters [0, 1]. So a curve goes
 * to babl only when it is shaped like a tone curve: sampled over [0, 1]
 * (a 'curv' table at its own points, a formula at ICC_CURVE_SAMPLES) and
 * clamped to [0, 1], it
 *   - never falls (a table not by a single u16 step; a formula not by
 *     more than rounding),
 *   - rises by at least ICC_CURVE_MIN_SPAN from its first sample to its
 *     last, and
 *   - is not flat over half its domain or more (steps rising less than
 *     half a u16 step count as flat).
 * The corpus's curves (4096- and 1024-point tables, sRGB 'para' curves, a
 * u8Fixed8 gamma of 2.2), the fixtures' and Rec. 709's pass with room to
 * spare. A gamma past ~11 fails the flat rule (x^g stays within half a
 * u16 step of 0 for half the domain), a curve covering less than half the
 * output range the span rule. */
#define ICC_CURVE_SAMPLES 1024u
#define ICC_CURVE_MIN_SPAN 0.5
#define ICC_CURVE_FLAT_STEP (0.5 / 65535.0)
#define ICC_CURVE_FALL_EPS 1e-9

typedef struct {
   gboolean b_started;  /* a first sample was taken */
   double   f_first;    /* the first sample */
   double   f_prev;     /* the latest sample */
   guint    u_steps;    /* samples after the first */
   guint    u_flat;     /* the current run of flat steps */
   guint    u_max_flat; /* the longest run of flat steps */
   gboolean b_falls;    /* a step fell, or a sample was NaN */
} CurveShape;

/* Add the next sample f_y of a curve (clamped to [0, 1]) to p_s. */
static void
_shape_add(CurveShape *p_s, double f_y) {
   if (isnan(f_y)) {
      p_s->b_falls = TRUE;
      return;
   }
   f_y = CLAMP(f_y, 0.0, 1.0);
   if (!p_s->b_started) {
      p_s->b_started = TRUE;
      p_s->f_first   = f_y;
      p_s->f_prev    = f_y;
      return;
   }
   double f_rise = f_y - p_s->f_prev;
   p_s->b_falls |= f_rise < -ICC_CURVE_FALL_EPS;
   p_s->u_flat     = f_rise < ICC_CURVE_FLAT_STEP ? p_s->u_flat + 1 : 0;
   p_s->u_max_flat = MAX(p_s->u_max_flat, p_s->u_flat);
   p_s->u_steps++;
   p_s->f_prev = f_y;
}

static gboolean
_shape_ok(const CurveShape *p_s) {
   return (!p_s->b_falls && p_s->u_steps > 0 &&
           p_s->f_prev - p_s->f_first >= ICC_CURVE_MIN_SPAN &&
           2u * p_s->u_max_flat < p_s->u_steps);
}

/* babl's to-linear value of a gamma: x^g for x > 0, else 0
 * (_babl_trc_gamma_to_linear). */
static double
_gamma_at(double f_x, double f_g) {
   return (f_x > 0 ? pow(f_x, f_g) : 0.0);
}

/* babl's to-linear value at f_x of the 'para' at p_tag (a type 0, 3 or 4
 * _para_is_sane took): _babl_trc_formula_srgb_to_linear's form, which is
 * ICC's, e and f 0 for type 3. */
static double
_para_at(const guint8 *p_tag, double f_x) {
   guint  u_fn   = p_tag[9];
   double f_p[7] = {0};
   for (guint u = 0; u < PARA_PARAMS[u_fn]; u++) {
      f_p[u] = _s15f16(p_tag + 12 + 4 * u);
   }
   if (u_fn == 0) {
      return (_gamma_at(f_x, f_p[0]));
   }
   return (f_x >= f_p[4] ? _gamma_at(f_p[1] * f_x + f_p[2], f_p[0]) + f_p[5]
                         : f_p[3] * f_x + f_p[6]);
}

/* Whether a TRC tag babl can read (a 'curv' _trc_is_sane sized, or a
 * 'para' _para_is_sane took) is shaped like a tone curve (above). A
 * 'curv' of no points is the identity. */
static gboolean
_curve_is_tone(const guint8 *p_tag) {
   CurveShape t_s    = {0};
   gboolean   b_curv = memcmp(p_tag, "curv", 4) == 0;
   guint32    u_n    = b_curv ? _be32(p_tag + 8) : 0;
   if (b_curv && u_n == 0) {
      return (TRUE);
   }
   if (u_n >= 2) {
      for (guint32 u = 0; u < u_n; u++) {
         const guint8 *p_v = p_tag + 12 + 2 * u;
         _shape_add(&t_s, (((guint)p_v[0] << 8) | p_v[1]) / 65535.0);
      }
      return (_shape_ok(&t_s));
   }
   /* One point is a u8Fixed8 gamma; otherwise a formula. */
   double f_g = b_curv ? (((guint)p_tag[12] << 8) | p_tag[13]) / 256.0 : 0;
   for (guint u = 0; u < ICC_CURVE_SAMPLES; u++) {
      double f_x = u / (double)(ICC_CURVE_SAMPLES - 1);
      _shape_add(&t_s, b_curv ? _gamma_at(f_x, f_g) : _para_at(p_tag, f_x));
   }
   return (_shape_ok(&t_s));
}

/* The tone curve tags babl reads: 'curv' with 12 + 2 * count bytes and at
 * most ICC_MAX_CURVE_POINTS points, or a 'para' _para_is_sane() takes --
 * either shaped like a tone curve (_curve_is_tone). A TRC tag of another
 * type is refused: babl would read it as a 'curv' count. (A 'curv' needs
 * no zero reserved word: babl reads anything that is not "para" as a
 * 'curv', which it then is.) */
static gboolean
_trc_is_sane(const guint8 *p_tag, guint32 u_size) {
   gboolean b_ok = FALSE;
   if (u_size < 12) {
      return (FALSE);
   }
   if (memcmp(p_tag, "curv", 4) == 0) {
      guint32  u_count = _be32(p_tag + 8);
      gboolean b_fits  = 12u + 2u * (guint64)u_count <= u_size;
      b_ok             = u_count <= ICC_MAX_CURVE_POINTS && b_fits;
   } else if (memcmp(p_tag, "para", 4) == 0) {
      b_ok = _para_is_sane(p_tag, u_size);
   }
   return (b_ok && _curve_is_tone(p_tag));
}

/* The type and least size of every other tag babl (or, for 'chad', LCMS
 * behind babl's CMYK path) reads a fixed layout from: three s15Fixed16
 * numbers after the 8-byte type header for an XYZ tag, the channel count,
 * phosphor type and three xy pairs for 'chrm', nine numbers for 'chad'. */
static const struct {
   const char *c_sig;
   const char *c_type;
   guint32     u_min;
} FIXED_TAGS[] = {
   {"rXYZ", "XYZ ", 20}, {"gXYZ", "XYZ ", 20}, {"bXYZ", "XYZ ", 20},
   {"wtpt", "XYZ ", 20}, {"chrm", "chrm", 36}, {"chad", "sf32", 44},
};

/* Whether one tag's data, already known to lie inside the profile, is
 * what babl will take it for (above). Tags babl does not read pass. */
static gboolean
_tag_data_is_sane(const guint8 *p_sig, const guint8 *p_tag, guint32 u_size) {
   if (memcmp(p_sig, "rTRC", 4) == 0 || memcmp(p_sig, "gTRC", 4) == 0 ||
       memcmp(p_sig, "bTRC", 4) == 0 || memcmp(p_sig, "kTRC", 4) == 0) {
      return (_trc_is_sane(p_tag, u_size));
   }
   for (gsize u = 0; u < G_N_ELEMENTS(FIXED_TAGS); u++) {
      if (memcmp(p_sig, FIXED_TAGS[u].c_sig, 4) == 0) {
         return (memcmp(p_tag, FIXED_TAGS[u].c_type, 4) == 0 &&
                 u_size >= FIXED_TAGS[u].u_min);
      }
   }
   return (TRUE);
}

gboolean
icc_profile_is_sane(GBytes *p_icc) {
   if (!icc_is_profile(p_icc)) {
      return (FALSE);
   }
   gsize         u_len;
   const guint8 *p = g_bytes_get_data(p_icc, &u_len);
   /* babl refuses a size field other than the buffer's; a tag table past
    * ICC_MAX_TAGS is a loop babl would run for every lookup. */
   guint32 u_count = _be32(p + 128);
   if (_be32(p) != u_len || u_count > ICC_MAX_TAGS ||
       132u + 12u * (guint64)u_count > u_len) {
      return (FALSE);
   }
   guint64 u_data = 132u + 12u * (guint64)u_count;
   for (guint32 u = 0; u < u_count; u++) {
      const guint8 *p_e    = p + 132 + 12 * u;
      guint32       u_off  = _be32(p_e + 4);
      guint32       u_size = _be32(p_e + 8);
      /* Every tag (not only the ones babl reads: LCMS reads the others on
       * the CMYK path) lies after the table and inside the profile, with
       * room for its type signature and reserved word. Tags may share
       * data -- rTRC / gTRC / bTRC often do -- so overlap between tags is
       * allowed; a tag over the header or the table is not. */
      if (u_off < u_data || u_size < 8 || (guint64)u_off + u_size > u_len ||
          !_tag_data_is_sane(p_e, p + u_off, u_size)) {
         return (FALSE);
      }
   }
   return (TRUE);
}

/* --- what babl will make of it --------------------------------------------
 *
 * babl_space_from_icc()'s own early exits (babl-icc.c, 0.1.112 and
 * 0.1.128, with BABL_ICC_INTENT_RELATIVE_COLORIMETRIC, the intent the
 * gegl loaders pass; 0.1.128 also names it BABL_ICC_INTENT_DEFAULT, 0.1.112
 * does not), in
 * its order, so a caller can keep babl from being asked at all: a CMYK
 * profile goes to LCMS whatever its class; otherwise the colour space must
 * be RGB or grey, the class a display or input one, the PCS XYZ, and a
 * profile with both A2B0 and B2A0 is left to LCMS. Those checks cost babl
 * nothing; the ones after them -- a curve missing, no primaries, chrm with
 * other than 3 channels or phosphor 0, Argyll's inconsistent CLUT +
 * matrix profiles -- come AFTER babl has parsed (and kept) the profile's
 * tone curves, so they are checked here too. */

static gboolean
_has_tag(const guint8 *p, gsize u_len, const char *c_sig) {
   const guint8 *p_tag  = NULL;
   gsize         u_size = 0;
   return (_find_tag(p, u_len, c_sig, &p_tag, &u_size));
}

/* The primaries babl reads from an RGB profile: the XYZ tags and the white
 * point -- and then, with a CLUT beside them, red's Z not above its X
 * (Argyll's deliberately swapped matrix, which babl refuses) -- or else a
 * 'chrm' of 3 channels and phosphor type 0 with the white point. */
static gboolean
_rgb_primaries_ok(const guint8 *p, gsize u_len) {
   const guint8 *p_tag  = NULL;
   gsize         u_size = 0;
   if (_find_tag(p, u_len, "rXYZ", &p_tag, &u_size) &&
       _has_tag(p, u_len, "gXYZ") && _has_tag(p, u_len, "bXYZ") &&
       _has_tag(p, u_len, "wtpt")) {
      gboolean b_clut =
         _has_tag(p, u_len, "A2B0") || _has_tag(p, u_len, "B2A0");
      return (!b_clut || (gint32)_be32(p_tag + 16) <= (gint32)_be32(p_tag + 8));
   }
   if (_find_tag(p, u_len, "chrm", &p_tag, &u_size) &&
       _has_tag(p, u_len, "wtpt")) {
      guint u_channels = ((guint)p_tag[8] << 8) | p_tag[9];
      guint u_phosphor = ((guint)p_tag[10] << 8) | p_tag[11];
      return (u_channels == 3 && u_phosphor == 0);
   }
   return (FALSE);
}

IccBablKind
icc_babl_kind(GBytes *p_icc) {
   if (!icc_profile_is_sane(p_icc)) {
      return (ICC_BABL_NONE);
   }
   gsize         u_len;
   const guint8 *p = g_bytes_get_data(p_icc, &u_len);
   if (memcmp(p + 16, "CMYK", 4) == 0) {
      return (ICC_BABL_CMYK);
   }
   gboolean b_rgb  = memcmp(p + 16, "RGB ", 4) == 0;
   gboolean b_gray = memcmp(p + 16, "GRAY", 4) == 0;
   gboolean b_class =
      memcmp(p + 12, "mntr", 4) == 0 || memcmp(p + 12, "scnr", 4) == 0;
   if (!(b_rgb || b_gray) || !b_class || memcmp(p + 20, "XYZ ", 4) != 0 ||
       (_has_tag(p, u_len, "A2B0") && _has_tag(p, u_len, "B2A0"))) {
      return (ICC_BABL_NONE);
   }
   if (b_gray) {
      return (_has_tag(p, u_len, "kTRC") ? ICC_BABL_GRAY : ICC_BABL_NONE);
   }
   gboolean b_curves = _has_tag(p, u_len, "rTRC") &&
                       _has_tag(p, u_len, "gTRC") && _has_tag(p, u_len, "bTRC");
   return (b_curves && _rgb_primaries_ok(p, u_len) ? ICC_BABL_RGB
                                                   : ICC_BABL_NONE);
}

guint
icc_babl_curve_tags(GBytes *p_icc) {
   static const char *C_SIGS[] = {"rTRC", "gTRC", "bTRC", "kTRC"};
   if (!icc_is_profile(p_icc)) {
      return (0);
   }
   gsize         u_len;
   const guint8 *p   = g_bytes_get_data(p_icc, &u_len);
   guint         u_n = 0;
   for (gsize u = 0; u < G_N_ELEMENTS(C_SIGS); u++) {
      u_n += _has_tag(p, u_len, C_SIGS[u]) ? 1 : 0;
   }
   return (u_n);
}
