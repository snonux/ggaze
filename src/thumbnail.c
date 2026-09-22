/*:*
 * ggaze — thumbnail cache
 *
 * freedesktop TMS: ~/.cache/thumbnails/{normal(128), large(256)} + a custom
 * x-large bucket (512) for >256 (decision #37/T). Caches PNG keyed by md5(URI),
 * stores Thumb::URI/MTime/Size, verifies mtime. Async decode via GTask.
 *
 * The cache is on disk and therefore survives across ggaze runs (and is shared
 * with every other TMS-compliant app, e.g. Nautilus) -- that is the whole point
 * of following the spec instead of hiding a private .ggaze directory next to
 * the pictures. Persistence only actually worked from ix0 on, though: the write
 * side was always correct, but the read side asked gdk-pixbuf for the wrong
 * option key and so judged *every* entry stale. See _thumb_option() below.
 *
 * Decoding goes through loader_load_pixbuf_scaled(), so the thumbnail of a
 * JXL/AVIF/HEIF file comes from the same backend the large view uses and the
 * oversized-JPEG guard is the loader's, not a copy.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "thumbnail.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

#include "loader/loader.h"
#include "loader/pixbuf-util.h"

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
_cache_path(GFile *p_file, int i_bucket) {
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

/* Read a Thumb::* value out of a loaded cache PNG.
 *
 * gdk-pixbuf is asymmetric about tEXt keys and that asymmetry is exactly the
 * ix0 bug: gdk_pixbuf_save() takes them as "tEXt::Thumb::MTime", but its PNG
 * *loader* also re-exposes them prefixed (io-png.c does
 * g_strconcat("tEXt::", key)), so the plain "Thumb::MTime" this module used to
 * ask for always came back NULL. Every lookup was therefore treated as stale
 * and every thumbnail re-decoded and re-written on every launch -- the on-disk
 * cache was effectively write-only, which is precisely what "thumbnails are
 * regenerated from scratch when I reopen ggaze" looked like from the outside.
 *
 * Ask for the prefixed spelling, then fall back to the bare one: entries
 * written by another TMS app through a loader that does not prefix (or a
 * future glycin-backed PNG loader with different conventions) then still count
 * as hits instead of being silently regenerated. */
static const char *
_thumb_option(GdkPixbuf *p_pix, const char *c_key) {
   char       *c_prefixed = g_strconcat("tEXt::", c_key, NULL);
   const char *c_val      = gdk_pixbuf_get_option(p_pix, c_prefixed);
   g_free(c_prefixed);
   if (c_val == NULL) {
      c_val = gdk_pixbuf_get_option(p_pix, c_key);
   }
   return (c_val);
}

/* Load a cached PNG into a texture, but only if the entry really describes the
 * current state of p_file:
 *   - Thumb::MTime must equal i_mtime -- the spec's staleness check, so an
 *     edited/replaced source file gets a fresh thumbnail rather than the old
 *     picture;
 *   - Thumb::URI, when present, must equal p_file's URI -- the file name is
 *     only md5(URI), and ~/.cache/thumbnails is shared with every other TMS
 *     app, so this is what keeps a hash collision from showing the wrong
 *     image. Absent is tolerated (older/foreign writers omit it); a matching
 *     mtime alone is then the guarantee.
 *
 * Any failure -- missing file, corrupt or unreadable PNG, mismatch -- returns
 * NULL, which makes the caller regenerate. A cache must never be able to turn
 * a displayable image into an error. */
static GdkTexture *
_load_cached(GFile *p_file, const char *c_path, gint64 i_mtime) {
   GError    *p_err = NULL;
   GdkPixbuf *p_pix = gdk_pixbuf_new_from_file(c_path, &p_err);
   if (p_pix == NULL) {
      g_clear_error(&p_err);
      return (NULL);
   }
   const char *c_m       = _thumb_option(p_pix, "Thumb::MTime");
   const char *c_u       = _thumb_option(p_pix, "Thumb::URI");
   char       *c_uri     = g_file_get_uri(p_file);
   gboolean    b_foreign = (c_u != NULL && !g_str_equal(c_u, c_uri));
   g_free(c_uri);
   if (c_m == NULL || g_ascii_strtoll(c_m, NULL, 10) != i_mtime || b_foreign) {
      g_object_unref(p_pix);
      return (NULL); /* stale, foreign or unverifiable entry */
   }
   GdkTexture *p_tex = pixbuf_util_to_texture(p_pix);
   g_object_unref(p_pix);
   return (p_tex);
}

/* Best-effort write of p_use to c_cache_path as a TMS PNG (Thumb::URI/MTime/
 * Size). Failure to write is non-fatal -- the texture is still returned to
 * the caller even if the on-disk cache entry could not be created. Split out
 * of _generate() to keep it near the ~30-line-per-function convention. */
static void
_write_cache(GFile *p_file, GdkPixbuf *p_use, const char *c_cache_path,
             gint64 i_mtime, gint64 i_size) {
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
}

/* Decode the image at <= i_bucket px (preserving aspect) through the loader
 * (so every format the large view shows gets a thumbnail, and the oversized-
 * JPEG guard lives in one place), upright, and write a TMS PNG to
 * c_cache_path. Returns the texture. */
static GdkTexture *
_generate(GFile *p_file, int i_bucket, const char *c_cache_path, gint64 i_mtime,
          gint64 i_size, GCancellable *p_cancel, GError **p_err) {
   GdkPixbuf *p_use =
      loader_load_pixbuf_scaled(p_file, i_bucket, p_cancel, p_err);
   if (p_use == NULL) {
      return (NULL);
   }

   _write_cache(p_file, p_use, c_cache_path, i_mtime, i_size);

   GdkTexture *p_tex = pixbuf_util_to_texture(p_use);
   g_object_unref(p_use);
   if (p_tex == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "could not build thumbnail texture");
   }
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

/* Return TRUE (having completed p_task with G_IO_ERROR_CANCELLED) if the
 * request was cancelled. Checked twice in _thumb_run: once before any I/O so a
 * detached grid releases the GTask -- and the picture ref it carries --
 * promptly, and again right before the expensive decode. */
static gboolean
_thumb_bail_if_cancelled(GTask *p_task, GCancellable *p_cancel) {
   if (!g_cancellable_is_cancelled(p_cancel)) {
      return (FALSE);
   }
   g_task_return_new_error(p_task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                           "thumbnail request cancelled");
   return (TRUE);
}

static void
_thumb_run(GTask *p_task) {
   ThumbTask    *p_tt     = (ThumbTask *)g_task_get_task_data(p_task);
   GCancellable *p_cancel = g_task_get_cancellable(p_task);
   GError       *p_err    = NULL;

   if (_thumb_bail_if_cancelled(p_task, p_cancel)) {
      return;
   }

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

   /* Ensure the cache dir exists, 0700 as the TMS requires. Best-effort: if it
    * cannot be created (read-only $XDG_CACHE_HOME, a file in the way, quota)
    * the lookup below just misses and _generate() still returns a texture --
    * an unusable cache degrades ggaze to "slow", never to "broken". */
   char *c_dir = g_path_get_dirname(p_tt->c_cache_path);
   g_mkdir_with_parents(c_dir, 0700);
   g_free(c_dir);

   GdkTexture *p_tex = _load_cached(p_tt->p_file, p_tt->c_cache_path, i_mtime);
   if (p_tex == NULL) {
      /* The decode is the expensive step; re-check cancellation first so a
       * detached grid doesn't pay for gdk_pixbuf_new_from_file_at_scale plus
       * a cache write it no longer needs. */
      g_clear_error(&p_err); /* nothing has set it yet; hand _generate() a
                                pristine GError either way */
      if (_thumb_bail_if_cancelled(p_task, p_cancel)) {
         return;
      }
      p_tex = _generate(p_tt->p_file, p_tt->i_bucket, p_tt->c_cache_path,
                        i_mtime, i_size, p_cancel, &p_err);
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

/* Pool item free func (g_thread_pool_new_full): runs for every GTask still
 * queued when thumbnail_delete() drops the pool. Without it those tasks were
 * simply discarded -- never run, never completed, never unref'd -- leaking
 * the GTask, its GFile/ThumbTask and the GtkPicture ref the grid's callback
 * carries, once per cell still pending when a window closed. Completing them
 * as CANCELLED lets every owner release its refs through the normal
 * callback path. */
static void
_thumb_item_drop(gpointer p_data) {
   /* g_thread_pool_free() pushes its private wake-up marker
    * (glib/gthreadpool.c wakeup_thread_marker == GUINT_TO_POINTER(1)) into
    * the queue once per thread before draining it, and GLib 2.88 hands that
    * marker to the item free func too (verified by backtrace). It is not a
    * task; skip it. */
   if (p_data == GUINT_TO_POINTER(1)) {
      return;
   }
   GTask *p_task = G_TASK(p_data);
   g_task_return_new_error(p_task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                           "thumbnail pool shut down");
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
   p_t->p_pool = g_thread_pool_new_full(_thumb_pool_func, NULL,
                                        _thumb_item_drop, i_max, FALSE, &p_err);
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
      /* Drop queued work immediately (each queued GTask is completed as
       * CANCELLED by _thumb_item_drop); don't block on running decodes. */
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
   p_tt->c_cache_path  = _cache_path(p_file, i_bucket);
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

char *
thumbnail_cache_path(GFile *p_file, int i_size) {
   g_return_val_if_fail(G_IS_FILE(p_file), NULL);
   return (_cache_path(p_file, _bucket_for(i_size)));
}

GdkTexture *
thumbnail_get_finish(GAsyncResult *p_res, GError **p_err) {
   g_return_val_if_fail(G_IS_TASK(p_res), NULL);
   return ((GdkTexture *)g_task_propagate_pointer((GTask *)p_res, p_err));
}