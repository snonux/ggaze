/*:*
 * ggaze — image format detection
 *
 * Magic-byte sniffing. No GTK -> unit-testable. detect_format() and
 * detect_jpeg_peek_dims() are pure functions (no I/O) operating on bytes
 * already in memory. detect_jpeg_peek_dims_from_path() is the one exception:
 * it does a small bounded read (GGAZE_JPEG_PEEK_LEN) for call sites that
 * hand a path straight to a decoder rather than loading bytes themselves
 * (thumbnail.c, info.c). Because that read is capped, "no SOF found" is
 * ambiguous -- the real SOF may just be further in than the prefix reaches,
 * since a chain of maximal-length marker segments can push it arbitrarily
 * far -- so it returns a tri-state GgazeJpegPeekStatus rather than a plain
 * bool, and callers must fail closed (reject) on the inconclusive case
 * exactly like an oversized header; see detect.h. A prior version of this
 * function collapsed that ambiguity into a single FALSE that its callers
 * treated as "safe to proceed," which let a padded file bypass the size
 * guard entirely and reintroduce the ~28-30s GdkPixbuf stall this module
 * exists to prevent (mu0 review round 3). detect_jpeg_peek_dims()/
 * _from_path() plus detect_jpeg_dims_within_bounds() let every JPEG-decoding
 * call site (pixbuf.c, jpeg.c, thumbnail.c, info.c) reject an oversized
 * declared header before decoding, without duplicating the bound comparison
 * or error message (mu0).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "detect.h"

#include <gio/gio.h>
#include <string.h>

/* One magic-number rule: u_len bytes of p_magic at p_offset identify
 * e_format. Rules are tried in order; the first match wins. */
typedef struct {
   GgazeFormat   e_format;
   gsize         u_offset;
   gsize         u_len;
   const guint8 *p_magic;
} MagicRule;

static const guint8 MAGIC_JPEG[] = {0xFF, 0xD8, 0xFF};
static const guint8 MAGIC_PNG[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
static const guint8 MAGIC_GIF[] = {'G', 'I', 'F', '8'};
static const guint8 MAGIC_RIFF[]    = {'R', 'I', 'F', 'F'};
static const guint8 MAGIC_WEBP[]    = {'W', 'E', 'B', 'P'};
static const guint8 MAGIC_TIFF_LE[] = {'I', 'I', 0x2A, 0x00};
static const guint8 MAGIC_TIFF_BE[] = {'M', 'M', 0x00, 0x2A};
static const guint8 MAGIC_ICO[]     = {0x00, 0x00, 0x01, 0x00};
static const guint8 MAGIC_JXL_CS[]  = {0xFF, 0x0A};
static const guint8 MAGIC_JXL_BOX[] = {0x00, 0x00, 0x00, 0x0C,
                                       'J',  'X',  'L',  ' '};
static const guint8 MAGIC_FTYP[]    = {'f', 't', 'y', 'p'};
static const guint8 MAGIC_AVIF[]    = {'a', 'v', 'i', 'f'};
static const guint8 MAGIC_AVIS[]    = {'a', 'v', 'i', 's'};
static const guint8 MAGIC_HEIC[]    = {'h', 'e', 'i', 'c'};
static const guint8 MAGIC_HEIX[]    = {'h', 'e', 'i', 'x'};
static const guint8 MAGIC_MIF1[]    = {'m', 'i', 'f', '1'};

/* Single-magic formats (WebP and the ISO BMFF brands need two checks and are
 * handled after this table). */
static const MagicRule MAGIC_RULES[] = {
   {GGAZE_FMT_JPEG, 0, sizeof(MAGIC_JPEG), MAGIC_JPEG},
   {GGAZE_FMT_PNG, 0, sizeof(MAGIC_PNG), MAGIC_PNG},
   {GGAZE_FMT_GIF, 0, sizeof(MAGIC_GIF), MAGIC_GIF},
   {GGAZE_FMT_TIFF, 0, sizeof(MAGIC_TIFF_LE), MAGIC_TIFF_LE},
   {GGAZE_FMT_TIFF, 0, sizeof(MAGIC_TIFF_BE), MAGIC_TIFF_BE},
   {GGAZE_FMT_ICO, 0, sizeof(MAGIC_ICO), MAGIC_ICO},
   {GGAZE_FMT_JXL, 0, sizeof(MAGIC_JXL_CS), MAGIC_JXL_CS},
   {GGAZE_FMT_JXL, 0, sizeof(MAGIC_JXL_BOX), MAGIC_JXL_BOX},
};

static gboolean
_has_magic(const guint8 *p_head, gsize u_len, gsize u_offset,
           const guint8 *p_magic, gsize u_magic_len) {
   return (u_len >= u_offset + u_magic_len &&
           memcmp(p_head + u_offset, p_magic, u_magic_len) == 0);
}

/* ISO BMFF (AVIF / HEIF): an "ftyp" box at offset 4 and the brand at 8. */
static GgazeFormat
_detect_bmff(const guint8 *p_head, gsize u_len) {
   if (!_has_magic(p_head, u_len, 4, MAGIC_FTYP, 4)) {
      return (GGAZE_FMT_UNKNOWN);
   }
   if (_has_magic(p_head, u_len, 8, MAGIC_AVIF, 4) ||
       _has_magic(p_head, u_len, 8, MAGIC_AVIS, 4)) {
      return (GGAZE_FMT_AVIF);
   }
   if (_has_magic(p_head, u_len, 8, MAGIC_HEIC, 4) ||
       _has_magic(p_head, u_len, 8, MAGIC_HEIX, 4) ||
       _has_magic(p_head, u_len, 8, MAGIC_MIF1, 4)) {
      return (GGAZE_FMT_HEIF);
   }
   return (GGAZE_FMT_UNKNOWN);
}

GgazeFormat
detect_format(const guint8 *p_head, gsize u_len) {
   if (p_head == NULL || u_len == 0) {
      return (GGAZE_FMT_UNKNOWN);
   }
   for (gsize u = 0; u < G_N_ELEMENTS(MAGIC_RULES); u++) {
      const MagicRule *p_r = &MAGIC_RULES[u];
      if (_has_magic(p_head, u_len, p_r->u_offset, p_r->p_magic, p_r->u_len)) {
         return (p_r->e_format);
      }
   }
   /* WebP: RIFF .... WEBP */
   if (_has_magic(p_head, u_len, 0, MAGIC_RIFF, 4) &&
       _has_magic(p_head, u_len, 8, MAGIC_WEBP, 4)) {
      return (GGAZE_FMT_WEBP);
   }
   return (_detect_bmff(p_head, u_len));
}

/* JPEG marker codes relevant to the SOF scan below. SOF0-SOF15 span
 * 0xC0-0xCF, but three codes in that range are reused for non-frame
 * segments and must be skipped rather than misread as a frame header. */
#define GGAZE_JPEG_MARKER_DHT 0xC4
#define GGAZE_JPEG_MARKER_JPG 0xC8
#define GGAZE_JPEG_MARKER_DAC 0xCC
#define GGAZE_JPEG_MARKER_SOS 0xDA

static gboolean
_is_sof_marker(guint8 u_marker) {
   return (u_marker >= 0xC0 && u_marker <= 0xCF &&
           u_marker != GGAZE_JPEG_MARKER_DHT &&
           u_marker != GGAZE_JPEG_MARKER_JPG &&
           u_marker != GGAZE_JPEG_MARKER_DAC);
}

/* Read a SOF segment's height/width at u_i (the segment's marker byte
 * offset) into *p_w and *p_h. Caller has already verified u_i+9 <= u_len, so no
 * further bounds check is needed: length(2) precision(1) height(2)
 * width(2), starting right after the 2-byte marker. */
static void
_read_sof_dims(const guint8 *p_buf, gsize u_i, guint32 *p_w, guint32 *p_h) {
   *p_h = ((guint32)p_buf[u_i + 5] << 8) | p_buf[u_i + 6];
   *p_w = ((guint32)p_buf[u_i + 7] << 8) | p_buf[u_i + 8];
}

/* If the marker at u_i is a bare fill byte (0xFF) or a segment-less marker
 * (TEM 0x01, RSTn/SOI/EOI 0xD0-0xD9), advance *p_next_i past just the marker
 * pair (or single fill byte) and return TRUE so _jpeg_peek_step() re-enters
 * the loop without trying to read a length; otherwise leave *p_next_i
 * untouched and return FALSE so the caller reads u_i as a length-bearing
 * segment instead. Split out of _jpeg_peek_step() to keep it under ~30
 * lines (mu0 review round 4). */
static gboolean
_jpeg_peek_skip_fill(guint8 u_marker, gsize u_i, gsize *p_next_i) {
   if (u_marker == 0xFF) {
      *p_next_i = u_i + 1;
      return (TRUE);
   }
   if (u_marker == 0x01 || (u_marker >= 0xD0 && u_marker <= 0xD9)) {
      *p_next_i = u_i + 2;
      return (TRUE);
   }
   return (FALSE);
}

/* Handle a SOF marker segment already located at u_i (its marker byte
 * offset): if its 9-byte payload fits within u_len, read the declared
 * width/height and report OK; otherwise the payload is truncated, which is
 * either a malformed file (b_capped == FALSE, u_len is the whole file) or an
 * inconclusive split-across-the-prefix-boundary case (b_capped == TRUE).
 * Split out of _jpeg_peek_step() to keep it under ~30 lines (mu0 review
 * round 4). */
static GgazeJpegPeekStatus
_jpeg_peek_sof(const guint8 *p_buf, gsize u_len, gsize u_i, gboolean b_capped,
               guint32 *p_w, guint32 *p_h) {
   if (u_i + 9 > u_len) {
      return (b_capped ? GGAZE_JPEG_PEEK_INCONCLUSIVE
                       : GGAZE_JPEG_PEEK_NOT_JPEG);
   }
   _read_sof_dims(p_buf, u_i, p_w, p_h);
   return (GGAZE_JPEG_PEEK_OK);
}

/* One iteration of the marker scan in _jpeg_peek_scan(): inspect the marker
 * segment at *p_i. Returns TRUE and advances *p_i past it if the scan
 * should keep going, or returns FALSE with a final status written to
 * *p_status if this segment ends the scan (malformed length, SOF found,
 * SOS with no SOF seen, or a truncated SOF payload -- see _jpeg_peek_sof()).
 * Split out of _jpeg_peek_scan() to keep both functions under ~30 lines
 * (mu0 review round 4). */
static gboolean
_jpeg_peek_step(const guint8 *p_buf, gsize u_len, gsize *p_i, gboolean b_capped,
                guint32 *p_w, guint32 *p_h, GgazeJpegPeekStatus *p_status) {
   gsize u_i = *p_i;
   if (p_buf[u_i] != 0xFF) {
      *p_i = u_i + 1; /* resync on a stray non-marker byte */
      return (TRUE);
   }
   guint8 u_marker = p_buf[u_i + 1];
   if (_jpeg_peek_skip_fill(u_marker, u_i, p_i)) {
      return (TRUE);
   }
   guint32 u_seglen = ((guint32)p_buf[u_i + 2] << 8) | p_buf[u_i + 3];
   if (u_seglen < 2) {
      *p_status = GGAZE_JPEG_PEEK_NOT_JPEG; /* malformed: length must include
                                             * itself */
      return (FALSE);
   }
   if (_is_sof_marker(u_marker)) {
      *p_status = _jpeg_peek_sof(p_buf, u_len, u_i, b_capped, p_w, p_h);
      return (FALSE);
   }
   if (u_marker == GGAZE_JPEG_MARKER_SOS) {
      *p_status = GGAZE_JPEG_PEEK_NOT_JPEG; /* scan data reached, no SOF seen
                                             * first */
      return (FALSE);
   }
   *p_i = u_i + 2 + (gsize)u_seglen;
   return (TRUE);
}

/* Shared SOF-marker scan behind both detect_jpeg_peek_dims() and
 * detect_jpeg_peek_dims_from_path(). b_capped tells it whether p_buf/u_len is
 * known to hold the *entire* file (b_capped == FALSE: running out of bytes
 * means there is genuinely nothing more to find, i.e. GGAZE_JPEG_PEEK_NOT_
 * JPEG) or only a possibly-truncated prefix of a larger file (b_capped ==
 * TRUE: running out of bytes means the answer is simply unknown, i.e.
 * GGAZE_JPEG_PEEK_INCONCLUSIVE). Every other exit (bad SOI, malformed
 * segment length, SOS reached with no SOF) is a definitive answer regardless
 * of b_capped, since the scan is sequential from the SOI and those
 * conditions cannot hide a SOF that has not been seen yet. The per-segment
 * work lives in _jpeg_peek_step(); this loop just drives it. */
static GgazeJpegPeekStatus
_jpeg_peek_scan(const guint8 *p_buf, gsize u_len, gboolean b_capped,
                guint32 *p_w, guint32 *p_h) {
   if (p_buf == NULL || u_len < 4 || p_buf[0] != 0xFF || p_buf[1] != 0xD8) {
      return (GGAZE_JPEG_PEEK_NOT_JPEG);
   }
   gsize               u_i      = 2;
   GgazeJpegPeekStatus e_status = GGAZE_JPEG_PEEK_NOT_JPEG;
   while (u_i + 4 <= u_len) {
      if (!_jpeg_peek_step(p_buf, u_len, &u_i, b_capped, p_w, p_h, &e_status)) {
         return (e_status);
      }
   }
   /* Ran off the end of p_buf without a terminal condition from
    * _jpeg_peek_step(): same reasoning as its truncated-SOF case above. */
   return (b_capped ? GGAZE_JPEG_PEEK_INCONCLUSIVE : GGAZE_JPEG_PEEK_NOT_JPEG);
}

gboolean
detect_jpeg_peek_dims(const guint8 *p_buf, gsize u_len, guint32 *p_w,
                      guint32 *p_h) {
   /* b_capped=FALSE: p_buf/u_len is the whole file, so _jpeg_peek_scan()
    * never returns GGAZE_JPEG_PEEK_INCONCLUSIVE here -- only OK or NOT_JPEG,
    * which map onto this function's plain TRUE/FALSE contract unchanged. */
   return (_jpeg_peek_scan(p_buf, u_len, FALSE, p_w, p_h) ==
           GGAZE_JPEG_PEEK_OK);
}

GgazeJpegPeekStatus
detect_jpeg_peek_dims_from_path(const char *c_path, guint32 *p_w,
                                guint32 *p_h) {
   if (c_path == NULL) {
      return (GGAZE_JPEG_PEEK_NOT_JPEG);
   }
   GFile            *p_file = g_file_new_for_path(c_path);
   GFileInputStream *p_in   = g_file_read(p_file, NULL, NULL);
   g_object_unref(p_file);
   if (p_in == NULL) {
      return (GGAZE_JPEG_PEEK_NOT_JPEG); /* unreadable: let the real decoder
                                          * produce its own error */
   }
   guint8 buf[GGAZE_JPEG_PEEK_LEN];
   gssize n =
      g_input_stream_read(G_INPUT_STREAM(p_in), buf, sizeof(buf), NULL, NULL);
   g_object_unref(p_in);
   if (n <= 0) {
      return (GGAZE_JPEG_PEEK_NOT_JPEG);
   }
   /* b_capped: TRUE iff we filled the whole read buffer, meaning the file may
    * continue past it and a "no SOF found" verdict would be inconclusive,
    * not definitive. A short read (n < sizeof(buf)) hit real EOF, so the
    * whole file was seen. */
   gboolean b_capped = ((gsize)n == sizeof(buf));
   return (_jpeg_peek_scan(buf, (gsize)n, b_capped, p_w, p_h));
}

gboolean
detect_dims_within_bounds(const char *c_backend, guint64 u_w, guint64 u_h,
                          gsize *p_rgba_len, GError **p_err) {
   if (u_w == 0 || u_h == 0) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "%s: zero-sized image (%" G_GUINT64_FORMAT
                  "x%" G_GUINT64_FORMAT ")",
                  c_backend, u_w, u_h);
      return (FALSE);
   }
   if (u_w > GGAZE_IMAGE_MAX_SIDE || u_h > GGAZE_IMAGE_MAX_SIDE ||
       u_w * u_h > GGAZE_IMAGE_MAX_PIXELS) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "%s: image too large (%" G_GUINT64_FORMAT
                  "x%" G_GUINT64_FORMAT ", max %d per side / %llu pixels)",
                  c_backend, u_w, u_h, GGAZE_IMAGE_MAX_SIDE,
                  (unsigned long long)GGAZE_IMAGE_MAX_PIXELS);
      return (FALSE);
   }
   /* Both sides are <= 32768 here, so w*h*4 is at most ~4e9 * 4 and fits a
    * 64-bit gsize; the gint and gsize checks are kept for 32-bit targets. */
   if (u_w > (guint64)G_MAXINT || u_h > (guint64)G_MAXINT) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "%s: dimensions exceed gint range", c_backend);
      return (FALSE);
   }
   guint64 u_bytes = u_w * u_h * 4u;
   if (u_bytes > (guint64)G_MAXSIZE) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "%s: buffer size overflows gsize", c_backend);
      return (FALSE);
   }
   if (p_rgba_len != NULL) {
      *p_rgba_len = (gsize)u_bytes;
   }
   return (TRUE);
}

gboolean
detect_jpeg_dims_within_bounds(guint32 u_w, guint32 u_h, GError **p_err) {
   /* A declared height of 0 is legal JPEG (a DNL marker supplies it later),
    * so a zero side is not "oversized": let the real decoder judge it. */
   if (u_w == 0 || u_h == 0) {
      return (TRUE);
   }
   return (detect_dims_within_bounds("jpeg", u_w, u_h, NULL, p_err));
}