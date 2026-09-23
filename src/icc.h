#ifndef GGAZE_ICC_H
#define GGAZE_ICC_H

/*:*
 * ggaze — embedded ICC profile extraction (plain C: no GEGL, no GTK)
 *
 * Reads the ICC profile a PNG (its iCCP chunk) or a JPEG (its APP2
 * "ICC_PROFILE" segments) embeds, and the profile's own description, so
 * the info card can name a file's colour space in EVERY build -- the
 * minimal (GEGL-off) lane included -- and the tests can check that an
 * export kept the profile byte for byte. No pixel is decoded: only the
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
 * WebP files report no profile, which is exactly what the viewer assumes
 * of them (sRGB).
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

/* The same walk over bytes already in memory (a whole file or its head;
 * the walk stops at the pixel data and never needs more). */
GBytes *icc_extract(const guint8 *p_data, gsize u_len, GError **p_err);

/* TRUE iff p_icc starts with a plausible ICC profile header: the 'acsp'
 * signature and a declared size that covers the header and fits the data.
 * FALSE for NULL. What icc_read_embedded() returns for a PNG whose iCCP
 * inflates to garbage fails this, and the info card then says the profile
 * is unreadable rather than naming nothing. */
gboolean icc_is_profile(GBytes *p_icc);

/* The profile's description ('desc' tag) as UTF-8, caller frees: the ASCII
 * text of a v2 textDescriptionType or, for a v4 multiLocalizedUnicodeType,
 * the English record when there is one and the first record otherwise.
 * NULL when the bytes are not a profile, the tag is absent, or its
 * offsets / lengths point outside the data (every read is bounds-checked
 * against the actual byte count, never the declared one). */
char *icc_description(GBytes *p_icc);

G_END_DECLS

#endif /* GGAZE_ICC_H */
