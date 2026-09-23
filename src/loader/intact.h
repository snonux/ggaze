#ifndef GGAZE_INTACT_H
#define GGAZE_INTACT_H

/*:*
 * ggaze — is this PNG / JPEG container complete? (plain C, no decoder)
 *
 * The enhance path decodes PNG and JPEG through GEGL's own loaders for
 * their ICC awareness (decision #45), and those loaders -- gegl:png-load
 * and gegl:jpg-load, measured on gegl 0.4.72 -- do not fail on a file that
 * ends early: their stream reader starts the file over on EOF, so libpng
 * sees the iCCP chunk twice ("iCCP: duplicate"), libjpeg sees "two SOF
 * markers", and the load spins forever. The decode gate (detect.c) only
 * bounds the header; this bounds the tail: a container is handed to GEGL
 * only when it is structurally complete, which is exactly the condition
 * under which those loaders never reach EOF mid-decode. No pixel is
 * decoded: a PNG costs one skip per chunk (chunk lengths are declared), a
 * JPEG a marker walk to SOS and then one sequential read for the EOI.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

/* TRUE iff p_file is a PNG whose every chunk's declared data lies within
 * the file and whose chunks end with IEND. FALSE with
 * G_IO_ERROR_INVALID_DATA ("truncated") otherwise, or with the GIO error
 * of a read failure. */
gboolean intact_png(GFile *p_file, GError **p_err);

/* TRUE iff p_file is a JPEG whose marker segments up to the first SOS all
 * lie within the file and whose bytes after that SOS contain an EOI marker
 * (FF D9) -- the one thing libjpeg needs to find before the end of the
 * data. An EOI inside an APPn segment ahead of SOS (an EXIF thumbnail's)
 * does not count. FALSE with G_IO_ERROR_INVALID_DATA otherwise, or with
 * the GIO error of a read failure. */
gboolean intact_jpeg(GFile *p_file, GError **p_err);

G_END_DECLS

#endif /* GGAZE_INTACT_H */
