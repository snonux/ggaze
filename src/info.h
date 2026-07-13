#ifndef GGAZE_INFO_H
#define GGAZE_INFO_H

/*:*
 * ggaze — image info / EXIF gather
 *
 * Gathers file info (dimensions, format, size) + EXIF (camera, lens, focal,
 * aperture, shutter, ISO, datetime, orientation) into a GgazeInfo struct.
 * Plain-C, no GtkWidget; uses GdkPixbuf (dims) + libexif (EXIF). Unit-testable.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
   int    i_width;
   int    i_height;
   char  *c_format;      /* content type (owned) */
   gint64 i_size;        /* file size in bytes */
   char  *c_camera;      /* Make + Model (owned) */
   char  *c_lens;        /* Lens model (owned, or NULL) */
   char  *c_focal;       /* FocalLength (owned) */
   char  *c_aperture;    /* FNumber (owned) */
   char  *c_shutter;     /* ExposureTime (owned) */
   char  *c_iso;         /* ISOSpeedRatings (owned) */
   char  *c_datetime;    /* DateTimeOriginal (owned) */
   int    i_orientation; /* EXIF Orientation (1-8, 0 if none) */
} GgazeInfo;

/* Gather info for p_file (synchronous). Returns a new GgazeInfo (caller owns,
 * free with info_delete) or NULL on failure. */
GgazeInfo *info_new(GFile *p_file);
void       info_delete(GgazeInfo *p_info);

/* Format the info as a multi-line string (caller frees). */
char *info_format(const GgazeInfo *p_info);

G_END_DECLS

#endif /* GGAZE_INFO_H */