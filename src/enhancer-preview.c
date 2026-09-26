/*:*
 * ggaze — the live enhance preview's source and render (GEGL builds)
 *
 * See enhancer-gegl.h "the live preview" (8l2, decision #53). A SOURCE is
 * the image decoded once -- through enhancer.c's own load path, so a
 * colour-managed file stays managed -- and scaled down to what the
 * viewport shows (preview-scale.h); the controller keeps it while the
 * image stays, and every render runs the chain on it: the transform
 * scaled onto it (transform_scale), each preset's pixel lengths scaled
 * with it (enhancer_priv_run_chain), the texture tagged with the size the
 * export would have (logical-size.h) so the viewer and the tools keep
 * measuring the image. The card thumbnails are cut from the same source
 * (a thumbnail-sized copy made with it), so opening the panel decodes
 * nothing of its own either.
 *
 * Measured on this machine (8 threads, gegl 0.4): scaling a 64 MP decode
 * down to a 1.5 MP source ~0.26 s; a Brightness render on it ~15 ms (the
 * first chain of a process pays ~0.25 s of babl setup once); where the
 * full-resolution preview took ~7-9 s per key.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "enhancer-gegl.h"

#if GGAZE_HAVE_GEGL

#include <math.h>

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

#include "enhancer-private.h"
#include "logical-size.h"
#include "preview-scale.h"
#include "transform.h"

struct EnhancerSource {
   GFile      *p_file;     /* the file it was built from */
   GeglBuffer *p_buf;      /* the image at d_scale, in the decode's space */
   GeglBuffer *p_thumb;    /* ... and at d_thumb_scale (the cards) */
   GdkTexture *p_original; /* p_buf shown, standing for the original's size
                            * (NULL at scale 1: the decode IS the original).
                            * Built on first demand, on the caller's thread
                            * (enhancer_source_get_original): only a Space
                            * press needs it */
   gint    i_orig_w;       /* the original's upright size */
   gint    i_orig_h;
   gdouble d_scale;        /* p_buf's width / i_orig_w */
   gdouble d_want;         /* the scale it was built for (preview_scale_
                            * for_view): what a later view is compared with */
   gdouble  d_thumb_scale; /* p_thumb's width / i_orig_w */
   gboolean b_managed;     /* decoded colour-managed */
};

/* Test seam: a sleep before each render (enhancer_test_set_render_delay). */
static guint u_render_delay_ms = 0;

void
enhancer_test_set_render_delay(guint u_ms) {
   g_atomic_int_set(&u_render_delay_ms, u_ms);
}

/* Test seam: fail every render (enhancer_test_set_render_fail). */
static gint i_render_fail = 0;

void
enhancer_test_set_render_fail(gboolean b_fail) {
   g_atomic_int_set(&i_render_fail, b_fail ? 1 : 0);
}

/* --- the source ------------------------------------------------------------
 */

typedef struct {
   GFile      *p_file;    /* owned */
   GdkTexture *p_decoded; /* owned ref, nullable */
   PreviewView t_view;
} _SourceReq;

static void
_source_req_free(_SourceReq *p_req) {
   g_clear_object(&p_req->p_file);
   g_clear_object(&p_req->p_decoded);
   g_free(p_req);
}

void
enhancer_source_delete(EnhancerSource *p_src) {
   if (p_src == NULL) {
      return;
   }
   g_clear_object(&p_src->p_file);
   g_clear_object(&p_src->p_buf);
   g_clear_object(&p_src->p_thumb);
   g_clear_object(&p_src->p_original);
   g_free(p_src);
}

/* p_full (consumed) at the scale p_view wants for it: itself when that is
 * 1, else a scaled copy. *pd_want receives that scale. */
static GeglBuffer *
_fit_to_view(GeglBuffer *p_full, const PreviewView *p_view, gdouble *pd_want) {
   *pd_want = preview_scale_for_view(gegl_buffer_get_width(p_full),
                                     gegl_buffer_get_height(p_full), p_view);
   if (*pd_want >= 1.0) {
      return (p_full);
   }
   GeglBuffer *p_small = enhancer_priv_downscale(p_full, *pd_want);
   g_object_unref(p_full);
   return (p_small);
}

/* The decode behind the source, scaled: the managed path first (as
 * enhancer_load), then -- the loader path -- the viewer's decode when one
 * was handed over (no second decode), else ggaze's loader. Sets the
 * original's size, the wanted scale and whether it was managed. NULL with
 * p_err on a failed load or a cancellation. */
static GeglBuffer *
_source_decode(const _SourceReq *p_req, GCancellable *p_cancel,
               EnhancerSource *p_src, GError **p_err) {
   GeglBuffer *p_full = enhancer_priv_load_managed(p_req->p_file, p_cancel);
   p_src->b_managed   = p_full != NULL;
   if (p_full == NULL &&
       g_cancellable_set_error_if_cancelled(p_cancel, p_err)) {
      return (NULL);
   }
   if (p_full == NULL && p_req->p_decoded != NULL) {
      GdkTexture *p_tex = p_req->p_decoded;
      p_src->i_orig_w   = gdk_texture_get_width(p_tex);
      p_src->i_orig_h   = gdk_texture_get_height(p_tex);
      p_src->d_want = preview_scale_for_view(p_src->i_orig_w, p_src->i_orig_h,
                                             &p_req->t_view);
      return (enhancer_priv_buffer_from_texture(p_tex, p_src->d_want));
   }
   if (p_full == NULL) {
      p_full = enhancer_priv_load_via_loader(p_req->p_file, p_cancel, p_err);
   }
   if (p_full == NULL) {
      return (NULL);
   }
   p_src->i_orig_w = gegl_buffer_get_width(p_full);
   p_src->i_orig_h = gegl_buffer_get_height(p_full);
   return (_fit_to_view(p_full, &p_req->t_view, &p_src->d_want));
}

/* Complete p_src around its scaled buffer: the thumbnail copy and the
 * scales actually reached (rounded sides). The texture hold-Space compares
 * against is NOT made here -- see enhancer_source_get_original. */
static void
_source_finish_build(EnhancerSource *p_src) {
   gint i_w        = gegl_buffer_get_width(p_src->p_buf);
   gint i_h        = gegl_buffer_get_height(p_src->p_buf);
   p_src->d_scale  = (gdouble)i_w / p_src->i_orig_w;
   gdouble d_thumb = preview_scale_thumb(i_w, i_h);
   p_src->p_thumb  = d_thumb < 1.0
                        ? enhancer_priv_downscale(p_src->p_buf, d_thumb)
                        : g_object_ref(p_src->p_buf);
   p_src->d_thumb_scale =
      (gdouble)gegl_buffer_get_width(p_src->p_thumb) / p_src->i_orig_w;
}

static void
_source_thread(GTask *p_task, gpointer p_obj, gpointer p_task_data,
               GCancellable *p_cancel) {
   (void)p_obj;
   const _SourceReq *p_req = p_task_data;
   if (g_task_return_error_if_cancelled(p_task)) {
      return;
   }
   GError         *p_err = NULL;
   EnhancerSource *p_src = g_new0(EnhancerSource, 1);
   p_src->p_buf          = _source_decode(p_req, p_cancel, p_src, &p_err);
   if (p_src->p_buf == NULL) {
      enhancer_source_delete(p_src);
      g_task_return_error(p_task, p_err);
      return;
   }
   p_src->p_file = g_object_ref(p_req->p_file);
   _source_finish_build(p_src);
   g_task_return_pointer(p_task, p_src, (GDestroyNotify)enhancer_source_delete);
}

void
enhancer_source_new_async(GFile *p_file, GdkTexture *p_decoded,
                          const PreviewView *p_view, GCancellable *p_cancel,
                          GAsyncReadyCallback p_cb, gpointer p_data) {
   g_return_if_fail(G_IS_FILE(p_file) && p_view != NULL);
   _SourceReq *p_req = g_new0(_SourceReq, 1);
   p_req->p_file     = g_object_ref(p_file);
   p_req->p_decoded  = p_decoded != NULL ? g_object_ref(p_decoded) : NULL;
   p_req->t_view     = *p_view;
   GTask *p_task     = g_task_new(p_file, p_cancel, p_cb, p_data);
   g_task_set_task_data(p_task, p_req, (GDestroyNotify)_source_req_free);
   g_task_run_in_thread(p_task, _source_thread);
   g_object_unref(p_task);
}

EnhancerSource *
enhancer_source_new_finish(GAsyncResult *p_res, GError **p_err) {
   g_return_val_if_fail(G_IS_TASK(p_res), NULL);
   return (g_task_propagate_pointer(G_TASK(p_res), p_err));
}

GFile *
enhancer_source_get_file(const EnhancerSource *p_src) {
   g_return_val_if_fail(p_src != NULL, NULL);
   return (p_src->p_file);
}

void
enhancer_source_get_orig_size(const EnhancerSource *p_src, gint *p_w,
                              gint *p_h) {
   g_return_if_fail(p_src != NULL && p_w != NULL && p_h != NULL);
   *p_w = p_src->i_orig_w;
   *p_h = p_src->i_orig_h;
}

gdouble
enhancer_source_get_scale(const EnhancerSource *p_src) {
   g_return_val_if_fail(p_src != NULL, 1.0);
   return (p_src->d_scale);
}

gboolean
enhancer_source_is_managed(const EnhancerSource *p_src) {
   g_return_val_if_fail(p_src != NULL, FALSE);
   return (p_src->b_managed);
}

/* Lazily (8l2 review): a session that never holds Space never pays for a
 * second copy of the source's pixels. The conversion is of the scaled
 * buffer only -- a few megapixels whatever the photo's size: measured at
 * ~2 ms for a 64 MP photo's 1600x1200 source (a 1280x800 view), where
 * building the source takes ~0.9 s -- so doing it on the first press, on
 * the main thread, keeps the compare instant. A render or card batch
 * reading the same buffer in a worker meanwhile is fine: GEGL buffers take
 * concurrent readers. A failed conversion leaves it NULL and is tried
 * again on the next call. */
GdkTexture *
enhancer_source_get_original(EnhancerSource *p_src) {
   g_return_val_if_fail(p_src != NULL, NULL);
   if (p_src->p_original == NULL && p_src->d_scale < 1.0) {
      p_src->p_original = enhancer_buffer_to_texture(p_src->p_buf, NULL);
      if (p_src->p_original != NULL) {
         logical_size_set(p_src->p_original, p_src->i_orig_w, p_src->i_orig_h);
      }
   }
   return (p_src->p_original);
}

gboolean
enhancer_source_has_original(const EnhancerSource *p_src) {
   g_return_val_if_fail(p_src != NULL, FALSE);
   return (p_src->p_original != NULL);
}

gboolean
enhancer_source_serves(const EnhancerSource *p_src, GFile *p_file,
                       const PreviewView *p_view) {
   g_return_val_if_fail(p_src != NULL && p_view != NULL, FALSE);
   if (p_file == NULL || !g_file_equal(p_src->p_file, p_file)) {
      return (FALSE);
   }
   return (preview_scale_covers(
      p_src->d_want,
      preview_scale_for_view(p_src->i_orig_w, p_src->i_orig_h, p_view)));
}

/* --- the render ------------------------------------------------------------
 */

typedef struct {
   GeglBuffer *p_buf;     /* owned ref: the source's buffer */
   GPtrArray  *p_presets; /* owned snapshot */
   guint32     u_mask;
   Transform   t_xf; /* in the original's pixels (a snapshot) */
   gint        i_orig_w;
   gint        i_orig_h;
   gdouble     d_scale;
} _RenderReq;

static void
_render_req_free(_RenderReq *p_req) {
   g_clear_object(&p_req->p_buf);
   g_clear_pointer(&p_req->p_presets, g_ptr_array_unref);
   g_free(p_req);
}

/* The chain on the source, as a texture standing for the export's size. */
static GdkTexture *
_render(const _RenderReq *p_req, GError **p_err) {
   Transform t_on_src;
   transform_scale(&p_req->t_xf, p_req->i_orig_w, p_req->i_orig_h,
                   gegl_buffer_get_width(p_req->p_buf),
                   gegl_buffer_get_height(p_req->p_buf), &t_on_src);
   GeglBuffer *p_out =
      enhancer_priv_run_chain(p_req->p_buf, p_req->p_presets, p_req->u_mask,
                              &t_on_src, p_req->d_scale, p_err);
   if (p_out == NULL) {
      return (NULL);
   }
   GdkTexture *p_tex = enhancer_buffer_to_texture(p_out, p_err);
   g_object_unref(p_out);
   if (p_tex != NULL) {
      gdouble d_w, d_h;
      transform_output_size(&p_req->t_xf, p_req->i_orig_w, p_req->i_orig_h,
                            &d_w, &d_h);
      logical_size_set(p_tex, MAX(1, (gint)lround(d_w)),
                       MAX(1, (gint)lround(d_h)));
   }
   return (p_tex);
}

static void
_render_thread(GTask *p_task, gpointer p_obj, gpointer p_task_data,
               GCancellable *p_cancel) {
   (void)p_obj;
   (void)p_cancel;
   guint u_delay = (guint)g_atomic_int_get(&u_render_delay_ms);
   if (u_delay > 0) {
      g_usleep((gulong)u_delay * 1000);
   }
   if (g_task_return_error_if_cancelled(p_task)) {
      return; /* superseded before it started */
   }
   GError     *p_err = NULL;
   GdkTexture *p_tex = NULL;
   if (g_atomic_int_get(&i_render_fail) != 0) {
      g_set_error(&p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: render failed (test seam)");
   } else {
      p_tex = _render(p_task_data, &p_err);
   }
   if (p_tex == NULL) {
      g_task_return_error(p_task, p_err);
   } else {
      g_task_return_pointer(p_task, p_tex, g_object_unref);
   }
}

void
enhancer_source_render_async(const EnhancerSource *p_src,
                             const GPtrArray *p_presets, guint32 u_mask,
                             const Transform *p_xf, GCancellable *p_cancel,
                             GAsyncReadyCallback p_cb, gpointer p_data) {
   g_return_if_fail(p_src != NULL && p_presets != NULL);
   _RenderReq *p_req = g_new0(_RenderReq, 1);
   p_req->p_buf      = g_object_ref(p_src->p_buf);
   p_req->p_presets  = enhancer_priv_presets_copy(p_presets);
   p_req->u_mask     = u_mask;
   p_req->i_orig_w   = p_src->i_orig_w;
   p_req->i_orig_h   = p_src->i_orig_h;
   p_req->d_scale    = p_src->d_scale;
   enhancer_priv_snapshot_transform(&p_req->t_xf, p_xf);
   GTask *p_task = g_task_new(NULL, p_cancel, p_cb, p_data);
   g_task_set_task_data(p_task, p_req, (GDestroyNotify)_render_req_free);
   g_task_run_in_thread(p_task, _render_thread);
   g_object_unref(p_task);
}

GdkTexture *
enhancer_source_render_finish(GAsyncResult *p_res, GError **p_err) {
   g_return_val_if_fail(G_IS_TASK(p_res), NULL);
   return (g_task_propagate_pointer(G_TASK(p_res), p_err));
}

/* --- the card thumbnails ---------------------------------------------------
 */

typedef struct {
   GeglBuffer *p_thumb;   /* owned ref: the source's thumbnail copy */
   GPtrArray  *p_presets; /* owned snapshot */
   gdouble     d_scale;   /* p_thumb's scale of the original */
} _ThumbReq;

static void
_thumb_req_free(_ThumbReq *p_req) {
   g_clear_object(&p_req->p_thumb);
   g_clear_pointer(&p_req->p_presets, g_ptr_array_unref);
   g_free(p_req);
}

static void
_texture_free(gpointer p_data) {
   if (p_data != NULL) {
      g_object_unref(p_data);
   }
}

/* One preset alone on the thumbnail source, or NULL when it cannot be
 * applied (the card then stays empty). */
static GdkTexture *
_thumb_one(const _ThumbReq *p_req, const EnhancerPreset *p_preset) {
   GPtrArray *p_one = g_ptr_array_new();
   g_ptr_array_add(p_one, (gpointer)p_preset);
   GError     *p_err = NULL;
   GeglBuffer *p_out = enhancer_priv_run_chain(p_req->p_thumb, p_one, 1, NULL,
                                               p_req->d_scale, &p_err);
   g_ptr_array_unref(p_one);
   GdkTexture *p_tex =
      p_out != NULL ? enhancer_buffer_to_texture(p_out, &p_err) : NULL;
   g_clear_object(&p_out);
   g_clear_error(&p_err);
   return (p_tex);
}

static void
_thumb_thread(GTask *p_task, gpointer p_obj, gpointer p_task_data,
              GCancellable *p_cancel) {
   (void)p_obj;
   const _ThumbReq *p_req = p_task_data;
   if (g_task_return_error_if_cancelled(p_task)) {
      return;
   }
   GError     *p_err  = NULL;
   GdkTexture *p_orig = enhancer_buffer_to_texture(p_req->p_thumb, &p_err);
   if (p_orig == NULL) {
      g_task_return_error(p_task, p_err);
      return;
   }
   GPtrArray *p_out = g_ptr_array_new_with_free_func(_texture_free);
   g_ptr_array_add(p_out, p_orig);
   for (guint u = 0; u < p_req->p_presets->len; u++) {
      if (g_cancellable_is_cancelled(p_cancel)) {
         g_ptr_array_unref(p_out);
         g_task_return_new_error(p_task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                 "enhancer preview cancelled");
         return;
      }
      g_ptr_array_add(
         p_out, _thumb_one(p_req, g_ptr_array_index(p_req->p_presets, u)));
   }
   g_task_return_pointer(p_task, p_out, (GDestroyNotify)g_ptr_array_unref);
}

void
enhancer_preview_thumbnails_async(const EnhancerSource *p_src,
                                  const GPtrArray      *p_presets,
                                  GCancellable         *p_cancel,
                                  GAsyncReadyCallback p_cb, gpointer p_data) {
   g_return_if_fail(p_src != NULL && p_presets != NULL);
   _ThumbReq *p_req = g_new0(_ThumbReq, 1);
   p_req->p_thumb   = g_object_ref(p_src->p_thumb);
   p_req->p_presets = enhancer_priv_presets_copy(p_presets); /* the mask's */
   p_req->d_scale   = p_src->d_thumb_scale;
   GTask *p_task    = g_task_new(NULL, p_cancel, p_cb, p_data);
   g_task_set_task_data(p_task, p_req, (GDestroyNotify)_thumb_req_free);
   g_task_run_in_thread(p_task, _thumb_thread);
   g_object_unref(p_task);
}

GPtrArray *
enhancer_preview_thumbnails_finish(GAsyncResult *p_res, GError **p_err) {
   g_return_val_if_fail(G_IS_TASK(p_res), NULL);
   return ((GPtrArray *)g_task_propagate_pointer(G_TASK(p_res), p_err));
}

#endif /* GGAZE_HAVE_GEGL */
