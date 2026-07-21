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

void
enhancer_set_presets(Enhancer *e, const GPtrArray *p) {
   g_return_if_fail(e);
   g_ptr_array_set_size(e->p_presets, 0);
   if (!p)
      return;
   for (guint i = 0; i < p->len; i++) {
      const EnhancerPreset *s  = g_ptr_array_index((GPtrArray *)p, i);
      EnhancerPreset       *np = g_new0(EnhancerPreset, 1);
      np->c_name               = g_strdup(s->c_name);
      np->c_graph              = g_strdup(s->c_graph);
      np->i_builtin            = s->i_builtin;
      g_ptr_array_add(e->p_presets, np);
   }
}

const GPtrArray *
enhancer_get_presets(Enhancer *e) {
   return e ? e->p_presets : NULL;
}

/* Build and apply a GEGL graph for a built-in preset. */
static GeglBuffer *
_apply_builtin(GeglBuffer *p_in, const char *c_name, GError **p_err) {
   GeglNode *p_graph = gegl_node_new();
   GeglNode *p_src   = gegl_node_new_child(
      p_graph, "operation", "gegl:buffer-source", "buffer", p_in, NULL);
   GeglNode *p_op = NULL;

   if (g_str_equal(c_name, "Auto-fix")) {
      p_op = gegl_node_new_child(p_graph, "operation", "gegl:stretch-contrast",
                                 NULL);
   } else if (g_str_equal(c_name, "Brightness")) {
      p_op = gegl_node_new_child(p_graph, "operation", "gegl:exposure",
                                 "exposure", 0.5, NULL);
   } else if (g_str_equal(c_name, "Contrast")) {
      p_op =
         gegl_node_new_child(p_graph, "operation", "gegl:brightness-contrast",
                             "contrast", 1.3, NULL);
   } else if (g_str_equal(c_name, "Saturation")) {
      p_op = gegl_node_new_child(p_graph, "operation", "gegl:saturation",
                                 "scale", 1.4, NULL);
   } else if (g_str_equal(c_name, "Warm")) {
      p_op =
         gegl_node_new_child(p_graph, "operation", "gegl:color-enhance", NULL);
   } else if (g_str_equal(c_name, "Cool")) {
      p_op = gegl_node_new_child(p_graph, "operation", "gegl:exposure",
                                 "exposure", -0.3, NULL);
   } else if (g_str_equal(c_name, "Sharpen")) {
      p_op =
         gegl_node_new_child(p_graph, "operation", "gegl:unsharp-mask", NULL);
   } else if (g_str_equal(c_name, "Denoise")) {
      p_op = gegl_node_new_child(p_graph, "operation", "gegl:noise-reduction",
                                 NULL);
   }

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

gboolean
enhancer_export(Enhancer *e, GeglBuffer *p_in, const EnhancerPreset *p_preset,
                GFile *p_out, GError **p_err) {
   (void)e;
   g_return_val_if_fail(p_in != NULL, FALSE);
   g_return_val_if_fail(p_out != NULL, FALSE);

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

   /* Apply the preset first. */
   GeglBuffer *p_buf = enhancer_apply(NULL, p_in, p_preset, p_err);
   if (p_buf == NULL) {
      g_free(c_path);
      return FALSE;
   }

   /* ku0: capture the save's real success. Record the output's pre-state so
    * a pre-existing file can't masquerade as a successful save. */
   GStatBuf st_before;
   gboolean b_existed = (g_stat(c_path, &st_before) == 0);

   GeglNode *p_graph = gegl_node_new();
   GeglNode *p_src   = gegl_node_new_child(
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
   g_object_unref(p_buf);

   /* Verify the save actually produced a non-empty file newer than before. */
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
   gint i_w = gegl_buffer_get_width(p_buf);
   gint i_h = gegl_buffer_get_height(p_buf);
   if (i_w <= 0 || i_h <= 0) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: empty buffer");
      return NULL;
   }
   const Babl   *p_fmt    = babl_format("R'G'B'A u8");
   gint          i_stride = i_w * 4;
   gsize         u_size   = (gsize)i_stride * (gsize)i_h;
   gpointer      p_data   = g_malloc(u_size);
   GeglRectangle rect     = {0, 0, i_w, i_h};
   gegl_buffer_get(p_buf, &rect, 1.0, p_fmt, p_data, i_stride, GEGL_ABYSS_NONE);
   GBytes     *p_bytes = g_bytes_new_take(p_data, u_size);
   GdkTexture *p_tex   = gdk_memory_texture_new(
      i_w, i_h, GDK_MEMORY_R8G8B8A8_PREMULTIPLIED, p_bytes, i_stride);
   g_bytes_unref(p_bytes);
   return p_tex;
}

#endif /* GGAZE_HAVE_GEGL */