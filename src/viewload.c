/*:*
 * ggaze — view-load pipeline (large view)
 *
 * See viewload.h. Extracted from window.c's _show_texture/_prefetch/
 * _load_current family so the last-write-wins logic is unit-testable.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "viewload.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

#include "loader/loader.h"
#include "texturecache.h"

struct ViewLoad {
   gatomicrefcount        u_refs;
   const ViewLoadHostOps *p_ops;
   gpointer               p_host;
   Navigator             *p_nav;    /* borrowed, may be NULL */
   TextureCache          *p_cache;  /* bounded LRU of decoded textures */
   GCancellable          *p_cancel; /* visible load; cancelled on each nav */
   GCancellable *p_prefetch_cancel; /* prefetch round; cancelled per round */
   gboolean      b_disposed;
};

/* One per loader_load_async call. Carries the source GFile identity so the
 * main-thread progress/finish callbacks can enforce last-write-wins: a
 * result whose file no longer equals navigator.current is dropped instead
 * of overwriting the viewer. The GTask always invokes the finish callback
 * (even on cancellation), which is the sole owner that frees the ctx. */
typedef struct {
   ViewLoad *p_vl;   /* ref'd; outlives the load */
   GFile    *p_file; /* ref'd; the file being loaded */
} LoadCtx;

/* A partial texture hopping from the decode thread to the main thread. */
typedef struct {
   ViewLoad   *p_vl;
   GFile      *p_file;
   GdkTexture *p_tex;
} ProgressInvoke;

static void _prefetch(ViewLoad *p_vl);

static ViewLoad *
_ref(ViewLoad *p_vl) {
   g_atomic_ref_count_inc(&p_vl->u_refs);
   return (p_vl);
}

static void
_unref(ViewLoad *p_vl) {
   if (g_atomic_ref_count_dec(&p_vl->u_refs)) {
      g_clear_object(&p_vl->p_cancel);
      g_clear_object(&p_vl->p_prefetch_cancel);
      g_clear_pointer(&p_vl->p_cache, texturecache_delete);
      g_free(p_vl);
   }
}

/* TRUE iff p_file is still navigator.current (last-write-wins). */
static gboolean
_is_current(ViewLoad *p_vl, GFile *p_file) {
   if (p_vl->b_disposed || p_vl->p_nav == NULL) {
      return (FALSE);
   }
   GFile *p_cur = navigator_get_current(p_vl->p_nav);
   return (p_cur != NULL && g_file_equal(p_cur, p_file));
}

ViewLoad *
viewload_new(const ViewLoadHostOps *p_ops, gpointer p_host, guint u_cache_cap) {
   g_return_val_if_fail(p_ops != NULL, NULL);
   ViewLoad *p_vl = g_new0(ViewLoad, 1);
   g_atomic_ref_count_init(&p_vl->u_refs);
   p_vl->p_ops             = p_ops;
   p_vl->p_host            = p_host;
   p_vl->p_cache           = texturecache_new(u_cache_cap);
   p_vl->p_cancel          = g_cancellable_new();
   p_vl->p_prefetch_cancel = g_cancellable_new();
   return (p_vl);
}

void
viewload_set_navigator(ViewLoad *p_vl, Navigator *p_nav) {
   g_return_if_fail(p_vl != NULL);
   p_vl->p_nav = p_nav;
}

GdkTexture *
viewload_get_cached(ViewLoad *p_vl, GFile *p_file) {
   g_return_val_if_fail(p_vl != NULL, NULL);
   if (p_file == NULL || p_vl->p_cache == NULL) {
      return (NULL);
   }
   return (texturecache_get(p_vl->p_cache, p_file));
}

void
viewload_clear_cache(ViewLoad *p_vl) {
   g_return_if_fail(p_vl != NULL);
   if (p_vl->p_cache != NULL) {
      texturecache_clear(p_vl->p_cache);
   }
}

/* --- prefetch ------------------------------------------------------------ */

/* Prefetch callback: just cache the result (never touches the viewer). */
static void
_prefetch_finish_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   LoadCtx    *p_ctx = (LoadCtx *)p_data;
   GError     *p_err = NULL;
   GdkTexture *p_tex = loader_load_finish(p_res, &p_err);
   (void)p_src;
   if (p_tex != NULL) {
      if (!p_ctx->p_vl->b_disposed) {
         texturecache_put(p_ctx->p_vl->p_cache, p_ctx->p_file, p_tex);
      }
      g_object_unref(p_tex);
   } else {
      g_clear_error(&p_err); /* a neighbour that fails to decode is not
                              * worth a message; the visible load reports */
   }
   g_object_unref(p_ctx->p_file);
   _unref(p_ctx->p_vl);
   g_free(p_ctx);
}

/* Prefetch the next/previous images into the cache (not shown). Cancels the
 * previous prefetch round so at most two prefetch loads are in flight. */
static void
_prefetch(ViewLoad *p_vl) {
   if (p_vl->p_nav == NULL) {
      return;
   }
   g_cancellable_cancel(p_vl->p_prefetch_cancel);
   g_clear_object(&p_vl->p_prefetch_cancel);
   p_vl->p_prefetch_cancel = g_cancellable_new();

   gint  i_idx = navigator_get_current_index(p_vl->p_nav);
   guint u_n   = navigator_get_count(p_vl->p_nav);
   if (u_n == 0) {
      return;
   }
   for (gint i_delta = -1; i_delta <= 1; i_delta += 2) {
      gint i_j = i_idx + i_delta;
      if (i_j < 0 || i_j >= (gint)u_n) {
         continue;
      }
      GFile *p_file = navigator_get_file(p_vl->p_nav, (guint)i_j);
      if (p_file != NULL && texturecache_get(p_vl->p_cache, p_file) == NULL) {
         LoadCtx *p_ctx = g_new(LoadCtx, 1);
         p_ctx->p_vl    = _ref(p_vl);
         p_ctx->p_file  = (GFile *)g_object_ref(p_file);
         loader_load_async(p_file, p_vl->p_prefetch_cancel, NULL, NULL,
                           _prefetch_finish_cb, p_ctx);
      }
   }
}

/* --- visible load -------------------------------------------------------- */

static gboolean
_on_progress_main(gpointer p_data) {
   ProgressInvoke *p_pi = (ProgressInvoke *)p_data;
   /* Last-write-wins: show the partial only if its source file is still the
    * current one; the full result replaces it in _load_finish_cb. */
   if (_is_current(p_pi->p_vl, p_pi->p_file)) {
      p_pi->p_vl->p_ops->show_texture(p_pi->p_vl->p_host, p_pi->p_tex);
   }
   g_object_unref(p_pi->p_tex);
   g_object_unref(p_pi->p_file);
   _unref(p_pi->p_vl);
   g_free(p_pi);
   return (G_SOURCE_REMOVE);
}

/* Decode-thread progress callback: hop to the main thread. */
static void
_load_progress_cb(GdkTexture *p_partial, gpointer p_data) {
   LoadCtx        *p_ctx = (LoadCtx *)p_data;
   ProgressInvoke *p_pi  = g_new(ProgressInvoke, 1);
   p_pi->p_vl            = _ref(p_ctx->p_vl);
   p_pi->p_file          = (GFile *)g_object_ref(p_ctx->p_file);
   p_pi->p_tex           = (GdkTexture *)g_object_ref(p_partial);
   g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, _on_progress_main, p_pi,
                              NULL);
}

/* The visible load of a still-current file failed: clear the canvas and say
 * so. Leaving the previous picture under the new title violated the
 * "viewer only shows navigator.current" invariant -- and did so silently. */
static void
_report_failure(ViewLoad *p_vl, GFile *p_file, const GError *p_err) {
   char *c_name = g_file_get_basename(p_file);
   char *c_msg  = g_strdup_printf("Cannot show %s: %s", c_name,
                                  p_err != NULL ? p_err->message : "?");
   g_debug("ggaze: failed to load %s: %s", c_name,
           p_err != NULL ? p_err->message : "?");
   p_vl->p_ops->show_texture(p_vl->p_host, NULL);
   p_vl->p_ops->show_status(p_vl->p_host, c_msg);
   g_free(c_msg);
   g_free(c_name);
}

/* Visible-load callback: only if this is still the current file
 * (last-write-wins), cache it, show it and prefetch neighbours. The cache
 * put comes BEFORE the show on purpose: the host's show_texture consults
 * viewload_get_cached() to decide whether the texture on screen provably
 * belongs to navigator.current (window.c _info_texture_for, which feeds the
 * info card's histogram), so the entry has to exist by the time the host
 * sees the texture, or a decode landing under an open card could never be
 * plotted. */
static void
_load_finish_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   LoadCtx    *p_ctx = (LoadCtx *)p_data;
   ViewLoad   *p_vl  = p_ctx->p_vl;
   GError     *p_err = NULL;
   GdkTexture *p_tex = loader_load_finish(p_res, &p_err);
   if (p_tex == NULL) {
      if (!g_error_matches(p_err, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
          _is_current(p_vl, p_ctx->p_file)) {
         _report_failure(p_vl, p_ctx->p_file, p_err);
      }
      g_clear_error(&p_err);
   } else {
      if (_is_current(p_vl, p_ctx->p_file)) {
         texturecache_put(p_vl->p_cache, p_ctx->p_file, p_tex);
         p_vl->p_ops->show_texture(p_vl->p_host, p_tex);
         _prefetch(p_vl);
      }
      g_object_unref(p_tex);
   }
   g_object_unref(p_ctx->p_file);
   _unref(p_vl);
   g_free(p_ctx);
}

/* Cancel the previous visible load and hand out a fresh cancellable. */
static void
_restart_visible_cancel(ViewLoad *p_vl) {
   g_cancellable_cancel(p_vl->p_cancel);
   g_clear_object(&p_vl->p_cancel);
   p_vl->p_cancel = g_cancellable_new();
}

void
viewload_load_current(ViewLoad *p_vl) {
   g_return_if_fail(p_vl != NULL);
   if (p_vl->b_disposed || p_vl->p_nav == NULL) {
      return;
   }
   GFile *p_cur = navigator_get_current(p_vl->p_nav);
   if (p_cur == NULL) {
      p_vl->p_ops->show_texture(p_vl->p_host, NULL);
      p_vl->p_ops->update_header(p_vl->p_host);
      return;
   }
   /* Cache hit: show immediately, no async load. texturecache_get validates
    * the file's mtime/size, so a file rewritten in place (external editor,
    * script) misses and is decoded afresh. */
   GdkTexture *p_cached = texturecache_get(p_vl->p_cache, p_cur);
   if (p_cached != NULL) {
      _restart_visible_cancel(p_vl); /* an in-flight load is now stale */
      p_vl->p_ops->show_texture(p_vl->p_host, p_cached);
      p_vl->p_ops->update_header(p_vl->p_host);
      _prefetch(p_vl);
      return;
   }
   /* Cache miss: one active load -- cancel the previous, start a new one.
    * Last-write-wins is enforced in the progress and finish callbacks via
    * the LoadCtx's source GFile. */
   _restart_visible_cancel(p_vl);
   LoadCtx *p_ctx = g_new(LoadCtx, 1);
   p_ctx->p_vl    = _ref(p_vl);
   p_ctx->p_file  = (GFile *)g_object_ref(p_cur);
   loader_load_async(p_cur, p_vl->p_cancel, _load_progress_cb, p_ctx,
                     _load_finish_cb, p_ctx);
   p_vl->p_ops->update_header(p_vl->p_host);
}

/* --- lifecycle ----------------------------------------------------------- */

void
viewload_dispose(ViewLoad *p_vl) {
   if (p_vl == NULL) {
      return;
   }
   p_vl->b_disposed = TRUE;
   p_vl->p_nav      = NULL;
   g_cancellable_cancel(p_vl->p_cancel);
   g_cancellable_cancel(p_vl->p_prefetch_cancel);
}

void
viewload_delete(ViewLoad *p_vl) {
   if (p_vl == NULL) {
      return;
   }
   viewload_dispose(p_vl);
   _unref(p_vl);
}
