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
 * of overwriting the viewer. It also carries the file's stamp as the cache
 * miss read it, BEFORE the decode started: the finished texture is cached
 * under that stamp, so a rewrite that lands mid-decode makes the next get
 * miss instead of stamping the old pixels as the new file (texturecache.h
 * texturecache_put_stamped). It carries the main context the load was
 * started from, which the decode thread queues its partials on, and the
 * cancellable the load was started with, which tells a queued partial
 * whether its load was superseded by the time it is dispatched (see
 * _load_progress_cb and _on_progress_main). The GTask always invokes the
 * finish callback (even on cancellation, and after the last progress
 * callback), which is the sole owner that frees the ctx (_load_ctx_free). */
typedef struct {
   ViewLoad     *p_vl;     /* ref'd; outlives the load */
   GFile        *p_file;   /* ref'd; the file being loaded */
   TextureStamp  t_stamp;  /* p_file's state before the decode began */
   GMainContext *p_main;   /* ref'd; the starting thread's main context */
   GCancellable *p_cancel; /* ref'd; the load's own cancellable */
} LoadCtx;

/* A partial texture hopping from the decode thread to the main thread on
 * an idle source. Owns a ref on everything it holds; _progress_idle_free
 * (the source's destroy notify) releases them whether or not the source
 * ever dispatched. */
typedef struct {
   ViewLoad     *p_vl;
   GFile        *p_file;
   GdkTexture   *p_tex;
   GCancellable *p_cancel; /* the visible load's; cancelled = superseded */
} ProgressIdle;

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

/* A LoadCtx for p_file, owning refs on p_vl, p_file and p_cancel (the
 * cancellable the load is started with); p_stamp is the miss stamp
 * texturecache_lookup() read (the one query this load costs). Built on the
 * main thread, so the context it captures is the same one the GTask
 * returns its result to. */
static LoadCtx *
_load_ctx_new(ViewLoad *p_vl, GFile *p_file, const TextureStamp *p_stamp,
              GCancellable *p_cancel) {
   LoadCtx *p_ctx  = g_new(LoadCtx, 1);
   p_ctx->p_vl     = _ref(p_vl);
   p_ctx->p_file   = (GFile *)g_object_ref(p_file);
   p_ctx->t_stamp  = *p_stamp;
   p_ctx->p_cancel = (GCancellable *)g_object_ref(p_cancel);
   /* Only a visible load reports partials, so only its ctx uses p_main; a
    * prefetch ctx refs it too, which is harmless and keeps one shape. */
   p_ctx->p_main = g_main_context_ref_thread_default();
   return (p_ctx);
}

/* Release everything a LoadCtx owns (each finish callback's last step). */
static void
_load_ctx_free(LoadCtx *p_ctx) {
   g_object_unref(p_ctx->p_file);
   g_object_unref(p_ctx->p_cancel);
   g_main_context_unref(p_ctx->p_main);
   _unref(p_ctx->p_vl);
   g_free(p_ctx);
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
         texturecache_put_stamped(p_ctx->p_vl->p_cache, p_ctx->p_file, p_tex,
                                  &p_ctx->t_stamp);
      }
      g_object_unref(p_tex);
   } else {
      g_clear_error(&p_err); /* a neighbour that fails to decode is not
                              * worth a message; the visible load reports */
   }
   _load_ctx_free(p_ctx);
}

/* Prefetch the next/previous images into the cache (not shown). Cancels the
 * previous prefetch round so at most two prefetch loads are in flight.
 *
 * An animated neighbour is prefetched with ALL its frames, deliberately
 * (task yb2, second review): the loader has no first-frame-only mode, and
 * one would put an entry in the cache that a hit could not play, so the
 * visible load would have to decode the file again on arrival -- no
 * saving, plus a second kind of cache entry to tell apart. What that
 * costs is bounded by the playback budget (loader/animation.h
 * GGAZE_ANIM_MAX_PIXELS: at most 128 MiB held per animation, about twice
 * that at the decode's peak), and a superseded round stops at the next
 * frame boundary, where the walk checks its cancellable. */
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
      GFile       *p_file = navigator_get_file(p_vl->p_nav, (guint)i_j);
      TextureStamp t_stamp;
      if (p_file != NULL &&
          texturecache_lookup(p_vl->p_cache, p_file, &t_stamp) == NULL) {
         LoadCtx *p_ctx =
            _load_ctx_new(p_vl, p_file, &t_stamp, p_vl->p_prefetch_cancel);
         loader_load_async(p_file, p_vl->p_prefetch_cancel, NULL, NULL,
                           _prefetch_finish_cb, p_ctx);
      }
   }
}

/* --- visible load -------------------------------------------------------- */

/* The partial's idle source destroy notify: drop every ref it holds. Runs
 * after the dispatch, or on its own if the source is destroyed unrun. */
static void
_progress_idle_free(gpointer p_data) {
   ProgressIdle *p_pi = (ProgressIdle *)p_data;
   g_object_unref(p_pi->p_tex);
   g_object_unref(p_pi->p_file);
   g_object_unref(p_pi->p_cancel);
   _unref(p_pi->p_vl);
   g_free(p_pi);
}

/* Last-write-wins, per LOAD and not only per file: show the partial only
 * if its load was not superseded AND its file is still the current one;
 * the full result replaces it in _load_finish_cb. The file check alone let
 * a stale partial through whenever the superseding step left the same file
 * current: A->B->A answered from the cache (the full picture shown, then
 * the old load's queued partial replaced it with a low-res one that
 * stayed), or a reload of the same file (the old load's partial, emitted
 * just before it saw the cancel, landing after the new load's full
 * texture). Every superseding step cancels the load's cancellable, always
 * on this thread: a new load and a cache hit through
 * _restart_visible_cancel, dispose directly. So a cancelled one here means
 * a stale partial. Through
 * show_partial, not show_texture: a low-res stand-in is not the file's
 * picture, and a host that remembers what it showed must not take it for
 * one. The source's destroy notify frees p_data. */
static gboolean
_on_progress_main(gpointer p_data) {
   ProgressIdle *p_pi = (ProgressIdle *)p_data;
   if (!g_cancellable_is_cancelled(p_pi->p_cancel) &&
       _is_current(p_pi->p_vl, p_pi->p_file)) {
      p_pi->p_vl->p_ops->show_partial(p_pi->p_vl->p_host, p_pi->p_tex);
   }
   return (G_SOURCE_REMOVE);
}

/* Decode-thread progress callback: hop to the main thread, ALWAYS through
 * an idle source on the load's main context. Not g_main_context_invoke():
 * that runs the function right here, on the decode thread, whenever it can
 * acquire the context -- i.e. whenever no thread owns it at that instant.
 * g_application_run() owns the default context for its whole run, so the
 * app itself never hit that; but a caller that iterates the context by
 * hand (every integration test's drain loop) leaves it unowned between
 * iterations, and a partial then reached the viewer from the decode
 * thread: a GTK call off the main thread (AGENTS.md), and a picture change
 * the main thread never iterated for (task fe2: in
 * /window/info_no_plot_while_loading B's partial replaced A in the middle
 * of the synchronous `win.next`). Queued, a partial lands only when the
 * main thread dispatches it. */
static void
_load_progress_cb(GdkTexture *p_partial, gpointer p_data) {
   LoadCtx      *p_ctx = (LoadCtx *)p_data;
   ProgressIdle *p_pi  = g_new(ProgressIdle, 1);
   p_pi->p_vl          = _ref(p_ctx->p_vl);
   p_pi->p_file        = (GFile *)g_object_ref(p_ctx->p_file);
   p_pi->p_tex         = (GdkTexture *)g_object_ref(p_partial);
   p_pi->p_cancel      = (GCancellable *)g_object_ref(p_ctx->p_cancel);
   GSource *p_src      = g_idle_source_new();
   g_source_set_priority(p_src, G_PRIORITY_DEFAULT);
   g_source_set_callback(p_src, _on_progress_main, p_pi, _progress_idle_free);
   g_source_set_static_name(p_src, "[ggaze] viewload partial");
   g_source_attach(p_src, p_ctx->p_main);
   g_source_unref(p_src);
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
   /* GTask's default check-cancellable already turns a result that lost
    * the race with a cancel into CANCELLED; checking the load's own
    * cancellable too keeps a superseded same-file load's older pixels off
    * the screen and out of the cache even if that default ever changes. */
   if (p_tex != NULL && g_cancellable_is_cancelled(p_ctx->p_cancel)) {
      g_clear_object(&p_tex);
      p_err =
         g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "superseded");
   }
   if (p_tex == NULL) {
      if (!g_error_matches(p_err, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
          _is_current(p_vl, p_ctx->p_file)) {
         _report_failure(p_vl, p_ctx->p_file, p_err);
      }
      g_clear_error(&p_err);
   } else {
      if (_is_current(p_vl, p_ctx->p_file)) {
         texturecache_put_stamped(p_vl->p_cache, p_ctx->p_file, p_tex,
                                  &p_ctx->t_stamp);
         p_vl->p_ops->show_texture(p_vl->p_host, p_tex);
         _prefetch(p_vl);
      }
      g_object_unref(p_tex);
   }
   _load_ctx_free(p_ctx);
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
   /* Cache hit: show immediately, no async load. The lookup validates the
    * file's stamp (mtime to the nanosecond, size, inode), so a file
    * rewritten in place (external editor, script) -- even within the same
    * second to the same byte count, clock tick permitting -- misses and is
    * decoded afresh; a miss hands out the stamp it read, which the decode
    * below is cached under (read before the decode, never after). */
   TextureStamp t_stamp;
   GdkTexture  *p_cached = texturecache_lookup(p_vl->p_cache, p_cur, &t_stamp);
   if (p_cached != NULL) {
      /* An in-flight load is now stale: cancelling it also drops a partial
       * it already queued, which would otherwise pass _is_current (same
       * file) and replace this full picture (_on_progress_main). */
      _restart_visible_cancel(p_vl);
      p_vl->p_ops->show_texture(p_vl->p_host, p_cached);
      p_vl->p_ops->update_header(p_vl->p_host);
      _prefetch(p_vl);
      return;
   }
   /* Cache miss: one active load -- cancel the previous, start a new one.
    * Last-write-wins is enforced in the progress and finish callbacks by
    * both the LoadCtx's source GFile and the load's own cancellable, which
    * the cancel here marks superseded (a same-file reload keeps the file
    * current, so only the cancellable tells the old load apart). */
   _restart_visible_cancel(p_vl);
   LoadCtx *p_ctx = _load_ctx_new(p_vl, p_cur, &t_stamp, p_vl->p_cancel);
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
