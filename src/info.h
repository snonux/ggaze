#ifndef GGAZE_INFO_H
#define GGAZE_INFO_H

/*:*
 * ggaze — image info / EXIF gather
 *
 * Gathers file info (dimensions, format, size) + EXIF (camera, lens, focal,
 * aperture, shutter, ISO, datetime, orientation) + the colour space the
 * file declares (its embedded ICC profile's name, via icc.c) into a
 * GgazeInfo struct. Plain-C, no GtkWidget; uses the loader (dims), libexif
 * (EXIF) and icc.c (profile). Unit-testable.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

/* What the file declares about its colour space (decision #45). The card
 * wording per state is info_format()'s; only an embedded profile carries a
 * name. */
typedef enum {
   GGAZE_ICC_NONE = 0,   /* no embedded profile: sRGB is assumed */
   GGAZE_ICC_EMBEDDED,   /* a profile is embedded; c_colorspace names it */
   GGAZE_ICC_UNREADABLE, /* a profile is there but broken (or not one) */
   GGAZE_ICC_UNINSPECTED /* a format icc.c does not search (not PNG/JPEG) */
} GgazeIccState;

typedef struct {
   int           i_width;
   int           i_height;
   char         *c_format;      /* content type (owned) */
   gint64        i_size;        /* file size in bytes */
   char         *c_camera;      /* Make + Model (owned) */
   char         *c_lens;        /* Lens model (owned, or NULL) */
   char         *c_focal;       /* FocalLength (owned) */
   char         *c_aperture;    /* FNumber (owned) */
   char         *c_shutter;     /* ExposureTime (owned) */
   char         *c_iso;         /* ISOSpeedRatings (owned) */
   char         *c_datetime;    /* DateTimeOriginal (owned) */
   int           i_orientation; /* EXIF Orientation (1-8, 0 if none) */
   GgazeIccState e_icc;         /* embedded-profile state, see above */
   char         *c_colorspace; /* the embedded profile's description (owned), or
                                * NULL unless e_icc is GGAZE_ICC_EMBEDDED */
   gboolean b_icc_managed;     /* the enhance path will apply that profile in
                                * this build. info_new() leaves it FALSE: only a
                                * caller that can ask the enhancer (GEGL builds:
                                * enhancer_would_manage, from info-overlay.c's
                                * worker) sets it; info_format() then adds
                                * "managed on enhance/export" to the line */
} GgazeInfo;

/* Gather info for p_file (synchronous). Returns a new GgazeInfo (caller owns,
 * free with info_delete) or NULL on failure. */
GgazeInfo *info_new(GFile *p_file);
void       info_delete(GgazeInfo *p_info);

/* Longest profile description the card shows, in characters; a longer
 * one is cut there with an ellipsis. */
#define INFO_ICC_DESC_MAX 64

/* Format the info as a multi-line string (caller frees). An embedded
 * profile's description is shown on one line: control characters become
 * spaces and it is capped at INFO_ICC_DESC_MAX characters. */
char *info_format(const GgazeInfo *p_info);

/* The EXIF Orientation (1-8) of the file at c_path, or 0 when it has none
 * or cannot be read -- the same validated read info_new() makes, on its own
 * for a caller that needs nothing else: the enhancer's GEGL decode path,
 * whose loaders do not auto-rotate (decision #26 says the picture must be
 * upright regardless). */
int info_exif_orientation(const char *c_path);

G_END_DECLS

#endif /* GGAZE_INFO_H */