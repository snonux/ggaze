/* enhancer.c — GEGL quick-enhance presets (optional, feature-gated). */
#include "enhancer.h"

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

gboolean
enhancer_export(Enhancer *e, GeglBuffer *p_in, const EnhancerPreset *p_preset,
                GFile *p_out, GError **p_err) {
   (void)e;
   (void)p_preset;
   g_return_val_if_fail(p_in != NULL, FALSE);
   g_return_val_if_fail(p_out != NULL, FALSE);

   /* Apply the preset first. */
   GeglBuffer *p_buf = enhancer_apply(NULL, p_in, p_preset, p_err);
   if (p_buf == NULL)
      return FALSE;

   char *c_path = g_file_get_path(p_out);
   if (c_path == NULL) {
      g_object_unref(p_buf);
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: non-local export path");
      return FALSE;
   }

   GeglNode *p_graph = gegl_node_new();
   GeglNode *p_src   = gegl_node_new_child(
      p_graph, "operation", "gegl:buffer-source", "buffer", p_buf, NULL);
   GeglNode *p_save = gegl_node_new_child(p_graph, "operation", "gegl:jpg-save",
                                          "path", c_path, NULL);
   gegl_node_link(p_src, p_save);
   gegl_node_process(p_save);
   gboolean b_ok = g_file_test(c_path, G_FILE_TEST_EXISTS);

   g_object_unref(p_graph);
   g_object_unref(p_buf);
   g_free(c_path);
   return b_ok;
}