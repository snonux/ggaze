/*:*
 * ggaze — image info / EXIF gather
 *
 * Gathers file info (via GFileInfo + GdkPixbuf for dims) and EXIF tags (via
 * libexif) into a GgazeInfo struct. Plain-C, no GtkWidget.
 *
 * JPEG-specific guard (mu0 review round 2): _fill_dims() calls
 * gdk_pixbuf_new_from_file() directly on the source file, bypassing
 * src/loader/loader.c entirely -- and info_new() runs synchronously on the
 * GTK main thread (window.c's _show_info, the info keybinding), so an
 * unguarded stall here freezes the whole UI. See _fill_dims() below.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "info.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <libexif/exif-data.h>
#include <libexif/exif-tag.h>

#include "loader/detect.h"

static char *
_dup_exif_value(ExifData *p_data, ExifTag e_tag) {
   if (p_data == NULL) {
      return (NULL);
   }
   ExifEntry *p_entry = exif_data_get_entry(p_data, e_tag);
   if (p_entry == NULL) {
      return (NULL);
   }
   char c_buf[256];
   exif_entry_get_value(p_entry, c_buf, sizeof(c_buf));
   if (c_buf[0] == '\0') {
      return (NULL);
   }
   return (g_strdup(c_buf));
}

static char *
_dup_camera(ExifData *p_data) {
   char *c_make  = _dup_exif_value(p_data, EXIF_TAG_MAKE);
   char *c_model = _dup_exif_value(p_data, EXIF_TAG_MODEL);
   char *c_out   = NULL;
   if (c_make != NULL && c_model != NULL) {
      c_out = g_strdup_printf("%s %s", c_make, c_model);
   } else if (c_make != NULL) {
      c_out = g_strdup(c_make);
   } else if (c_model != NULL) {
      c_out = g_strdup(c_model);
   }
   g_free(c_make);
   g_free(c_model);
   return (c_out);
}

static int
_get_orientation(ExifData *p_data) {
   if (p_data == NULL) {
      return (0);
   }
   ExifEntry *p_entry = exif_data_get_entry(p_data, EXIF_TAG_ORIENTATION);
   if (p_entry == NULL || p_entry->components == 0 || p_entry->data == NULL) {
      return (0);
   }
   /* Orientation is a SHORT (2 bytes, big/little endian per the data's byte
    * order). */
   ExifByteOrder e_order = exif_data_get_byte_order(p_data);
   return ((int)exif_get_short(p_entry->data, e_order));
}

/* File info: size + content type. Any failure (e.g. a race with deletion)
 * just leaves those fields at their g_new0 zero value. */
static void
_fill_file_info(GgazeInfo *p_info, GFile *p_file) {
   GError    *p_err = NULL;
   GFileInfo *p_fi =
      g_file_query_info(p_file, "standard::size,standard::content-type",
                        G_FILE_QUERY_INFO_NONE, NULL, &p_err);
   if (p_fi == NULL) {
      g_clear_error(&p_err);
      return;
   }
   p_info->i_size   = (gint64)g_file_info_get_size(p_fi);
   const char *c_ct = g_file_info_get_content_type(p_fi);
   if (c_ct != NULL) {
      p_info->c_format = g_strdup(c_ct);
   }
   g_object_unref(p_fi);
}

/* Dimensions via GdkPixbuf (without applying orientation). Peeks a JPEG's
 * declared header dimensions first (detect_jpeg_peek_dims_from_path(), no
 * decoder invoked) and skips the GdkPixbuf call entirely when oversized --
 * info_new() runs synchronously on the GTK main thread (window.c's
 * _show_info, bound to the info keybinding), so gdk_pixbuf_new_from_file()
 * stalling ~28s there (mu0 review round 2: GdkPixbuf/glycin pre-allocating
 * off the declared size before its own internal cap rejects it) freezes the
 * whole UI, not just a background worker. Any other failure (missing/
 * corrupt file, non-JPEG) falls through to GdkPixbuf's own error, which is
 * discarded exactly as before this guard: info display simply omits
 * dimensions (fields stay at their g_new0 zero) rather than failing
 * info_new() outright. */
static void
_fill_dims(GgazeInfo *p_info, const char *c_path) {
   guint32 u_w, u_h;
   if (detect_jpeg_peek_dims_from_path(c_path, &u_w, &u_h) &&
       !detect_jpeg_dims_within_bounds(u_w, u_h, NULL)) {
      return;
   }
   GError    *p_err = NULL;
   GdkPixbuf *p_pix = gdk_pixbuf_new_from_file(c_path, &p_err);
   if (p_pix == NULL) {
      g_clear_error(&p_err);
      return;
   }
   p_info->i_width  = gdk_pixbuf_get_width(p_pix);
   p_info->i_height = gdk_pixbuf_get_height(p_pix);
   g_object_unref(p_pix);
}

/* EXIF via libexif: camera/lens/exposure fields + raw Orientation. No-op
 * (fields stay NULL/0) if the file carries no EXIF data. */
static void
_fill_exif(GgazeInfo *p_info, const char *c_path) {
   ExifData *p_exif = exif_data_new_from_file(c_path);
   if (p_exif == NULL) {
      return;
   }
   p_info->c_camera      = _dup_camera(p_exif);
   p_info->c_focal       = _dup_exif_value(p_exif, EXIF_TAG_FOCAL_LENGTH);
   p_info->c_aperture    = _dup_exif_value(p_exif, EXIF_TAG_FNUMBER);
   p_info->c_shutter     = _dup_exif_value(p_exif, EXIF_TAG_EXPOSURE_TIME);
   p_info->c_iso         = _dup_exif_value(p_exif, EXIF_TAG_ISO_SPEED_RATINGS);
   p_info->c_datetime    = _dup_exif_value(p_exif, EXIF_TAG_DATE_TIME_ORIGINAL);
   p_info->i_orientation = _get_orientation(p_exif);
   exif_data_unref(p_exif);
}

GgazeInfo *
info_new(GFile *p_file) {
   g_return_val_if_fail(G_IS_FILE(p_file), NULL);
   char *c_path = g_file_get_path(p_file);
   if (c_path == NULL) {
      return (NULL);
   }

   GgazeInfo *p_info = g_new0(GgazeInfo, 1);
   _fill_file_info(p_info, p_file);
   _fill_dims(p_info, c_path);
   _fill_exif(p_info, c_path);

   g_free(c_path);
   return (p_info);
}

void
info_delete(GgazeInfo *p_info) {
   if (p_info == NULL) {
      return;
   }
   g_free(p_info->c_format);
   g_free(p_info->c_camera);
   g_free(p_info->c_lens);
   g_free(p_info->c_focal);
   g_free(p_info->c_aperture);
   g_free(p_info->c_shutter);
   g_free(p_info->c_iso);
   g_free(p_info->c_datetime);
   g_free(p_info);
}

static char *
_join(GString *p_str, const char *c_label, const char *c_val) {
   if (c_val != NULL && c_val[0] != '\0') {
      g_string_append_printf(p_str, "%s: %s\n", c_label, c_val);
   }
   return (NULL);
}

char *
info_format(const GgazeInfo *p_info) {
   if (p_info == NULL) {
      return (g_strdup(""));
   }
   GString *p_str = g_string_new(NULL);
   g_string_append_printf(p_str, "%d×%d\n", p_info->i_width, p_info->i_height);
   _join(p_str, "Format", p_info->c_format);
   if (p_info->i_size > 0) {
      char *c_sz = g_format_size(p_info->i_size);
      g_string_append_printf(p_str, "Size: %s\n", c_sz);
      g_free(c_sz);
   }
   _join(p_str, "Camera", p_info->c_camera);
   _join(p_str, "Lens", p_info->c_lens);
   _join(p_str, "Focal", p_info->c_focal);
   _join(p_str, "Aperture", p_info->c_aperture);
   _join(p_str, "Shutter", p_info->c_shutter);
   _join(p_str, "ISO", p_info->c_iso);
   _join(p_str, "Date", p_info->c_datetime);
   if (p_info->i_orientation > 0) {
      g_string_append_printf(p_str, "Orientation: %d\n", p_info->i_orientation);
   }
   /* trim trailing newline */
   if (p_str->len > 0 && p_str->str[p_str->len - 1] == '\n') {
      g_string_truncate(p_str, p_str->len - 1);
   }
   return (g_string_free(p_str, FALSE));
}