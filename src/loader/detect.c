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
 * The same idea closes a second stall (task tb2): every magic-number rule
 * carries the smallest complete file its format allows, and
 * detect_reject_truncated() lets the loader refuse a file shorter than that
 * before any decoder sees it. On a glycin desktop (Fedora >= 41) gdk-pixbuf
 * forwards unknown-to-it formats to sandboxed loader subprocesses, and the
 * JXL one was measured to wait forever on a truncated codestream, with no
 * cancellable or timeout the caller could apply; the minimum-length gate is
 * the one bound available from this side.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "detect.h"

#include <gio/gio.h>
#include <string.h>

/* Smallest complete file per signature (see detect_min_file_len() in
 * detect.h for the contract: signature plus the fixed-size mandatory header
 * structure, never entropy data, optional parts or trailers, so these are
 * under-estimates a valid file can never fall below). Each must stay <=
 * GGAZE_DETECT_SNIFF_LEN; test_detect.c asserts that. */
enum {
   /* SOI (2) + smallest frame header (marker 2, length 2, precision 1,
    * height 2, width 2, Nf 1, one component 3 = 13) + smallest scan header
    * (marker 2, length 2, Ns 1, one component 2, Ss/Se/AhAl 3 = 10). EOI
    * is not counted: a camera-truncated file lacking it still decodes. */
   MIN_LEN_JPEG = 25,
   /* 8-byte signature + IHDR chunk (length 4, type 4, data 13, CRC 4). */
   MIN_LEN_PNG = 33,
   /* 6-byte header + 7-byte logical screen descriptor. */
   MIN_LEN_GIF = 13,
   /* 12-byte RIFF/WEBP header + the first chunk's 8-byte header. */
   MIN_LEN_WEBP = 20,
   /* 8-byte header + 2-byte entry count + one 12-byte IFD entry. */
   MIN_LEN_TIFF = 22,
   /* 6-byte ICONDIR + one 16-byte ICONDIRENTRY. */
   MIN_LEN_ICO = 22,
   /* 2-byte signature + the bit-packed SizeHeader / ImageMetadata / frame
    * header / TOC (never under 4 bytes together) + at least 2 bytes of
    * group data. The smallest valid codestream known (jxl art) is 12
    * bytes; 8 leaves margin below it. */
   MIN_LEN_JXL_CODESTREAM = 8,
   /* 12-byte signature box + 16-byte ftyp box (size, type, major brand,
    * minor version; the mandatory "jxl " compatible brand would add 4) +
    * 8-byte jxlc box header + MIN_LEN_JXL_CODESTREAM of codestream. */
   MIN_LEN_JXL_CONTAINER = 44,
   /* 16-byte ftyp box + 12-byte meta FullBox header + 8-byte mdat box
    * header; the mandatory meta children (hdlr, pitm, iloc, iinf, iprp)
    * add far more in any real file. Shared by AVIF and HEIF. */
   MIN_LEN_BMFF = 36,
};

/* One magic-number rule: u_len bytes of p_magic at p_offset identify
 * e_format, whose smallest complete file is u_min_len bytes. Rules are
 * tried in order; the first match wins. */
typedef struct {
   GgazeFormat   e_format;
   gsize         u_offset;
   gsize         u_len;
   const guint8 *p_magic;
   gsize         u_min_len;
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
   {GGAZE_FMT_JPEG, 0, sizeof(MAGIC_JPEG), MAGIC_JPEG, MIN_LEN_JPEG},
   {GGAZE_FMT_PNG, 0, sizeof(MAGIC_PNG), MAGIC_PNG, MIN_LEN_PNG},
   {GGAZE_FMT_GIF, 0, sizeof(MAGIC_GIF), MAGIC_GIF, MIN_LEN_GIF},
   {GGAZE_FMT_TIFF, 0, sizeof(MAGIC_TIFF_LE), MAGIC_TIFF_LE, MIN_LEN_TIFF},
   {GGAZE_FMT_TIFF, 0, sizeof(MAGIC_TIFF_BE), MAGIC_TIFF_BE, MIN_LEN_TIFF},
   {GGAZE_FMT_ICO, 0, sizeof(MAGIC_ICO), MAGIC_ICO, MIN_LEN_ICO},
   {GGAZE_FMT_JXL, 0, sizeof(MAGIC_JXL_CS), MAGIC_JXL_CS,
    MIN_LEN_JXL_CODESTREAM},
   {GGAZE_FMT_JXL, 0, sizeof(MAGIC_JXL_BOX), MAGIC_JXL_BOX,
    MIN_LEN_JXL_CONTAINER},
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

/* The one sniff behind detect_format() and detect_min_file_len(): returns
 * the format and stores the matched signature's minimum complete-file
 * length in *p_min_len (0 when unrecognised). Kept together so a signature
 * can never gain a format without a minimum, or vice versa. */
static GgazeFormat
_sniff(const guint8 *p_head, gsize u_len, gsize *p_min_len) {
   *p_min_len = 0;
   if (p_head == NULL || u_len == 0) {
      return (GGAZE_FMT_UNKNOWN);
   }
   for (gsize u = 0; u < G_N_ELEMENTS(MAGIC_RULES); u++) {
      const MagicRule *p_r = &MAGIC_RULES[u];
      if (_has_magic(p_head, u_len, p_r->u_offset, p_r->p_magic, p_r->u_len)) {
         *p_min_len = p_r->u_min_len;
         return (p_r->e_format);
      }
   }
   /* WebP: RIFF .... WEBP */
   if (_has_magic(p_head, u_len, 0, MAGIC_RIFF, 4) &&
       _has_magic(p_head, u_len, 8, MAGIC_WEBP, 4)) {
      *p_min_len = MIN_LEN_WEBP;
      return (GGAZE_FMT_WEBP);
   }
   GgazeFormat e_bmff = _detect_bmff(p_head, u_len);
   if (e_bmff != GGAZE_FMT_UNKNOWN) {
      *p_min_len = MIN_LEN_BMFF;
   }
   return (e_bmff);
}

GgazeFormat
detect_format(const guint8 *p_head, gsize u_len) {
   gsize u_min_len;
   return (_sniff(p_head, u_len, &u_min_len));
}

const char *
detect_format_name(GgazeFormat e_format) {
   switch (e_format) {
   case GGAZE_FMT_JPEG:
      return ("JPEG");
   case GGAZE_FMT_PNG:
      return ("PNG");
   case GGAZE_FMT_GIF:
      return ("GIF");
   case GGAZE_FMT_WEBP:
      return ("WebP");
   case GGAZE_FMT_TIFF:
      return ("TIFF");
   case GGAZE_FMT_ICO:
      return ("ICO");
   case GGAZE_FMT_JXL:
      return ("JXL");
   case GGAZE_FMT_AVIF:
      return ("AVIF");
   case GGAZE_FMT_HEIF:
      return ("HEIF");
   case GGAZE_FMT_UNKNOWN:
   default:
      return ("unknown");
   }
}

gsize
detect_min_file_len(const guint8 *p_head, gsize u_len) {
   gsize u_min_len;
   (void)_sniff(p_head, u_len, &u_min_len);
   return (u_min_len);
}

gboolean
detect_reject_truncated(const guint8 *p_head, gsize u_len, GError **p_err) {
   gsize       u_min_len;
   GgazeFormat e_format = _sniff(p_head, u_len, &u_min_len);
   /* u_len is the whole file or GGAZE_DETECT_SNIFF_LEN bytes of it, and
    * every minimum is <= GGAZE_DETECT_SNIFF_LEN, so u_len < u_min_len can
    * only mean the file itself is too short. */
   if (u_len >= u_min_len) {
      return (TRUE);
   }
   g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "truncated %s file: %" G_GSIZE_FORMAT
               " bytes, the smallest complete %s file is %" G_GSIZE_FORMAT,
               detect_format_name(e_format), u_len,
               detect_format_name(e_format), u_min_len);
   return (FALSE);
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
   gsize  u_read = 0;
   /* g_input_stream_read_all(), not a single read: a single read may return
    * fewer bytes than the file holds on a FIFO, a pipe or a GVFS stream
    * (loader.c's _read_header() has the same note), and a short single read
    * would then be mistaken for the whole file below. With read_all only
    * EOF can end the read early, which is what makes "u_read < sizeof(buf)"
    * mean "the whole file was seen". A failed read is NOT_JPEG like an
    * unreadable file: the real decoder produces its own error. */
   gboolean b_ok = g_input_stream_read_all(G_INPUT_STREAM(p_in), buf,
                                           sizeof(buf), &u_read, NULL, NULL);
   g_object_unref(p_in);
   if (!b_ok || u_read == 0) {
      return (GGAZE_JPEG_PEEK_NOT_JPEG);
   }
   /* b_capped: TRUE iff the whole buffer filled, meaning the file may
    * continue past it and a "no SOF found" verdict would be inconclusive,
    * not definitive. Anything less hit EOF (see above), so the whole file
    * was seen. */
   gboolean b_capped = (u_read == sizeof(buf));
   return (_jpeg_peek_scan(buf, u_read, b_capped, p_w, p_h));
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