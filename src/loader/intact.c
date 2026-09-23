/*:*
 * ggaze — will GEGL's PNG / JPEG loader get through this file?
 *
 * See intact.h. Both walks run on a buffered GFileInputStream through
 * streamread.c. A file that ends early is G_IO_ERROR_INVALID_DATA with the
 * word "truncated", a PNG whose data libpng would reject is INVALID_DATA
 * with "corrupt", so a log line names the cause.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "intact.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "ggaze-config.h"
#include "streamread.h"

#if GGAZE_HAVE_JPEG
#include <jpeglib.h>
#include <setjmp.h>
#include <stdio.h>
#endif

/* Chunk data is read (and CRC'd / inflated) in blocks of this size. */
#define INTACT_BLOCK 32768

static gboolean
_truncated(const char *c_what, GError **p_err) {
   g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "%s is truncated (ends before its %s)", c_what,
               c_what[0] == 'P' ? "IEND chunk" : "EOI marker");
   return (FALSE);
}

/* A PNG that libpng would stop on: STREAMREAD_ERROR with INVALID_DATA. */
static StreamReadStatus
_png_corrupt(const char *c_why, GError **p_err) {
   g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "PNG is corrupt (%s)", c_why);
   return (STREAMREAD_ERROR);
}

static guint32
_be32(const guint8 *p) {
   return (((guint32)p[0] << 24) | ((guint32)p[1] << 16) |
           ((guint32)p[2] << 8) | (guint32)p[3]);
}

/* --- CRC-32 (ISO 3309, the one PNG chunks carry) ------------------------
 *
 * libpng stops on a critical chunk whose CRC does not match, so the walk
 * checks it. GLib has no CRC-32 and ggaze links no zlib of its own (GIO's
 * zlib converter below does the inflating), hence the 256-entry table. */

static guint32 u_crc_table[256];

static void
_crc_init(void) {
   static gsize u_once = 0;
   if (g_once_init_enter(&u_once)) {
      for (guint32 u = 0; u < 256; u++) {
         guint32 u_c = u;
         for (int i = 0; i < 8; i++) {
            u_c = (u_c & 1) ? 0xEDB88320u ^ (u_c >> 1) : u_c >> 1;
         }
         u_crc_table[u] = u_c;
      }
      g_once_init_leave(&u_once, 1);
   }
}

/* Running CRC over p (start with 0xFFFFFFFF, finish by inverting). */
static guint32
_crc_update(guint32 u_crc, const guint8 *p, gsize u_len) {
   for (gsize u = 0; u < u_len; u++) {
      u_crc = u_crc_table[(u_crc ^ p[u]) & 0xFF] ^ (u_crc >> 8);
   }
   return (u_crc);
}

/* --- PNG image data: the rows IHDR promises -----------------------------
 *
 * The inflated IDAT stream is, per Adam7 pass (one pass when the image is
 * not interlaced), rows of one filter-type byte (0-4) and the row's
 * packed pixels. libpng errors on a stream that ends before the last row
 * ("Not enough image data") and on a filter byte over 4; it only warns
 * about data past the last row. So the walk follows the rows and stops
 * counting once all are seen. */

typedef struct {
   guint32  u_w, u_h;   /* IHDR */
   guint    u_bpp;      /* bits per pixel */
   gboolean b_adam7;    /* interlaced */
   int      i_pass;     /* index into C_PASSES; before the first pass,
                         * the one ahead of it */
   guint32 u_rows_left; /* rows left in the pass */
   gsize   u_rowbytes;  /* a row's pixel bytes in this pass */
   gsize   u_in_row;    /* bytes of the row still to come; 0 = a filter
                         * byte is next */
   gboolean b_done;     /* every row seen */
} PngRows;

/* Pass origins and steps (x0, y0, dx, dy): the seven Adam7 passes, then
 * the single pass of a non-interlaced image. */
static const guint8 C_PASSES[8][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8},
                                      {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2},
                                      {0, 1, 1, 2}, {0, 0, 1, 1}};
#define PASS_PLAIN 7

/* Pixels a pass covers along a side of n pixels, from origin u0, step u_d. */
static guint32
_pass_extent(guint32 u_n, guint u0, guint u_d) {
   return (u_n > u0 ? (u_n - u0 + u_d - 1) / u_d : 0);
}

/* Move to the next pass that has pixels (an Adam7 pass of a tiny image
 * may have none), or mark the image done. */
static void
_rows_next_pass(PngRows *p_r) {
   int i_last = p_r->b_adam7 ? PASS_PLAIN - 1 : PASS_PLAIN;
   while (++p_r->i_pass <= i_last) {
      const guint8 *p_g    = C_PASSES[p_r->i_pass];
      guint32       u_cols = _pass_extent(p_r->u_w, p_g[0], p_g[2]);
      guint32       u_rows = _pass_extent(p_r->u_h, p_g[1], p_g[3]);
      if (u_cols > 0 && u_rows > 0) {
         p_r->u_rows_left = u_rows;
         p_r->u_rowbytes  = ((gsize)u_cols * p_r->u_bpp + 7) / 8;
         return;
      }
   }
   p_r->b_done = TRUE;
}

/* Bits per pixel for an IHDR colour type / bit depth pair, 0 when libpng
 * would refuse the pair. */
static guint
_png_bpp(guint8 u_type, guint8 u_depth) {
   static const struct {
      guint8 u_type, u_channels, u_depths; /* bit i set: depth 1 << i ok */
   } C_TYPES[] = {
      {0, 1, 0x1F}, {2, 3, 0x18}, {3, 1, 0x0F}, {4, 2, 0x18}, {6, 4, 0x18}};
   for (gsize u = 0; u < G_N_ELEMENTS(C_TYPES); u++) {
      if (C_TYPES[u].u_type == u_type) {
         for (guint i = 0; i < 5; i++) {
            if (u_depth == (1u << i) && (C_TYPES[u].u_depths >> i) & 1) {
               return (C_TYPES[u].u_channels * u_depth);
            }
         }
      }
   }
   return (0);
}

/* Set the row plan up from the 13 IHDR data bytes; FALSE for a header
 * libpng refuses (zero side, bad type / depth / method). */
static gboolean
_rows_start(PngRows *p_r, const guint8 *p_ihdr) {
   memset(p_r, 0, sizeof(*p_r));
   p_r->u_w     = _be32(p_ihdr);
   p_r->u_h     = _be32(p_ihdr + 4);
   p_r->u_bpp   = _png_bpp(p_ihdr[9], p_ihdr[8]);
   p_r->b_adam7 = p_ihdr[12] == 1;
   p_r->i_pass  = p_r->b_adam7 ? -1 : PASS_PLAIN - 1;
   if (p_r->u_w == 0 || p_r->u_h == 0 || p_r->u_bpp == 0 || p_ihdr[10] != 0 ||
       p_ihdr[11] != 0 || p_ihdr[12] > 1) {
      return (FALSE);
   }
   _rows_next_pass(p_r);
   return (TRUE);
}

/* Account for u_len inflated bytes; FALSE on a filter byte over 4. */
static gboolean
_rows_consume(PngRows *p_r, const guint8 *p, gsize u_len) {
   while (u_len > 0 && !p_r->b_done) {
      if (p_r->u_in_row == 0) {
         if (*p > 4) {
            return (FALSE);
         }
         p++;
         u_len--;
         p_r->u_in_row = p_r->u_rowbytes;
         continue;
      }
      gsize u_take = MIN(u_len, p_r->u_in_row);
      p += u_take;
      u_len -= u_take;
      p_r->u_in_row -= u_take;
      if (p_r->u_in_row == 0 && --p_r->u_rows_left == 0) {
         _rows_next_pass(p_r);
      }
   }
   return (TRUE);
}

/* --- the PNG walk --------------------------------------------------------- */

typedef struct {
   PngRows     t_rows;
   GConverter *p_dec;  /* zlib inflater over the concatenated IDAT data */
   gboolean    b_ihdr; /* IHDR seen (it must come first) */
   gboolean    b_idat; /* an IDAT seen */
   gboolean    b_zend; /* the zlib stream ended */
   IntactSize *p_size; /* the caller's, never NULL here */
} PngWalk;

/* Inflate one block of IDAT data into a scratch buffer and follow the
 * rows through it. The inflater asks for more input (PARTIAL_INPUT) when
 * the block ends mid-stream: the next IDAT continues it. */
static StreamReadStatus
_idat_feed(PngWalk *p_w, const guint8 *p_in, gsize u_len, GError **p_err) {
   guint8 c_out[INTACT_BLOCK];
   while (!p_w->b_zend && !p_w->t_rows.b_done) {
      gsize            u_read = 0, u_wrote = 0;
      GError          *p_conv = NULL;
      GConverterResult e_res =
         g_converter_convert(p_w->p_dec, p_in, u_len, c_out, sizeof(c_out),
                             G_CONVERTER_NO_FLAGS, &u_read, &u_wrote, &p_conv);
      if (e_res == G_CONVERTER_ERROR) {
         gboolean b_more =
            g_error_matches(p_conv, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT);
         g_error_free(p_conv);
         return (b_more ? STREAMREAD_OK
                        : _png_corrupt("image data does not inflate", p_err));
      }
      p_in += u_read;
      u_len -= u_read;
      p_w->b_zend = e_res == G_CONVERTER_FINISHED;
      if (!_rows_consume(&p_w->t_rows, c_out, u_wrote)) {
         return (_png_corrupt("bad row filter", p_err));
      }
      if (u_len == 0 && u_wrote < sizeof(c_out)) {
         break; /* this block is used up */
      }
   }
   return (STREAMREAD_OK);
}

/* Read u_len chunk data bytes into the running CRC, inflating them when
 * they are IDAT data. */
static StreamReadStatus
_png_read_data(GInputStream *p_in, PngWalk *p_w, gsize u_len, gboolean b_idat,
               guint32 *pu_crc, GError **p_err) {
   guint8 c_buf[INTACT_BLOCK];
   while (u_len > 0) {
      gsize            u_n  = MIN(u_len, sizeof(c_buf));
      StreamReadStatus e_rd = streamread_exact(p_in, c_buf, u_n, p_err);
      if (e_rd == STREAMREAD_OK && b_idat) {
         e_rd = _idat_feed(p_w, c_buf, u_n, p_err);
      }
      if (e_rd != STREAMREAD_OK) {
         return (e_rd);
      }
      *pu_crc = _crc_update(*pu_crc, c_buf, u_n);
      u_len -= u_n;
   }
   return (STREAMREAD_OK);
}

/* IHDR's data (13 bytes, already CRC'd by the caller's read): the size for
 * the caller and the row plan. */
static StreamReadStatus
_png_take_ihdr(GInputStream *p_in, PngWalk *p_w, guint32 u_len, guint32 *pu_crc,
               GError **p_err) {
   guint8 c_ihdr[13];
   if (u_len != sizeof(c_ihdr)) {
      return (_png_corrupt("bad IHDR length", p_err));
   }
   StreamReadStatus e_rd = streamread_exact(p_in, c_ihdr, 13, p_err);
   if (e_rd != STREAMREAD_OK) {
      return (e_rd);
   }
   *pu_crc          = _crc_update(*pu_crc, c_ihdr, 13);
   p_w->b_ihdr      = TRUE;
   p_w->p_size->u_w = _be32(c_ihdr);
   p_w->p_size->u_h = _be32(c_ihdr + 4);
   return (_rows_start(&p_w->t_rows, c_ihdr) ? STREAMREAD_OK
                                             : _png_corrupt("bad IHDR", p_err));
}

/* One critical chunk (type c_type, u_len data bytes): data through the CRC
 * (IHDR parsed, IDAT inflated), then the stored CRC compared. */
static StreamReadStatus
_png_critical(GInputStream *p_in, PngWalk *p_w, const guint8 *c_type,
              guint32 u_len, GError **p_err) {
   guint32          u_crc = _crc_update(0xFFFFFFFFu, c_type, 4);
   gboolean         b_hdr = memcmp(c_type, "IHDR", 4) == 0;
   gboolean         b_dat = memcmp(c_type, "IDAT", 4) == 0;
   StreamReadStatus e_rd =
      b_hdr ? _png_take_ihdr(p_in, p_w, u_len, &u_crc, p_err)
            : _png_read_data(p_in, p_w, u_len, b_dat, &u_crc, p_err);
   guint8 c_crc[4];
   if (e_rd == STREAMREAD_OK) {
      e_rd = streamread_exact(p_in, c_crc, 4, p_err);
   }
   if (e_rd == STREAMREAD_OK && _be32(c_crc) != (u_crc ^ 0xFFFFFFFFu)) {
      return (_png_corrupt("CRC error in a critical chunk", p_err));
   }
   p_w->b_idat = p_w->b_idat || b_dat;
   return (e_rd);
}

/* The chunks after the signature, to IEND. Ancillary chunks (lower-case
 * first letter) are skipped unchecked: libpng only warns about those. */
static StreamReadStatus
_png_chunks(GInputStream *p_in, PngWalk *p_w, GError **p_err) {
   for (;;) {
      guint8           c_hdr[8];
      StreamReadStatus e_rd  = streamread_exact(p_in, c_hdr, 8, p_err);
      guint32          u_len = _be32(c_hdr);
      if (e_rd != STREAMREAD_OK) {
         return (e_rd);
      }
      if (u_len > 0x7FFFFFFFu) {
         return (_png_corrupt("chunk length out of range", p_err));
      }
      if (!p_w->b_ihdr && memcmp(c_hdr + 4, "IHDR", 4) != 0) {
         return (_png_corrupt("IHDR is not the first chunk", p_err));
      }
      if (c_hdr[4] & 0x20) { /* ancillary: data + CRC */
         e_rd = streamread_skip(p_in, (gsize)u_len + 4, p_err);
      } else {
         e_rd = _png_critical(p_in, p_w, c_hdr + 4, u_len, p_err);
      }
      if (e_rd != STREAMREAD_OK || memcmp(c_hdr + 4, "IEND", 4) == 0) {
         return (e_rd);
      }
   }
}

static StreamReadStatus
_png_check(GInputStream *p_in, IntactSize *p_size, GError **p_err) {
   guint8           c_sig[8];
   StreamReadStatus e_rd = streamread_exact(p_in, c_sig, sizeof(c_sig), p_err);
   if (e_rd != STREAMREAD_OK) {
      return (e_rd);
   }
   if (memcmp(c_sig, "\x89PNG\r\n\x1a\n", 8) != 0) {
      return (_png_corrupt("no PNG signature", p_err));
   }
   _crc_init();
   PngWalk t_w = {.p_size = p_size};
   t_w.p_dec =
      G_CONVERTER(g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB));
   e_rd = _png_chunks(p_in, &t_w, p_err);
   g_object_unref(t_w.p_dec);
   if (e_rd == STREAMREAD_OK && (!t_w.b_idat || !t_w.t_rows.b_done)) {
      return (_png_corrupt("image data ends short", p_err));
   }
   return (e_rd);
}

/* --- the JPEG walk -------------------------------------------------------- */

/* A frame header's code: SOF0-SOF15 minus DHT (C4), JPG (C8), DAC (CC). */
static gboolean
_jpeg_is_sof(guint8 u_code) {
   return (u_code >= 0xC0 && u_code <= 0xCF && u_code != 0xC4 &&
           u_code != 0xC8 && u_code != 0xCC);
}

/* One length-prefixed segment; a SOF's precision, height and width are
 * read on the way (the first SOF's size is the image's). */
static StreamReadStatus
_jpeg_segment(GInputStream *p_in, guint8 u_code, IntactSize *p_size,
              GError **p_err) {
   guint8           c_len[2];
   StreamReadStatus e_rd = streamread_exact(p_in, c_len, 2, p_err);
   if (e_rd != STREAMREAD_OK) {
      return (e_rd);
   }
   gsize u_len = ((gsize)c_len[0] << 8) | c_len[1];
   gsize u_sof = _jpeg_is_sof(u_code) ? 5 : 0;
   if (u_len < 2 + u_sof) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "JPEG segment length %" G_GSIZE_FORMAT " is invalid", u_len);
      return (STREAMREAD_ERROR);
   }
   if (u_sof > 0) {
      guint8 c_sof[5]; /* precision, height (2), width (2) */
      e_rd = streamread_exact(p_in, c_sof, sizeof(c_sof), p_err);
      if (e_rd != STREAMREAD_OK) {
         return (e_rd);
      }
      if (p_size->u_w == 0 && p_size->u_h == 0) {
         p_size->u_h = ((guint32)c_sof[1] << 8) | c_sof[2];
         p_size->u_w = ((guint32)c_sof[3] << 8) | c_sof[4];
      }
   }
   return (streamread_skip(p_in, u_len - 2 - u_sof, p_err));
}

/* Walk the marker segments after SOI up to the first SOS (0xDA). TEM,
 * RSTn and SOI carry no length; EOI before any SOS is a file with no
 * image at all, which counts as truncated for the caller's purpose. */
static StreamReadStatus
_jpeg_walk_to_sos(GInputStream *p_in, IntactSize *p_size, GError **p_err) {
   guint8           c_soi[2];
   StreamReadStatus e_rd = streamread_exact(p_in, c_soi, 2, p_err);
   while (e_rd == STREAMREAD_OK) {
      guint8 u_code = 0;
      e_rd          = streamread_jpeg_marker(p_in, &u_code, p_err);
      if (e_rd != STREAMREAD_OK || u_code == 0xDA) {
         return (e_rd);
      }
      if (u_code == 0xD9) {
         return (STREAMREAD_EOF);
      }
      if (u_code != 0x01 && (u_code < 0xD0 || u_code > 0xD8)) {
         e_rd = _jpeg_segment(p_in, u_code, p_size, p_err);
      }
   }
   return (e_rd);
}

/* Sequential scan of the rest of the stream for FF D9, carrying the last
 * byte across block boundaries. In entropy-coded data every 0xFF is byte-
 * stuffed (FF 00) or a marker, so FF D9 there can only be the EOI. */
static StreamReadStatus
_jpeg_find_eoi(GInputStream *p_in, GError **p_err) {
   guint8   c_buf[INTACT_BLOCK];
   gboolean b_ff = FALSE;
   for (;;) {
      gssize i_n = g_input_stream_read(p_in, c_buf, sizeof(c_buf), NULL, p_err);
      if (i_n < 0) {
         return (STREAMREAD_ERROR);
      }
      if (i_n == 0) {
         return (STREAMREAD_EOF);
      }
      for (gssize i = 0; i < i_n; i++) {
         if (b_ff && c_buf[i] == 0xD9) {
            return (STREAMREAD_OK);
         }
         b_ff = (c_buf[i] == 0xFF);
      }
   }
}

static StreamReadStatus
_jpeg_check(GInputStream *p_in, IntactSize *p_size, GError **p_err) {
   StreamReadStatus e_rd = _jpeg_walk_to_sos(p_in, p_size, p_err);
   if (e_rd != STREAMREAD_OK) {
      return (e_rd);
   }
   return (_jpeg_find_eoi(p_in, p_err));
}

/* --- entry points --------------------------------------------------------- */

typedef StreamReadStatus (*IntactWalk)(GInputStream *, IntactSize *, GError **);

/* Open p_file (buffered: the JPEG marker search reads a byte at a time)
 * and run fn_walk on it; a walk that ends in EOF is reported as truncated,
 * an error as itself. */
static gboolean
_check(GFile *p_file, const char *c_what, IntactWalk fn_walk,
       IntactSize *p_size, GError **p_err) {
   IntactSize  t_size          = {0, 0};
   IntactSize *p_out           = p_size != NULL ? p_size : &t_size;
   *p_out                      = t_size;
   GError           *p_local   = NULL;
   GFileInputStream *p_file_in = g_file_read(p_file, NULL, &p_local);
   if (p_file_in == NULL) {
      g_propagate_error(p_err, p_local);
      return (FALSE);
   }
   GInputStream *p_in = g_buffered_input_stream_new(G_INPUT_STREAM(p_file_in));
   StreamReadStatus e_rd = fn_walk(p_in, p_out, &p_local);
   g_input_stream_close(p_in, NULL, NULL);
   g_object_unref(p_in);
   g_object_unref(p_file_in);
   if (p_local != NULL) {
      g_propagate_error(p_err, p_local);
      return (FALSE);
   }
   return (e_rd == STREAMREAD_OK ? TRUE : _truncated(c_what, p_err));
}

gboolean
intact_png(GFile *p_file, IntactSize *p_size, GError **p_err) {
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   return (_check(p_file, "PNG", _png_check, p_size, p_err));
}

gboolean
intact_jpeg(GFile *p_file, IntactSize *p_size, GError **p_err) {
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   return (_check(p_file, "JPEG", _jpeg_check, p_size, p_err));
}

/* --- the libjpeg pass (intact_jpeg_decodes) ------------------------------ */

#if GGAZE_HAVE_JPEG

typedef struct {
   struct jpeg_error_mgr pub;
   jmp_buf               buf;
} IntactJerr;

static void
_jerr_exit(j_common_ptr p_cinfo) {
   longjmp(((IntactJerr *)p_cinfo->err)->buf, 1);
}

/* Warnings ("extraneous bytes", "premature end of data") are libjpeg
 * decoding on, which is all this pass asks; nothing to print. */
static void
_jerr_quiet(j_common_ptr p_cinfo) {
   (void)p_cinfo;
}

/* Decode all of p_fp at 1/8 scale into one reused row, under a longjmp
 * error handler. FALSE with libjpeg's message in c_msg (JMSG_LENGTH_MAX)
 * when libjpeg gives up. The scale changes only the IDCT and the row
 * size: libjpeg-turbo scales every component alike, so the markers, the
 * entropy decoding and the upsampling / colour-conversion setup -- where
 * its fatal errors come from -- are those of a full-size decode with the
 * same (default) output colour space GEGL's loader asks for. */
static gboolean
_jpeg_decode_all(FILE *p_fp, char *c_msg) {
   struct jpeg_decompress_struct cinfo;
   IntactJerr                    jerr;
   JSAMPLE *volatile p_row = NULL; /* read after a longjmp */
   cinfo.err               = jpeg_std_error(&jerr.pub);
   jerr.pub.error_exit     = _jerr_exit;
   jerr.pub.output_message = _jerr_quiet;
   if (setjmp(jerr.buf)) {
      (*cinfo.err->format_message)((j_common_ptr)&cinfo, c_msg);
      jpeg_destroy_decompress(&cinfo);
      g_free((gpointer)p_row);
      return (FALSE);
   }
   jpeg_create_decompress(&cinfo);
   jpeg_stdio_src(&cinfo, p_fp);
   jpeg_read_header(&cinfo, TRUE);
   cinfo.scale_num   = 1;
   cinfo.scale_denom = 8;
   jpeg_start_decompress(&cinfo);
   p_row = g_malloc((gsize)cinfo.output_width * cinfo.output_components);
   while (cinfo.output_scanline < cinfo.output_height) {
      JSAMPROW p_rows[1] = {p_row};
      jpeg_read_scanlines(&cinfo, p_rows, 1);
   }
   jpeg_finish_decompress(&cinfo);
   jpeg_destroy_decompress(&cinfo);
   g_free((gpointer)p_row);
   return (TRUE);
}

gboolean
intact_jpeg_decodes(GFile *p_file, GError **p_err) {
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   char *c_path = g_file_get_path(p_file);
   FILE *p_fp   = c_path != NULL ? g_fopen(c_path, "rb") : NULL;
   g_free(c_path);
   if (p_fp == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "JPEG cannot be opened for the libjpeg pass");
      return (FALSE);
   }
   char     c_msg[JMSG_LENGTH_MAX] = "";
   gboolean b_ok                   = _jpeg_decode_all(p_fp, c_msg);
   fclose(p_fp);
   if (!b_ok) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "JPEG is corrupt (libjpeg: %s)", c_msg);
   }
   return (b_ok);
}

#else

gboolean
intact_jpeg_decodes(GFile *p_file, GError **p_err) {
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "no libjpeg in this build to check the JPEG with");
   return (FALSE);
}

#endif /* GGAZE_HAVE_JPEG */
