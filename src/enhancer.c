/* enhancer.c — GEGL quick-enhance presets (optional, feature-gated). */
#include "enhancer.h"
#include "ggaze-config.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

struct Enhancer {
   GPtrArray *p_presets;
};

static void
_preset_free(gpointer p) {
   EnhancerPreset *d = (EnhancerPreset *)p;
   if (d) {
      g_free(d->c_name);
      g_free(d->c_graph);
      g_free(d);
   }
}

Enhancer *
enhancer_new(void) {
   Enhancer *e  = g_new0(Enhancer, 1);
   e->p_presets = g_ptr_array_new_with_free_func(_preset_free);
   /* Built-in presets (programmatic; graph=NULL). */
   const char *names[] = {
      "Auto-fix", "Brightness", "Contrast", "Saturation",
      "Warm",     "Cool",       "Sharpen",  "Denoise",
   };
   for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
      EnhancerPreset *p = g_new0(EnhancerPreset, 1);
      p->c_name         = g_strdup(names[i]);
      p->i_builtin      = 1;
      g_ptr_array_add(e->p_presets, p);
   }
   return e;
}

void
enhancer_delete(Enhancer *e) {
   if (!e)
      return;
   g_ptr_array_unref(e->p_presets);
   g_free(e);
}

/* Deep-copy p_src into a new GPtrArray of EnhancerPreset* (NULL/empty ->
 * empty array). Shared by enhancer_set_presets and the async apply path
 * (which snapshots the preset list before handing it to a worker thread, so
 * a concurrent enhancer_set_presets() cannot race it). */
static GPtrArray *
_presets_copy(const GPtrArray *p_src) {
   GPtrArray *p_out = g_ptr_array_new_with_free_func(_preset_free);
   if (p_src == NULL) {
      return (p_out);
   }
   for (guint i = 0; i < p_src->len; i++) {
      const EnhancerPreset *s  = g_ptr_array_index((GPtrArray *)p_src, i);
      EnhancerPreset       *np = g_new0(EnhancerPreset, 1);
      np->c_name               = g_strdup(s->c_name);
      np->c_graph              = g_strdup(s->c_graph);
      np->i_builtin            = s->i_builtin;
      g_ptr_array_add(p_out, np);
   }
   return (p_out);
}

void
enhancer_set_presets(Enhancer *e, const GPtrArray *p) {
   g_return_if_fail(e);
   GPtrArray *p_copy = _presets_copy(p);
   g_ptr_array_unref(e->p_presets);
   e->p_presets = p_copy;
}

const GPtrArray *
enhancer_get_presets(Enhancer *e) {
   return e ? e->p_presets : NULL;
}

/* Create the GEGL op node for a built-in preset (in p_graph), or NULL if the
 * name is unknown. */
static GeglNode *
_op_for_builtin(GeglNode *p_graph, const char *c_name) {
   if (g_str_equal(c_name, "Auto-fix")) {
      return (gegl_node_new_child(p_graph, "operation", "gegl:stretch-contrast",
                                  NULL));
   }
   if (g_str_equal(c_name, "Brightness")) {
      return (gegl_node_new_child(p_graph, "operation", "gegl:exposure",
                                  "exposure", 0.5, NULL));
   }
   if (g_str_equal(c_name, "Contrast")) {
      return (gegl_node_new_child(p_graph, "operation",
                                  "gegl:brightness-contrast", "contrast", 1.3,
                                  NULL));
   }
   if (g_str_equal(c_name, "Saturation")) {
      return (gegl_node_new_child(p_graph, "operation", "gegl:saturation",
                                  "scale", 1.4, NULL));
   }
   if (g_str_equal(c_name, "Warm")) {
      return (
         gegl_node_new_child(p_graph, "operation", "gegl:color-enhance", NULL));
   }
   if (g_str_equal(c_name, "Cool")) {
      return (gegl_node_new_child(p_graph, "operation", "gegl:exposure",
                                  "exposure", -0.3, NULL));
   }
   if (g_str_equal(c_name, "Sharpen")) {
      return (
         gegl_node_new_child(p_graph, "operation", "gegl:unsharp-mask", NULL));
   }
   if (g_str_equal(c_name, "Denoise")) {
      return (gegl_node_new_child(p_graph, "operation", "gegl:noise-reduction",
                                  NULL));
   }
   return (NULL);
}

/* Build and apply a GEGL graph for a built-in preset. */
static GeglBuffer *
_apply_builtin(GeglBuffer *p_in, const char *c_name, GError **p_err) {
   GeglNode *p_graph = gegl_node_new();
   GeglNode *p_src   = gegl_node_new_child(
      p_graph, "operation", "gegl:buffer-source", "buffer", p_in, NULL);
   GeglNode *p_op = _op_for_builtin(p_graph, c_name);

   if (p_op == NULL) {
      g_object_unref(p_graph);
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: unknown preset '%s'", c_name);
      return NULL;
   }

   gegl_node_link(p_src, p_op);
   GeglRectangle st_rect = {0, 0, gegl_buffer_get_width(p_in),
                            gegl_buffer_get_height(p_in)};
   GeglBuffer   *p_out   = gegl_buffer_new(&st_rect, babl_format("RGBA float"));
   GeglNode     *p_sink  = gegl_node_new_child(
      p_graph, "operation", "gegl:buffer-sink", "buffer", &p_out, NULL);
   gegl_node_link(p_op, p_sink);
   gegl_node_process(p_sink);
   g_object_unref(p_graph);
   return p_out;
}

/* Apply a chain of the enabled built-in presets (bit i of u_mask -> preset i)
 * in array order, composing them. Returns a new buffer, or NULL if no preset
 * is enabled or any op is unavailable. */
GeglBuffer *
enhancer_apply_chain(Enhancer *e, GeglBuffer *p_in, const GPtrArray *p_presets,
                     guint8 u_mask, GError **p_err) {
   (void)e;
   g_return_val_if_fail(p_in != NULL, NULL);
   g_return_val_if_fail(p_presets != NULL, NULL);
   GeglNode *p_graph = gegl_node_new();
   GeglNode *p_src   = gegl_node_new_child(
      p_graph, "operation", "gegl:buffer-source", "buffer", p_in, NULL);
   GeglNode *p_prev = p_src;
   gboolean  b_any  = FALSE;
   for (guint i = 0; i < p_presets->len && i < 8; i++) {
      if ((u_mask & (guint8)(1u << i)) == 0) {
         continue;
      }
      const EnhancerPreset *p_pr = g_ptr_array_index((GPtrArray *)p_presets, i);
      if (!p_pr->i_builtin) {
         g_object_unref(p_graph);
         g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "enhancer: user graph presets not chainable");
         return NULL;
      }
      GeglNode *p_op = _op_for_builtin(p_graph, p_pr->c_name);
      if (p_op == NULL) {
         g_object_unref(p_graph);
         g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "enhancer: unknown preset '%s'", p_pr->c_name);
         return NULL;
      }
      gegl_node_link(p_prev, p_op);
      p_prev = p_op;
      b_any  = TRUE;
   }
   if (!b_any) {
      g_object_unref(p_graph);
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: no preset enabled");
      return NULL;
   }
   GeglRectangle st_rect = {0, 0, gegl_buffer_get_width(p_in),
                            gegl_buffer_get_height(p_in)};
   GeglBuffer   *p_out   = gegl_buffer_new(&st_rect, babl_format("RGBA float"));
   GeglNode     *p_sink  = gegl_node_new_child(
      p_graph, "operation", "gegl:buffer-sink", "buffer", &p_out, NULL);
   gegl_node_link(p_prev, p_sink);
   gegl_node_process(p_sink);
   g_object_unref(p_graph);
   return p_out;
}

GeglBuffer *
enhancer_apply(Enhancer *e, GeglBuffer *p_in, const EnhancerPreset *p_preset,
               GError **p_err) {
   (void)e;
   g_return_val_if_fail(p_in != NULL, NULL);
   g_return_val_if_fail(p_preset != NULL, NULL);

   if (p_preset->i_builtin) {
      return _apply_builtin(p_in, p_preset->c_name, p_err);
   }

   /* User preset: parse a GEGL graph string. */
   if (p_preset->c_graph == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: empty graph for preset '%s'", p_preset->c_name);
      return NULL;
   }
   /* TODO: implement gegl_node_new_from_xml parsing. For M9 v1, built-in only.
    */
   g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "enhancer: user graph presets not yet supported");
   return NULL;
}

/* Pick the GEGL saver op and (for jpeg) quality from the output extension.
 * Returns the op name, or NULL if the extension is unsupported / the op is
 * not installed. ju0: never write JPEG bytes into a .png. */
static const char *
_saver_for_ext(GFile *p_out) {
   char       *c_base = g_file_get_basename(p_out);
   const char *c_dot  = strrchr(c_base, '.');
   const char *c_op   = NULL;
   if (c_dot != NULL) {
      if (g_ascii_strcasecmp(c_dot, ".jpg") == 0 ||
          g_ascii_strcasecmp(c_dot, ".jpeg") == 0) {
         c_op = "gegl:jpg-save";
      } else if (g_ascii_strcasecmp(c_dot, ".png") == 0) {
         c_op = "gegl:png-save";
      } else if (g_ascii_strcasecmp(c_dot, ".webp") == 0) {
         c_op = "gegl:webp-save";
      }
   }
   g_free(c_base);
   /* webp-save ships as a plugin; only promise it if installed. */
   if (c_op != NULL && !gegl_has_operation(c_op)) {
      return NULL;
   }
   return c_op;
}

/* Save p_buf to p_out with the format chosen by the output extension.
 * ju0: pick the saver by extension (jpg q95 / png / webp). ku0: verify the
 * save actually produced a non-empty, newer file instead of trusting a
 * pre-existing path. Returns TRUE on a real write. */
static gboolean
_save_buffer(GeglBuffer *p_buf, GFile *p_out, GError **p_err) {
   char *c_path = g_file_get_path(p_out);
   if (c_path == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: non-local export path");
      return FALSE;
   }
   const char *c_op = _saver_for_ext(p_out);
   if (c_op == NULL) {
      g_free(c_path);
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "enhancer: unsupported export extension");
      return FALSE;
   }
   GStatBuf  st_before;
   gboolean  b_existed = (g_stat(c_path, &st_before) == 0);
   GeglNode *p_graph   = gegl_node_new();
   GeglNode *p_src     = gegl_node_new_child(
      p_graph, "operation", "gegl:buffer-source", "buffer", p_buf, NULL);
   GeglNode *p_save;
   if (g_str_equal(c_op, "gegl:jpg-save")) {
      p_save = gegl_node_new_child(p_graph, "operation", c_op, "path", c_path,
                                   "quality", 95, NULL);
   } else {
      p_save =
         gegl_node_new_child(p_graph, "operation", c_op, "path", c_path, NULL);
   }
   gegl_node_link(p_src, p_save);
   gegl_node_process(p_save);
   g_object_unref(p_graph);
   GStatBuf st_after;
   gboolean b_ok = FALSE;
   if (g_stat(c_path, &st_after) == 0 && st_after.st_size > 0) {
      if (!b_existed || st_after.st_mtime != st_before.st_mtime ||
          st_after.st_size != st_before.st_size) {
         b_ok = TRUE;
      }
   }
   g_free(c_path);
   if (!b_ok) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: export produced no valid file");
      return FALSE;
   }
   return TRUE;
}

gboolean
enhancer_export(Enhancer *e, GeglBuffer *p_in, const EnhancerPreset *p_preset,
                GFile *p_out, GError **p_err) {
   (void)e;
   g_return_val_if_fail(p_in != NULL, FALSE);
   g_return_val_if_fail(p_out != NULL, FALSE);
   GeglBuffer *p_buf = enhancer_apply(NULL, p_in, p_preset, p_err);
   if (p_buf == NULL) {
      return FALSE;
   }
   gboolean b_ok = _save_buffer(p_buf, p_out, p_err);
   g_object_unref(p_buf);
   return b_ok;
}

/* Export p_in with the enabled-preset chain (u_mask) composed, to p_out. */
gboolean
enhancer_export_chain(Enhancer *e, GeglBuffer *p_in, const GPtrArray *p_presets,
                      guint8 u_mask, GFile *p_out, GError **p_err) {
   (void)e;
   g_return_val_if_fail(p_in != NULL, FALSE);
   g_return_val_if_fail(p_out != NULL, FALSE);
   g_return_val_if_fail(p_presets != NULL, FALSE);
   GeglBuffer *p_buf =
      enhancer_apply_chain(NULL, p_in, p_presets, u_mask, p_err);
   if (p_buf == NULL) {
      return FALSE;
   }
   gboolean b_ok = _save_buffer(p_buf, p_out, p_err);
   g_object_unref(p_buf);
   return b_ok;
}

#if GGAZE_HAVE_GEGL

GeglBuffer *
enhancer_load(GFile *p_file, GError **p_err) {
   g_return_val_if_fail(p_file != NULL, NULL);
   char *c_path = g_file_get_path(p_file);
   if (c_path == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: non-local load path");
      return NULL;
   }
   GeglBuffer *p_buf   = NULL;
   GeglNode   *p_graph = gegl_node_new();
   GeglNode   *p_load  = gegl_node_new_child(p_graph, "operation", "gegl:load",
                                             "path", c_path, NULL);
   GeglNode   *p_sink  = gegl_node_new_child(
      p_graph, "operation", "gegl:buffer-sink", "buffer", &p_buf, NULL);
   gegl_node_link(p_load, p_sink);
   gegl_node_process(p_sink);
   g_object_unref(p_graph);
   if (p_buf == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: failed to load %s", c_path);
      g_free(c_path);
      return NULL;
   }
   g_free(c_path);
   return p_buf;
}

GdkTexture *
enhancer_buffer_to_texture(GeglBuffer *p_buf, GError **p_err) {
   g_return_val_if_fail(p_buf != NULL, NULL);
   const GeglRectangle *p_rect = gegl_buffer_get_extent(p_buf);
   gint                 i_w    = p_rect->width;
   gint                 i_h    = p_rect->height;
   if (i_w <= 0 || i_h <= 0) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: empty buffer");
      return NULL;
   }
   const Babl *p_fmt    = babl_format("R'G'B'A u8");
   gint        i_stride = i_w * 4;
   gsize       u_size   = (gsize)i_stride * (gsize)i_h;
   gpointer    p_data   = g_malloc(u_size);
   gegl_buffer_get(p_buf, p_rect, 1.0, p_fmt, p_data, i_stride,
                   GEGL_ABYSS_NONE);
   GBytes     *p_bytes = g_bytes_new_take(p_data, u_size);
   GdkTexture *p_tex =
      gdk_memory_texture_new(i_w, i_h, GDK_MEMORY_R8G8B8A8, p_bytes, i_stride);
   g_bytes_unref(p_bytes);
   return p_tex;
}

/* --- async apply (tu0): off the caller's main thread ---------------------
 *
 * enhancer_load + enhancer_apply_chain + enhancer_buffer_to_texture are each
 * synchronous and CPU/IO-heavy; a GTK caller must not run them on the main
 * thread (AGENTS.md: "Decode runs in GTask threads"). This wraps all three
 * in a single GTask worker, mirroring loader.c's async wrapper. p_presets is
 * deep-copied into the task data before the worker starts so the caller's
 * Enhancer (whose preset array a concurrent Preferences apply could replace
 * via enhancer_set_presets) is never touched from the worker thread.
 */

typedef struct {
   Enhancer  *p_e;       /* borrowed; not touched off the calling thread */
   GFile     *p_file;    /* owned */
   GPtrArray *p_presets; /* owned deep copy (thread-safe snapshot) */
   guint8     u_mask;
} _AsyncApplyReq;

static void
_async_apply_req_free(_AsyncApplyReq *p_req) {
   if (p_req == NULL) {
      return;
   }
   g_clear_object(&p_req->p_file);
   g_clear_pointer(&p_req->p_presets, g_ptr_array_unref);
   g_free(p_req);
}

static void
_apply_chain_thread(GTask *p_task, gpointer p_src, gpointer p_task_data,
                    GCancellable *p_cancel) {
   (void)p_src;
   _AsyncApplyReq *p_req = (_AsyncApplyReq *)p_task_data;
   if (g_task_return_error_if_cancelled(p_task)) {
      return; /* superseded before the worker even started */
   }
   GError     *p_err = NULL;
   GeglBuffer *p_buf = enhancer_load(p_req->p_file, &p_err);
   GdkTexture *p_tex = NULL;
   if (p_buf != NULL) {
      GeglBuffer *p_enh = enhancer_apply_chain(
         p_req->p_e, p_buf, p_req->p_presets, p_req->u_mask, &p_err);
      if (p_enh != NULL) {
         p_tex = enhancer_buffer_to_texture(p_enh, &p_err);
         g_object_unref(p_enh);
      }
      g_object_unref(p_buf);
   }
   (void)p_cancel;
   if (p_tex == NULL) {
      g_task_return_error(p_task, p_err);
   } else {
      g_task_return_pointer(p_task, p_tex, (GDestroyNotify)g_object_unref);
   }
}

void
enhancer_apply_chain_async(Enhancer *p_e, GFile *p_file,
                           const GPtrArray *p_presets, guint8 u_mask,
                           GCancellable *p_cancel, GAsyncReadyCallback p_cb,
                           gpointer p_data) {
   g_return_if_fail(p_file != NULL);
   _AsyncApplyReq *p_req = g_new0(_AsyncApplyReq, 1);
   p_req->p_e            = p_e;
   p_req->p_file         = (GFile *)g_object_ref(p_file);
   p_req->p_presets      = _presets_copy(p_presets);
   p_req->u_mask         = u_mask;
   GTask *p_task         = g_task_new(p_file, p_cancel, p_cb, p_data);
   g_task_set_task_data(p_task, p_req, (GDestroyNotify)_async_apply_req_free);
   g_task_run_in_thread(p_task, _apply_chain_thread);
   g_object_unref(p_task);
}

GdkTexture *
enhancer_apply_chain_finish(GAsyncResult *p_res, GError **p_err) {
   g_return_val_if_fail(G_IS_TASK(p_res), NULL);
   return ((GdkTexture *)g_task_propagate_pointer((GTask *)p_res, p_err));
}

typedef struct {
   GFile     *p_file;
   GPtrArray *p_presets;
} _PreviewReq;

static void
_preview_req_free(_PreviewReq *p_req) {
   g_clear_object(&p_req->p_file);
   g_ptr_array_unref(p_req->p_presets);
   g_free(p_req);
}

static void
_texture_free(gpointer p_data) {
   if (p_data != NULL) {
      g_object_unref(p_data);
   }
}

static GeglBuffer *
_preview_downscale(GeglBuffer *p_in, GError **p_err) {
   gint i_w = gegl_buffer_get_width(p_in);
   gint i_h = gegl_buffer_get_height(p_in);
   if (i_w <= 0 || i_h <= 0) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: empty preview source");
      return (NULL);
   }
   gdouble   d_scale = MIN(1.0, 512.0 / (gdouble)MAX(i_w, i_h));
   gint      i_out_w = MAX(1, (gint)(i_w * d_scale));
   gint      i_out_h = MAX(1, (gint)(i_h * d_scale));
   GeglNode *p_graph = gegl_node_new();
   GeglNode *p_src   = gegl_node_new_child(
      p_graph, "operation", "gegl:buffer-source", "buffer", p_in, NULL);
   GeglNode *p_scale =
      gegl_node_new_child(p_graph, "operation", "gegl:scale-size", "x",
                          (gdouble)i_out_w, "y", (gdouble)i_out_h, NULL);
   GeglBuffer *p_out  = NULL;
   GeglNode   *p_sink = gegl_node_new_child(
      p_graph, "operation", "gegl:buffer-sink", "buffer", &p_out, NULL);
   gegl_node_link_many(p_src, p_scale, p_sink, NULL);
   gegl_node_process(p_sink);
   g_object_unref(p_graph);
   if (p_out == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: preview downscale failed");
   }
   return (p_out);
}

static void
_preview_thread(GTask *p_task, gpointer p_src, gpointer p_task_data,
                GCancellable *p_cancel) {
   (void)p_src;
   _PreviewReq *p_req = (_PreviewReq *)p_task_data;
   if (g_task_return_error_if_cancelled(p_task)) {
      return;
   }
   GError     *p_err  = NULL;
   GeglBuffer *p_full = enhancer_load(p_req->p_file, &p_err);
   if (p_full != NULL && g_cancellable_is_cancelled(p_cancel)) {
      g_object_unref(p_full);
      g_task_return_new_error(p_task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "enhancer preview cancelled");
      return;
   }
   GeglBuffer *p_small =
      p_full != NULL ? _preview_downscale(p_full, &p_err) : NULL;
   g_clear_object(&p_full);
   if (p_small == NULL) {
      g_task_return_error(p_task, p_err);
      return;
   }
   if (g_cancellable_is_cancelled(p_cancel)) {
      g_object_unref(p_small);
      g_task_return_new_error(p_task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "enhancer preview cancelled");
      return;
   }
   GPtrArray  *p_out      = g_ptr_array_new_with_free_func(_texture_free);
   GdkTexture *p_original = enhancer_buffer_to_texture(p_small, &p_err);
   if (p_original == NULL) {
      g_object_unref(p_small);
      g_ptr_array_unref(p_out);
      g_task_return_error(p_task, p_err);
      return;
   }
   g_ptr_array_add(p_out, p_original);
   for (guint i = 0; i < p_req->p_presets->len; i++) {
      if (g_cancellable_is_cancelled(p_cancel)) {
         g_object_unref(p_small);
         g_ptr_array_unref(p_out);
         g_task_return_new_error(p_task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                 "enhancer preview cancelled");
         return;
      }
      EnhancerPreset *p_preset  = g_ptr_array_index(p_req->p_presets, i);
      GError         *p_one_err = NULL;
      GeglBuffer     *p_effect =
         enhancer_apply(NULL, p_small, p_preset, &p_one_err);
      GdkTexture *p_tex = p_effect != NULL
                             ? enhancer_buffer_to_texture(p_effect, &p_one_err)
                             : NULL;
      g_clear_object(&p_effect);
      g_clear_error(&p_one_err);
      g_ptr_array_add(p_out, p_tex);
   }
   g_object_unref(p_small);
   g_task_return_pointer(p_task, p_out, (GDestroyNotify)g_ptr_array_unref);
}

void
enhancer_preview_thumbnails_async(Enhancer *p_e, GFile *p_file,
                                  const GPtrArray    *p_presets,
                                  GCancellable       *p_cancel,
                                  GAsyncReadyCallback p_cb, gpointer p_data) {
   (void)p_e;
   g_return_if_fail(G_IS_FILE(p_file));
   _PreviewReq *p_req = g_new0(_PreviewReq, 1);
   p_req->p_file      = (GFile *)g_object_ref(p_file);
   p_req->p_presets   = _presets_copy(p_presets);
   if (p_req->p_presets->len > 8) {
      g_ptr_array_set_size(p_req->p_presets, 8);
   }
   GTask *p_task = g_task_new(p_file, p_cancel, p_cb, p_data);
   g_task_set_task_data(p_task, p_req, (GDestroyNotify)_preview_req_free);
   g_task_run_in_thread(p_task, _preview_thread);
   g_object_unref(p_task);
}

GPtrArray *
enhancer_preview_thumbnails_finish(GAsyncResult *p_res, GError **p_err) {
   g_return_val_if_fail(G_IS_TASK(p_res), NULL);
   return ((GPtrArray *)g_task_propagate_pointer(G_TASK(p_res), p_err));
}

#endif /* GGAZE_HAVE_GEGL */
