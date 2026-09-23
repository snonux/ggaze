#ifndef GGAZE_ICC_H
#define GGAZE_ICC_H

/*:*
 * ggaze — embedded ICC profile extraction (plain C: no GEGL, no GTK)
 *
 * Reads the ICC profile a PNG (its iCCP chunk) or a JPEG (its APP2
 * "ICC_PROFILE" segments) embeds, and the profile's own description, so
 * the info card can name a file's colour space in EVERY build -- the
 * minimal (GEGL-off) lane included -- and the tests can check which
 * profile an export carries. No pixel is decoded: only the
 * header segments ahead of the pixel data are walked, on a stream, so a
 * 46 MB photo costs a few KiB of reads. No babl / LCMS either: the
 * description comes from the profile's 'desc' tag (v2 ASCII text or v4
 * multi-localised Unicode), parsed here.
 *
 * Applying a profile is not this module's job. With GEGL enabled the
 * enhance / export path decodes through gegl:png-load / gegl:jpg-load,
 * which tag the buffer's babl space from these same bytes (enhancer.c,
 * decision #45); without GEGL nothing is applied and the card says so.
 * Formats other than PNG and JPEG are not searched: JXL / AVIF / HEIF /
 * WebP files report no profile, and icc_container_searched() lets the
 * info card say "not read" for them instead of claiming sRGB.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

/* Largest embedded profile accepted. Matrix/TRC profiles are a few KiB and
 * even a CLUT printer profile stays well under this; a header declaring
 * more is a lie (or a bomb) and is refused BEFORE any allocation. */
#define ICC_MAX_PROFILE_LEN (16u * 1024u * 1024u)

/* The ICC profile embedded in p_file: a PNG's inflated iCCP payload, or a
 * JPEG's APP2 ICC_PROFILE segments reassembled in sequence order. Returns
 * a new GBytes (caller unrefs) or NULL. NULL WITHOUT an error means the
 * file carries no profile (or is not a PNG / JPEG); NULL WITH
 * G_IO_ERROR_INVALID_DATA means a profile container is there but broken
 * (an iCCP that does not inflate, a segment past the file's end, missing
 * or duplicate sequence numbers, a length over ICC_MAX_PROFILE_LEN); other
 * GIO errors are I/O failures (NOT_FOUND, ...). The bytes are NOT
 * validated as a profile: see icc_is_profile(). */
GBytes *icc_read_embedded(GFile *p_file, GError **p_err);

/* TRUE iff p_file starts with a signature icc_read_embedded() searches (a
 * JPEG SOI or the PNG signature), so a caller can tell "no profile in this
 * PNG / JPEG" from "a format this module does not look into" -- the info
 * card must not call a WebP / AVIF / HEIF / JXL sRGB just because it was
 * not searched. FALSE for any other format, a shorter file, or a read
 * failure. */
gboolean icc_container_searched(GFile *p_file);

/* The same walk over bytes already in memory (a whole file or its head;
 * the walk stops at the pixel data and never needs more). */
GBytes *icc_extract(const guint8 *p_data, gsize u_len, GError **p_err);

/* TRUE iff p_icc starts with a plausible ICC profile header: the 'acsp'
 * signature and a declared size that covers the header and fits the data.
 * FALSE for NULL. What icc_read_embedded() returns for a PNG whose iCCP
 * inflates to garbage fails this, and the info card then says the profile
 * is unreadable rather than naming nothing. */
gboolean icc_is_profile(GBytes *p_icc);

/* Bounds icc_profile_is_sane() holds a profile to: the tag table's
 * length (real profiles carry a few dozen tags) and the points of a 'curv'
 * tone curve. Real curves stop at 4096 points (Adobe's and Argyll's
 * largest), and babl needs the bound: it writes every new RGB space's
 * profile, its curves at 12 + 2 * points bytes each, into a 64 KiB stack
 * buffer and copies out whatever it wrote past that (icc.c has the
 * arithmetic; a 65536-point curve crashed it). A profile with a longer
 * curve is declined, so its file shows unmanaged, as before xb2. */
#define ICC_MAX_TAGS 1024u
#define ICC_MAX_CURVE_POINTS 4096u

/* TRUE iff p_icc is a profile babl_space_from_icc() can be handed: a
 * plausible header (icc_is_profile) whose size field IS the byte count, a
 * tag table of at most ICC_MAX_TAGS entries inside the profile, every tag
 * after the table and inside the profile (at least 8 bytes), and every tag
 * babl reads (r/g/b/kTRC, r/g/bXYZ, wtpt, chrm, chad) of the type and
 * size babl reads it as -- a 'curv' with 12 + 2 * count bytes (count <=
 * ICC_MAX_CURVE_POINTS), a 'para' with a zero reserved word, function
 * type 0, 3 or 4 and all its parameters, the piecewise types' break
 * points inside babl's assertion (0 <= d, c * d < 0.998), an XYZ tag of 20
 * bytes or more. babl checks none of that itself: a huge 'curv' count, a
 * 'para' with a nonzero reserved byte or a curve past 4096 points crashes
 * it, a break point outside [0, 0.998) aborts it (assert), and some counts
 * exit the process (babl_fatal). FALSE for NULL. Says nothing about CLUT
 * tags (A2B0, ...), which only LCMS reads, on babl's CMYK path: see
 * enhancer.c. */
gboolean icc_profile_is_sane(GBytes *p_icc);

/* What babl_space_from_icc() would make of a profile: a space of one of
 * these kinds, or nothing. */
typedef enum {
   ICC_BABL_NONE = 0, /* babl declines it (or icc_profile_is_sane does) */
   ICC_BABL_RGB,      /* a matrix / TRC RGB space */
   ICC_BABL_GRAY,     /* a grey TRC space */
   ICC_BABL_CMYK,     /* handed to babl's LCMS path */
} IccBablKind;

/* The kind of space babl_space_from_icc() (BABL_ICC_INTENT_DEFAULT) makes
 * of p_icc, decided from its header and tag table alone: ICC_BABL_CMYK for
 * any CMYK profile; ICC_BABL_RGB / ICC_BABL_GRAY for a display or input
 * ('mntr' / 'scnr') RGB / grey profile with an XYZ PCS and not both A2B0
 * and B2A0, carrying its tone curves (r/g/bTRC, kTRC) and, for RGB, its
 * primaries (r/g/bXYZ and wtpt -- red's Z not above its X when a CLUT sits
 * beside them, Argyll's swapped matrix -- or a 3-channel, phosphor-0
 * 'chrm' and wtpt). ICC_BABL_NONE for any other profile, for which babl
 * would return no space, and for one icc_profile_is_sane() refuses. A
 * caller that asks babl only about the other kinds keeps babl's tables
 * from growing for a profile it would decline anyway (babl keeps the
 * curves it parsed before some of these checks). */
IccBablKind icc_babl_kind(GBytes *p_icc);

/* How many of the tone-curve tags babl parses from ANY profile it gets
 * past its early checks -- rTRC, gTRC, bTRC and kTRC, whatever the colour
 * space -- p_icc carries (0-4; 0 for NULL or not a profile). Each can add
 * a curve to babl's table even when the space it builds uses none of it
 * (a kTRC on an RGB profile). */
guint icc_babl_curve_tags(GBytes *p_icc);

/* The profile's description ('desc' tag) as UTF-8, caller frees: the ASCII
 * text of a v2 textDescriptionType or, for a v4 multiLocalizedUnicodeType,
 * the English record when there is one and the first record otherwise.
 * NULL when the bytes are not a profile, the tag is absent, or its
 * offsets / lengths point outside the data (every read is bounds-checked
 * against the actual byte count, never the declared one). */
char *icc_description(GBytes *p_icc);

G_END_DECLS

#endif /* GGAZE_ICC_H */
