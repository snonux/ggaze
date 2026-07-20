/*:*
 * ggaze — thumbnail cache
 *
 * freedesktop TMS: ~/.cache/thumbnails/{normal(128), large(256)} + a custom
 * x-large bucket (512) for >256 (decision #37/T). Caches PNG keyed by md5(URI),
 * stores Thumb::URI/MTime/Size, verifies mtime. Async decode via GTask.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "thumbnail.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

#define GGAZE_TMS_NORMAL 128
#define GGAZE_TMS_LARGE 256
#define GGAZE_TMS_XLARGE 512

struct Thumbnail {
   GThreadPool *p_pool; /* bounded decode pool, keeps the laptop off 100% */
};

typedef struct {
   Thumbnail *p_t;
   GFile     *p_file;       /* owned */
   int        i_size;       /* requested size */
   int        i_bucket;     /* bucket size actually cached */
   char      *c_cache_path; /* owned */
} ThumbTask;

/* --- helpers ------------------------------------------------------------- */

static int
_bucket_for(int i_size) {
   if (i_size <= GGAZE_TMS_NORMAL) {
      return (GGAZE_TMS_NORMAL);
   }
   if (i_size <= GGAZE_TMS_LARGE) {
      return (GGAZE_TMS_LARGE);
   }
   return (GGAZE_TMS_XLARGE);
}

static char *
_cache_dir_for_bucket(int i_bucket) {
   const char *c_sub   = (i_bucket <= GGAZE_TMS_NORMAL)  ? "normal"
                         : (i_bucket <= GGAZE_TMS_LARGE) ? "large"
                                                         : "x-large";
   const char *c_cache = g_get_user_cache_dir();
   char       *c_dir   = g_build_filename(c_cache, "thumbnails", c_sub, NULL);
   return (c_dir);
}

static char *
_cache_path(Thumbnail *p_t, GFile *p_file, int i_bucket) {
   (void)p_t;
   char      *c_uri = g_file_get_uri(p_file);
   GChecksum *p_sum = g_checksum_new(G_CHECKSUM_MD5);
   g_checksum_update(p_sum, (const guchar *)c_uri, strlen(c_uri));
   const char *c_hex  = g_checksum_get_string(p_sum);
   char       *c_dir  = _cache_dir_for_bucket(i_bucket);
   char       *c_path = g_strdup_printf("%s/%s.png", c_dir, c_hex);
   g_free(c_dir);
   g_checksum_free(p_sum);
   g_free(c_uri);
   return (c_path);
}

static GdkTexture *
_texture_from_pixbuf(GdkPixbuf *p_pix) {
   g_return_val_if_fail(GDK_IS_PIXBUF(p_pix), NULL);
   int        i_w    = gdk_pixbuf_get_width(p_pix);
   int        i_h    = gdk_pixbuf_get_height(p_pix);
   GdkPixbuf *p_rgba = gdk_pixbuf_get_has_alpha(p_pix)
                          ? GDK_PIXBUF(g_object_ref(p_pix))
                          : gdk_pixbuf_add_alpha(p_pix, FALSE, 0, 0, 0);
   if (p_rgba == NULL) {
      return (NULL);
   }
   int     i_rowstride = gdk_pixbuf_get_rowstride(p_rgba);
   guchar *p_pixels    = gdk_pixbuf_get_pixels(p_rgba);
   gsize   u_len   = (gsize)(i_h - 1) * (gsize)i_rowstride + (gsize)i_w * 4u;
   GBytes *p_bytes = g_bytes_new_with_free_func(
      p_pixels, u_len, (GDestroyNotify)g_object_unref, p_rgba);
   GdkTexture *p_tex = gdk_memory_texture_new(i_w, i_h, GDK_MEMORY_R8G8B8A8,
                                              p_bytes, (gsize)i_rowstride);
   g_bytes_unref(p_bytes);
   return (p_tex);
}

/* Load a cached PNG into a texture, verifying Thumb::MTime == i_mtime. */
static GdkTexture *
_load_cached(const char *c_path, gint64 i_mtime) {
   GError    *p_err = NULL;
   GdkPixbuf *p_pix = gdk_pixbuf_new_from_file(c_path, &p_err);
   if (p_pix == NULL) {
      g_clear_error(&p_err);
      return (NULL);
   }
   const char *c_m = gdk_pixbuf_get_option(p_pix, "Thumb::MTime");
   if (c_m == NULL || g_ascii_strtoll(c_m, NULL, 10) != i_mtime) {
      g_object_unref(p_pix);
      return (NULL); /* stale or missing mtime */
   }
   GdkTexture *p_tex = _texture_from_pixbuf(p_pix);
   g_object_unref(p_pix);
   return (p_tex);
}

/* Decode the image at <= i_bucket px (preserving aspect), apply EXIF
 * orientation, and write a TMS PNG to c_cache_path. Returns the texture. */
static GdkTexture *
_generate(GFile *p_file, int i_bucket, const char *c_cache_path, gint64 i_mtime,
          gint64 i_size, GError **p_err) {
   char *c_path = g_file_get_path(p_file);
   if (c_path == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "cannot thumbnail a non-local file");
      return (NULL);
   }
   GdkPixbuf *p_pix = gdk_pixbuf_new_from_file_at_scale(c_path, i_bucket,
                                                        i_bucket, TRUE, p_err);
   g_free(c_path);
   if (p_pix == NULL) {
      return (NULL);
   }
   GdkPixbuf *p_oriented = gdk_pixbuf_apply_embedded_orientation(p_pix);
   GdkPixbuf *p_use =
      (p_oriented != NULL) ? p_oriented : GDK_PIXBUF(g_object_ref(p_pix));
   g_object_unref(p_pix);

   /* Best-effort write to the cache; failure to write is non-fatal. */
   char c_mtime[32];
   char c_size[32];
   g_snprintf(c_mtime, sizeof(c_mtime), "%" G_GINT64_FORMAT, i_mtime);
   g_snprintf(c_size, sizeof(c_size), "%" G_GINT64_FORMAT, i_size);
   char   *c_uri  = g_file_get_uri(p_file);
   GError *p_werr = NULL;
   gdk_pixbuf_save(p_use, c_cache_path, "png", &p_werr, "tEXt::Thumb::URI",
                   c_uri, "tEXt::Thumb::MTime", c_mtime, "tEXt::Thumb::Size",
                   c_size, NULL);
   if (p_werr != NULL) {
      g_error_free(p_werr);
   }
   g_free(c_uri);

   GdkTexture *p_tex = _texture_from_pixbuf(p_use);
   g_object_unref(p_use);
   return (p_tex);
}

/* --- GTask worker -------------------------------------------------------- */

static void
_thumb_task_free(gpointer p_void) {
   ThumbTask *p_tt = (ThumbTask *)p_void;
   g_clear_object(&p_tt->p_file);
   g_free(p_tt->c_cache_path);
   g_free(p_tt);
}

static void
_thumb_run(GTask *p_task) {
   ThumbTask *p_tt  = (ThumbTask *)g_task_get_task_data(p_task);
   GError    *p_err = NULL;

   /* File mtime + size (for verify + Thumb::Size). */
   GFileInfo *p_info =
      g_file_query_info(p_tt->p_file, "standard::size,time::modified",
                        G_FILE_QUERY_INFO_NONE, NULL, &p_err);
   if (p_info == NULL) {
      g_task_return_error(p_task, p_err);
      return;
   }
   gint64 i_mtime = (gint64)g_file_info_get_attribute_uint64(
      p_info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
   gint64 i_size = (gint64)g_file_info_get_size(p_info);
   g_object_unref(p_info);

   /* Ensure the cache dir exists (best-effort). */
   char *c_dir = g_path_get_dirname(p_tt->c_cache_path);
   g_mkdir_with_parents(c_dir, 0700);
   g_free(c_dir);

   GdkTexture *p_tex = _load_cached(p_tt->c_cache_path, i_mtime);
   if (p_tex == NULL) {
      p_tex = _generate(p_tt->p_file, p_tt->i_bucket, p_tt->c_cache_path,
                        i_mtime, i_size, &p_err);
   }
   if (p_tex == NULL) {
      g_task_return_error(p_task, p_err);
   } else {
      g_task_return_pointer(p_task, p_tex, (GDestroyNotify)g_object_unref);
   }
}

/* Bounded pool worker: run the decode, then drop our task ref. g_task_return_*
 * marshals the callback to the main thread regardless of which thread calls
 * it, so this is safe from a worker. */
static void
_thumb_pool_func(gpointer p_data, gpointer p_user) {
   (void)p_user;
   GTask *p_task = G_TASK(p_data);
   _thumb_run(p_task);
   g_object_unref(p_task);
}

/* GTaskThreadFunc wrapper for the (unlikely) fallback to g_task_run_in_thread
 * if the bounded pool could not be created. */
static void
_thumb_pool_func_wrap(GTask *p_task, gpointer p_src, gpointer p_task_data,
                      GCancellable *p_cancel) {
   (void)p_src;
   (void)p_task_data;
   (void)p_cancel;
   _thumb_run(p_task);
}

/* --- public ------------------------------------------------------------- */

Thumbnail *
thumbnail_new(void) {
   Thumbnail *p_t = g_new(Thumbnail, 1);
   /* Bound the decode pool to ~half the cores (max 4) so a large folder's
    * thumbnail generation doesn't peg every CPU at 100%. g_task_return_* still
    * delivers each result to the main thread. */
   gint    i_max = MAX(1, MIN(g_get_num_processors() / 2, 4));
   GError *p_err = NULL;
   p_t->p_pool =
      g_thread_pool_new(_thumb_pool_func, NULL, i_max, FALSE, &p_err);
   if (p_err != NULL) {
      g_warning("ggaze: thumbnail pool: %s", p_err->message);
      g_error_free(p_err);
   }
   return (p_t);
}

void
thumbnail_delete(Thumbnail *p_t) {
   if (p_t == NULL) {
      return;
   }
   if (p_t->p_pool != NULL) {
      /* Drop queued work immediately; don't block on running decodes. */
      g_thread_pool_free(p_t->p_pool, TRUE, FALSE);
   }
   g_free(p_t);
}

void
thumbnail_get_async(Thumbnail *p_t, GFile *p_file, int i_size,
                    GCancellable *p_cancel, GAsyncReadyCallback p_cb,
                    gpointer p_data) {
   g_return_if_fail(p_t != NULL);
   g_return_if_fail(G_IS_FILE(p_file));
   int        i_bucket = _bucket_for(i_size);
   ThumbTask *p_tt     = g_new(ThumbTask, 1);
   p_tt->p_t           = p_t;
   p_tt->p_file        = (GFile *)g_object_ref(p_file);
   p_tt->i_size        = i_size;
   p_tt->i_bucket      = i_bucket;
   p_tt->c_cache_path  = _cache_path(p_t, p_file, i_bucket);
   GTask *p_task       = g_task_new(p_file, p_cancel, p_cb, p_data);
   g_task_set_task_data(p_task, p_tt, _thumb_task_free);
   if (p_t->p_pool != NULL) {
      /* Push to the bounded pool (transfers our extra ref to the worker). */
      g_thread_pool_push(p_t->p_pool, g_object_ref(p_task), NULL);
      g_object_unref(p_task);
   } else {
      /* Fallback if the pool failed to create: unbounded GTask pool. */
      g_task_run_in_thread(p_task, _thumb_pool_func_wrap);
      g_object_unref(p_task);
   }
}

GdkTexture *
thumbnail_get_finish(Thumbnail *p_t, GAsyncResult *p_res, GError **p_err) {
   (void)p_t;
   g_return_val_if_fail(G_IS_TASK(p_res), NULL);
   return ((GdkTexture *)g_task_propagate_pointer((GTask *)p_res, p_err));
}