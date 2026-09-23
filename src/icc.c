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
 * profile, sized for what babl reads from it (babl-icc.c, 0.1.128). */

/* Parameter count of each 'para' function type babl knows (ICC.1 10.18);
 * any other type is refused (babl would fall back to a guessed gamma). */
static const guint PARA_PARAMS[] = {1, 3, 4, 5, 7};

/* The tone curve tags babl reads: 'curv' with 12 + 2 * count bytes and at
 * most ICC_MAX_CURVE_POINTS points, or 'para' of a known type with all its
 * s15Fixed16 parameters. A TRC tag of another type is refused: babl would
 * read it as a 'curv' count. */
static gboolean
_trc_is_sane(const guint8 *p_tag, guint32 u_size) {
   if (u_size < 12) {
      return (FALSE);
   }
   if (memcmp(p_tag, "curv", 4) == 0) {
      guint32 u_count = _be32(p_tag + 8);
      return (u_count <= ICC_MAX_CURVE_POINTS &&
              12u + 2u * (guint64)u_count <= u_size);
   }
   if (memcmp(p_tag, "para", 4) == 0) {
      guint u_fn = ((guint)p_tag[8] << 8) | p_tag[9];
      return (u_fn < G_N_ELEMENTS(PARA_PARAMS) &&
              12u + 4u * PARA_PARAMS[u_fn] <= u_size);
   }
   return (FALSE);
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
