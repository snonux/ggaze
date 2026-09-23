/*:*
 * ggaze — image info / EXIF gather
 *
 * Gathers file info (GFileInfo), pixel dimensions (through the loader, so
 * every format the viewer shows is covered and the oversized-JPEG guard is
 * the loader's), EXIF tags (libexif) and the declared colour space (the
 * embedded ICC profile's description, icc.c) into a GgazeInfo struct.
 * Plain-C, no GtkWidget; info-overlay.c runs it in a GTask worker.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "info.h"

#include <libexif/exif-data.h>
#include <libexif/exif-format.h>
#include <libexif/exif-tag.h>

#include "icc.h"
#include "loader/loader.h"

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
   if (p_entry == NULL || p_entry->data == NULL) {
      return (0);
   }
   /* Orientation must be exactly one SHORT (2-byte) component per the EXIF
    * spec. exif_get_short() blindly reads 2 bytes from p_entry->data and
    * trusts the caller to have validated format/size first -- a malformed
    * file can declare a mismatched format (e.g. 1-byte BYTE) or a truncated
    * component count while data's real allocation follows that declared
    * size, so skipping this check is a heap out-of-bounds read on crafted
    * EXIF. Reject anything that doesn't match, or that decodes outside the
    * spec's 1-8 orientation range, as unknown rather than passing it through.
    */
   if (p_entry->format != EXIF_FORMAT_SHORT || p_entry->components != 1 ||
       p_entry->size < exif_format_get_size(EXIF_FORMAT_SHORT)) {
      return (0);
   }
   ExifByteOrder e_order  = exif_data_get_byte_order(p_data);
   int           i_orient = (int)exif_get_short(p_entry->data, e_order);
   if (i_orient < 1 || i_orient > 8) {
      return (0);
   }
   return (i_orient);
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

/* Dimensions through the loader: a header scan for the formats GdkPixbuf
 * knows (no pixel decode), the matching backend's decode for JXL/AVIF/HEIF.
 * info_new() runs in a GTask worker (info-overlay.c), so the decode path
 * cannot freeze the UI. On failure the fields stay 0 and info_format says
 * "Size unknown". */
static void
_fill_dims(GgazeInfo *p_info, GFile *p_file) {
   int i_w = 0;
   int i_h = 0;
   if (loader_peek_dimensions(p_file, &i_w, &i_h)) {
      p_info->i_width  = i_w;
      p_info->i_height = i_h;
   }
}

/* EXIF via libexif: camera/lens/exposure fields + raw Orientation. No-op
 * (fields stay NULL/0) if the file carries no EXIF data. */
static void
_fill_exif(GgazeInfo *p_info, const char *c_path) {
   ExifData *p_exif = exif_data_new_from_file(c_path);
   if (p_exif == NULL) {
      return;
   }
   p_info->c_camera   = _dup_camera(p_exif);
   p_info->c_focal    = _dup_exif_value(p_exif, EXIF_TAG_FOCAL_LENGTH);
   p_info->c_aperture = _dup_exif_value(p_exif, EXIF_TAG_FNUMBER);
   p_info->c_shutter  = _dup_exif_value(p_exif, EXIF_TAG_EXPOSURE_TIME);
   p_info->c_iso      = _dup_exif_value(p_exif, EXIF_TAG_ISO_SPEED_RATINGS);
   p_info->c_datetime = _dup_exif_value(p_exif, EXIF_TAG_DATE_TIME_ORIGINAL);
   /* Lens model (EXIF 2.3, tag 0xa434); libexif exposes it as a plain
    * ASCII tag. The field was declared but never filled. */
   p_info->c_lens        = _dup_exif_value(p_exif, EXIF_TAG_LENS_MODEL);
   p_info->i_orientation = _get_orientation(p_exif);
   exif_data_unref(p_exif);
}

int
info_exif_orientation(const char *c_path) {
   g_return_val_if_fail(c_path != NULL, 0);
   ExifData *p_exif = exif_data_new_from_file(c_path);
   if (p_exif == NULL) {
      return (0);
   }
   int i_orient = _get_orientation(p_exif);
   exif_data_unref(p_exif);
   return (i_orient);
}

/* Colour space: the embedded ICC profile's description (icc.c) when the
 * file carries a readable one. A profile container that is there but broken
 * (INVALID_DATA), or bytes that are not a profile at all, are reported as
 * unreadable rather than passed off as sRGB, so the card never claims a
 * colour space the file does not deliver; an I/O failure (the file vanished
 * mid-gather) reads as "none", like every other field here. A format
 * icc.c does not search (WebP, AVIF, HEIF, JXL) is "not inspected", not
 * "none": it may well carry a profile, and the card must not call it sRGB
 * on the strength of a search that never happened. */
static void
_fill_colorspace(GgazeInfo *p_info, GFile *p_file) {
   GError *p_err = NULL;
   GBytes *p_icc = icc_read_embedded(p_file, &p_err);
   if (p_icc == NULL) {
      p_info->e_icc =
         g_error_matches(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA)
            ? GGAZE_ICC_UNREADABLE
            : GGAZE_ICC_NONE;
      if (p_err == NULL && !icc_container_searched(p_file)) {
         p_info->e_icc = GGAZE_ICC_UNINSPECTED;
      }
      g_clear_error(&p_err);
      return;
   }
   if (!icc_is_profile(p_icc)) {
      p_info->e_icc = GGAZE_ICC_UNREADABLE;
   } else {
      p_info->e_icc        = GGAZE_ICC_EMBEDDED;
      p_info->c_colorspace = icc_description(p_icc);
      if (p_info->c_colorspace == NULL) {
         p_info->c_colorspace = g_strdup("unnamed profile");
      }
   }
   g_bytes_unref(p_icc);
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
   _fill_dims(p_info, p_file);
   _fill_exif(p_info, c_path);
   _fill_colorspace(p_info, p_file);

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
   g_free(p_info->c_colorspace);
   g_free(p_info);
}

static char *
_join(GString *p_str, const char *c_label, const char *c_val) {
   if (c_val != NULL && c_val[0] != '\0') {
      g_string_append_printf(p_str, "%s: %s\n", c_label, c_val);
   }
   return (NULL);
}

/* The profile description as the card shows it: the profile's 'desc' is
 * the file's text, not ggaze's, so control characters (a newline would
 * start a fake card line) become spaces, and it is cut to
 * INFO_ICC_DESC_MAX characters with an ellipsis (a v4 profile may carry a
 * paragraph). Caller frees. */
static char *
_display_desc(const char *c_desc) {
   GString *p_out = g_string_new(NULL);
   glong    l_n   = 0;
   for (const char *p = c_desc; *p != '\0'; p = g_utf8_next_char(p), l_n++) {
      if (l_n == INFO_ICC_DESC_MAX) {
         g_string_append(p_out, "…");
         break;
      }
      gunichar u_c = g_utf8_get_char(p);
      g_string_append_unichar(p_out, g_unichar_iscntrl(u_c) ? ' ' : u_c);
   }
   return (g_string_free(p_out, FALSE));
}

/* The colour-space line, one wording per GgazeIccState (see info.h). An
 * embedded profile's name is followed by "managed on enhance/export" only
 * when the caller established that this build's enhance path will really
 * apply it (b_icc_managed): a note claimed for every profile was untrue
 * for an sRGB profile, a JPEG in a build without libjpeg, a non-local file
 * or a profile babl cannot parse -- all of which the enhancer leaves on
 * the plain loader path. */
static void
_append_colorspace(GString *p_str, const GgazeInfo *p_info) {
   char *c_desc = NULL;
   switch (p_info->e_icc) {
   case GGAZE_ICC_EMBEDDED:
      c_desc = _display_desc(p_info->c_colorspace);
      g_string_append_printf(
         p_str, "Color space: %s (embedded ICC%s)\n", c_desc,
         p_info->b_icc_managed ? "; managed on enhance/export" : "");
      g_free(c_desc);
      break;
   case GGAZE_ICC_UNREADABLE:
      g_string_append(p_str, "Color space: embedded ICC profile unreadable "
                             "(shown as sRGB)\n");
      break;
   case GGAZE_ICC_UNINSPECTED:
      g_string_append(p_str, "Color space: not read for this format "
                             "(shown as sRGB)\n");
      break;
   default:
      g_string_append(p_str,
                      "Color space: sRGB (assumed, no embedded profile)\n");
      break;
   }
}

char *
info_format(const GgazeInfo *p_info) {
   if (p_info == NULL) {
      return (g_strdup(""));
   }
   GString *p_str = g_string_new(NULL);
   if (p_info->i_width > 0 && p_info->i_height > 0) {
      g_string_append_printf(p_str, "%d×%d\n", p_info->i_width,
                             p_info->i_height);
   } else {
      g_string_append(p_str, "Size unknown (not decodable)\n");
   }
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
   _append_colorspace(p_str, p_info);
   /* trim trailing newline */
   if (p_str->len > 0 && p_str->str[p_str->len - 1] == '\n') {
      g_string_truncate(p_str, p_str->len - 1);
   }
   return (g_string_free(p_str, FALSE));
}