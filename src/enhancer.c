/*:*
 * ggaze — GEGL quick-enhance presets (optional, feature-gated)
 *
 * The preset engine: a list of EnhancerPreset (built-ins from BUILTINS[]
 * below plus the user's graph presets from Preferences) and the GEGL
 * operations that apply, chain, preview and export them. Every operation is
 * a pure function of its arguments; the Enhancer instance only owns the
 * list. A user preset is a whitespace-separated chain of GEGL operations,
 * each "op:name" optionally followed by "prop=value" pairs, e.g.
 * "gegl:saturation scale=1.3 gegl:unsharp-mask std-dev=1.5".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "enhancer.h"

#include <math.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "enhancer-gegl.h"
#include "ggaze-config.h"
#include "loader/loader.h" /* orientation-aware load -> upright texture */
#include "pathutil.h"
#include "transform.h"

struct Enhancer {
   GPtrArray *p_presets;
};

/* --- built-in preset table ------------------------------------------------
 *
 * Adding a built-in is one row here: the name the UI shows and the function
 * that creates its GEGL node. Nothing else switches on the preset kind. */
typedef GeglNode *(*EnhancerMakeOpFn)(GeglNode *p_graph);

static GeglNode *
_make_auto_fix(GeglNode *p_graph) {
   return (
      gegl_node_new_child(p_graph, "operation", "gegl:stretch-contrast", NULL));
}

static GeglNode *
_make_brightness(GeglNode *p_graph) {
   return (gegl_node_new_child(p_graph, "operation", "gegl:exposure",
                               "exposure", 0.5, NULL));
}

static GeglNode *
_make_contrast(GeglNode *p_graph) {
   return (gegl_node_new_child(p_graph, "operation", "gegl:brightness-contrast",
                               "contrast", 1.3, NULL));
}

static GeglNode *
_make_saturation(GeglNode *p_graph) {
   return (gegl_node_new_child(p_graph, "operation", "gegl:saturation", "scale",
                               1.4, NULL));
}

static GeglNode *
_make_warm(GeglNode *p_graph) {
   return (
      gegl_node_new_child(p_graph, "operation", "gegl:color-enhance", NULL));
}

static GeglNode *
_make_cool(GeglNode *p_graph) {
   return (gegl_node_new_child(p_graph, "operation", "gegl:exposure",
                               "exposure", -0.3, NULL));
}

static GeglNode *
_make_sharpen(GeglNode *p_graph) {
   return (
      gegl_node_new_child(p_graph, "operation", "gegl:unsharp-mask", NULL));
}

static GeglNode *
_make_denoise(GeglNode *p_graph) {
   return (
      gegl_node_new_child(p_graph, "operation", "gegl:noise-reduction", NULL));
}

static const struct {
   const char      *c_name;
   EnhancerMakeOpFn fn_make;
} BUILTINS[] = {
   {"Auto-fix", _make_auto_fix}, {"Brightness", _make_brightness},
   {"Contrast", _make_contrast}, {"Saturation", _make_saturation},
   {"Warm", _make_warm},         {"Cool", _make_cool},
   {"Sharpen", _make_sharpen},   {"Denoise", _make_denoise},
};

/* --- preset list --------------------------------------------------------- */

static void
_preset_free(gpointer p) {
   EnhancerPreset *p_pr = (EnhancerPreset *)p;
   if (p_pr != NULL) {
      g_free(p_pr->c_name);
      g_free(p_pr->c_graph);
      g_free(p_pr);
   }
}

static EnhancerPreset *
_preset_new(const char *c_name, const char *c_graph, int i_builtin) {
   EnhancerPreset *p_pr = g_new0(EnhancerPreset, 1);
   p_pr->c_name         = g_strdup(c_name);
   p_pr->c_graph        = g_strdup(c_graph);
   p_pr->i_builtin      = i_builtin;
   return (p_pr);
}

/* Deep-copy p_src into a new GPtrArray of EnhancerPreset* (NULL/empty ->
 * empty array). Shared by enhancer_set_presets and the async paths (which
 * snapshot the preset list before handing it to a worker thread, so a
 * concurrent enhancer_set_presets() cannot race it). */
static GPtrArray *
_presets_copy(const GPtrArray *p_src) {
   GPtrArray *p_out = g_ptr_array_new_with_free_func(_preset_free);
   for (guint u = 0; p_src != NULL && u < p_src->len; u++) {
      const EnhancerPreset *p_s = g_ptr_array_index((GPtrArray *)p_src, u);
      g_ptr_array_add(p_out,
                      _preset_new(p_s->c_name, p_s->c_graph, p_s->i_builtin));
   }
   return (p_out);
}

static void
_add_builtins(GPtrArray *p_list) {
   for (gsize u = 0; u < G_N_ELEMENTS(BUILTINS); u++) {
      g_ptr_array_add(p_list, _preset_new(BUILTINS[u].c_name, NULL, 1));
   }
}

Enhancer *
enhancer_new(void) {
   Enhancer *p_e  = g_new0(Enhancer, 1);
   p_e->p_presets = g_ptr_array_new_with_free_func(_preset_free);
   _add_builtins(p_e->p_presets);
   return (p_e);
}

void
enhancer_delete(Enhancer *p_e) {
   if (p_e == NULL) {
      return;
   }
   g_ptr_array_unref(p_e->p_presets);
   g_free(p_e);
}

void
enhancer_set_presets(Enhancer *p_e, const GPtrArray *p_presets) {
   g_return_if_fail(p_e != NULL);
   GPtrArray *p_copy = _presets_copy(p_presets);
   g_ptr_array_unref(p_e->p_presets);
   p_e->p_presets = p_copy;
}

void
enhancer_set_user_presets(Enhancer *p_e, const GPtrArray *p_pairs) {
   g_return_if_fail(p_e != NULL);
   GPtrArray *p_list = g_ptr_array_new_with_free_func(_preset_free);
   _add_builtins(p_list);
   for (guint u = 0; p_pairs != NULL && u < p_pairs->len; u++) {
      const SettingsPair *p_pr = g_ptr_array_index((GPtrArray *)p_pairs, u);
      g_ptr_array_add(p_list, _preset_new(p_pr->c_name, p_pr->c_value, 0));
   }
   g_ptr_array_unref(p_e->p_presets);
   p_e->p_presets = p_list;
}

const GPtrArray *
enhancer_get_presets(Enhancer *p_e) {
   return (p_e != NULL ? p_e->p_presets : NULL);
}

char *
enhancer_describe_mask(const GPtrArray *p_presets, guint8 u_mask) {
   if (p_presets == NULL || u_mask == 0) {
      return (NULL);
   }
   GString *p_str = g_string_new(NULL);
   for (guint u = 0; u < p_presets->len && u < GGAZE_ENHANCE_MAX_PRESETS; u++) {
      if ((u_mask & (guint8)(1u << u)) == 0) {
         continue;
      }
      const EnhancerPreset *p_pr = g_ptr_array_index((GPtrArray *)p_presets, u);
      if (p_str->len > 0) {
         g_string_append_c(p_str, ',');
      }
      g_string_append(p_str, p_pr->c_name);
   }
   if (p_str->len == 0) {
      g_string_free(p_str, TRUE);
      return (NULL);
   }
   return (g_string_free(p_str, FALSE));
}

/* --- export destination --------------------------------------------------- */

gboolean
enhancer_ext_supported(const char *c_ext) {
   return (c_ext != NULL && (g_ascii_strcasecmp(c_ext, ".jpg") == 0 ||
                             g_ascii_strcasecmp(c_ext, ".jpeg") == 0 ||
                             g_ascii_strcasecmp(c_ext, ".png") == 0 ||
                             g_ascii_strcasecmp(c_ext, ".webp") == 0));
}

GFile *
enhancer_export_dest_for(GFile *p_src) {
   g_return_val_if_fail(G_IS_FILE(p_src), NULL);
   char       *c_base = g_file_get_basename(p_src);
   char       *c_stem = NULL;
   const char *c_ext  = NULL;
   pathutil_split_ext(c_base, &c_stem, &c_ext);
   if (!enhancer_ext_supported(c_ext)) {
      /* An unsupported source extension is part of the stem ("a.tiff" ->
       * "a.tiff-enhanced.jpg"), so nothing about the original name is lost. */
      g_free(c_stem);
      c_stem = g_strdup(c_base);
      c_ext  = ".jpg";
   }
   char  *c_prefix = g_strconcat(c_stem, "-enhanced", NULL);
   GFile *p_dir    = g_file_get_parent(p_src);
   GFile *p_out    = pathutil_unique_child(p_dir, c_prefix, c_ext, 1);
   g_clear_object(&p_dir);
   g_free(c_prefix);
   g_free(c_stem);
   g_free(c_base);
   return (p_out);
}

/* --- graph building ------------------------------------------------------- */

/* Set one "prop=value" pair on p_node, converting the text to the property's
 * type. Unknown properties are reported; a bad value falls back to the
 * property's default. */
static gboolean
_set_prop(GeglNode *p_node, const char *c_pair, GError **p_err) {
   const char *c_eq = strchr(c_pair, '=');
   if (c_eq == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "enhancer: expected prop=value, got '%s'", c_pair);
      return (FALSE);
   }
   char       *c_prop = g_strndup(c_pair, (gsize)(c_eq - c_pair));
   GParamSpec *p_spec = gegl_node_find_property(p_node, c_prop);
   gboolean    b_ok   = (p_spec != NULL);
   if (b_ok) {
      GValue t_val = G_VALUE_INIT;
      g_value_init(&t_val, p_spec->value_type);
      if (G_IS_PARAM_SPEC_DOUBLE(p_spec)) {
         g_value_set_double(&t_val, g_ascii_strtod(c_eq + 1, NULL));
      } else if (G_IS_PARAM_SPEC_INT(p_spec)) {
         g_value_set_int(&t_val, (gint)g_ascii_strtoll(c_eq + 1, NULL, 10));
      } else if (G_IS_PARAM_SPEC_BOOLEAN(p_spec)) {
         g_value_set_boolean(&t_val, g_ascii_strcasecmp(c_eq + 1, "true") == 0);
      } else if (G_IS_PARAM_SPEC_STRING(p_spec)) {
         g_value_set_string(&t_val, c_eq + 1);
      } else {
         b_ok = FALSE;
      }
      if (b_ok) {
         gegl_node_set_property(p_node, c_prop, &t_val);
      }
      g_value_unset(&t_val);
   }
   if (!b_ok) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "enhancer: unknown or unsupported property '%s'", c_prop);
   }
   g_free(c_prop);
   return (b_ok);
}

/* Parse a user graph string ("gegl:op prop=v gegl:op2 ...") into nodes
 * chained after p_prev. Returns the last node, or NULL with p_err. */
static GeglNode *
_make_user_chain(GeglNode *p_graph, GeglNode *p_prev, const char *c_graph,
                 GError **p_err) {
   char    **pp_tok = g_strsplit_set(c_graph, " \t\n", -1);
   GeglNode *p_last = p_prev;
   GeglNode *p_node = NULL;
   gboolean  b_ok   = TRUE;
   for (guint u = 0; b_ok && pp_tok[u] != NULL; u++) {
      const char *c_tok = pp_tok[u];
      if (*c_tok == '\0') {
         continue;
      }
      if (strchr(c_tok, '=') != NULL && p_node != NULL) {
         b_ok = _set_prop(p_node, c_tok, p_err);
         continue;
      }
      if (!gegl_has_operation(c_tok)) {
         g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "enhancer: no GEGL operation '%s'", c_tok);
         b_ok = FALSE;
         break;
      }
      p_node = gegl_node_new_child(p_graph, "operation", c_tok, NULL);
      gegl_node_link(p_last, p_node);
      p_last = p_node;
   }
   g_strfreev(pp_tok);
   if (b_ok && p_last == p_prev) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "enhancer: empty graph");
      b_ok = FALSE;
   }
   return (b_ok ? p_last : NULL);
}

/* Append p_preset's node(s) after p_prev in p_graph; returns the new tail
 * or NULL with p_err. Built-ins come from BUILTINS[], user presets from
 * their graph string. */
static GeglNode *
_append_preset(GeglNode *p_graph, GeglNode *p_prev,
               const EnhancerPreset *p_preset, GError **p_err) {
   if (p_preset->c_graph != NULL) {
      return (_make_user_chain(p_graph, p_prev, p_preset->c_graph, p_err));
   }
   for (gsize u = 0; u < G_N_ELEMENTS(BUILTINS); u++) {
      if (g_str_equal(BUILTINS[u].c_name, p_preset->c_name)) {
         GeglNode *p_op = BUILTINS[u].fn_make(p_graph);
         gegl_node_link(p_prev, p_op);
         return (p_op);
      }
   }
   g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
               "enhancer: unknown preset '%s'", p_preset->c_name);
   return (NULL);
}

/* --- geometric transform nodes (decision #35) ---------------------------
 *
 * Measured with a 3x2 probe on gegl 0.4.72: gegl:rotate's positive degrees
 * turn the image COUNTER-clockwise on screen (y down), and its output extent
 * is offset -- with origin 0,0 a +90 turn lands at x=0,y=-3, and a small
 * angle about the centre gets a bounding box padded by a pixel or two for
 * the sampler. So the Transform's clockwise angles are negated here, and
 * every op that needs coordinates (auto-crop, the user's crop) reads the
 * live bounding box of its input node and offsets by its origin, cropping
 * to the analytic transform_base_size so the output is exactly the size the
 * crop tool laid its rectangle out on. */

/* A gegl:crop of the analytic size d_w x d_h centred on the point (d_cx,
 * d_cy) the straighten turned about -- NOT on the rotated node's bounding
 * box, which GEGL pads asymmetrically for the sampler (measured: a 400x300
 * at 1 degree gets a -3,-4 origin and a 406x308 extent), so centring on it
 * shifted the crop by up to a pixel and let antialiased edge pixels in.
 * Whole-pixel offsets: an integer crop cannot be truer than that, and the
 * half-pixel this rounding can cost is inside TRANSFORM_AUTOCROP_INSET. */
static GeglNode *
_append_centred_crop(GeglNode *p_graph, GeglNode *p_prev, gdouble d_cx,
                     gdouble d_cy, gdouble d_w, gdouble d_h) {
   gdouble   d_x = round(d_cx - d_w / 2.0);
   gdouble   d_y = round(d_cy - d_h / 2.0);
   GeglNode *p_crop =
      gegl_node_new_child(p_graph, "operation", "gegl:crop", "x", d_x, "y", d_y,
                          "width", d_w, "height", d_h, NULL);
   gegl_node_link(p_prev, p_crop);
   return (p_crop);
}

/* `[` / `]`: i_quarter clockwise quarter turns. Origin (0, 0) with the
 * nearest sampler makes it a pure pixel permutation -- no resampling blur,
 * which a centre origin would introduce for an odd width - height. */
static GeglNode *
_append_quarter_turn(GeglNode *p_graph, GeglNode *p_prev, gint i_quarter) {
   GeglNode *p_rot = gegl_node_new_child(
      p_graph, "operation", "gegl:rotate", "degrees", -90.0 * i_quarter,
      "origin-x", 0.0, "origin-y", 0.0, "sampler", GEGL_SAMPLER_NEAREST, NULL);
   gegl_node_link(p_prev, p_rot);
   return (p_rot);
}

/* `R`: the straighten angle about the image centre, then a crop to the size
 * transform_straighten_size promises -- the inset inscribed rectangle
 * (auto-crop) or the rotated bounding box -- about that same centre, so the
 * output is exactly transform_base_size and every auto-cropped pixel is
 * fully opaque (tests/test_enhancer.c straighten_autocrop_is_opaque). */
static GeglNode *
_append_straighten(GeglNode *p_graph, GeglNode *p_prev, const Transform *p_xf) {
   GeglRectangle t_bbox = gegl_node_get_bounding_box(p_prev);
   gdouble       d_cx   = t_bbox.x + t_bbox.width / 2.0;
   gdouble       d_cy   = t_bbox.y + t_bbox.height / 2.0;
   GeglNode     *p_rot  = gegl_node_new_child(
      p_graph, "operation", "gegl:rotate", "degrees", -p_xf->d_degrees,
      "origin-x", d_cx, "origin-y", d_cy, NULL);
   gegl_node_link(p_prev, p_rot);
   gdouble d_w, d_h;
   transform_straighten_size(t_bbox.width, t_bbox.height, p_xf->d_degrees,
                             p_xf->b_autocrop, &d_w, &d_h);
   return (_append_centred_crop(p_graph, p_rot, d_cx, d_cy, d_w, d_h));
}

/* `c`: the user's crop, clamped into the base image it was drawn on
 * (transform_effective_crop) and offset by that image's origin. An empty
 * result -- a crop the straighten has pushed entirely outside its base --
 * crops nothing rather than emitting an empty image: the controller keeps
 * such a crop in the state (the title says "crop (outside view)") so a
 * nudge back applies it again, and this skip is what makes that safe. */
static GeglNode *
_append_user_crop(GeglNode *p_graph, GeglNode *p_prev, const Transform *p_xf) {
   GeglRectangle t_bbox = gegl_node_get_bounding_box(p_prev);
   CropRect      t_crop;
   if (!transform_effective_crop(p_xf, t_bbox.width, t_bbox.height, &t_crop)) {
      return (p_prev);
   }
   GeglNode *p_crop = gegl_node_new_child(
      p_graph, "operation", "gegl:crop", "x", t_bbox.x + t_crop.d_x, "y",
      t_bbox.y + t_crop.d_y, "width", t_crop.d_w, "height", t_crop.d_h, NULL);
   gegl_node_link(p_prev, p_crop);
   return (p_crop);
}

/* Append p_xf's ops after p_prev in decision #35's order; returns the new
 * tail (p_prev itself for NULL / the identity). */
static GeglNode *
_append_transform(GeglNode *p_graph, GeglNode *p_prev, const Transform *p_xf) {
   if (p_xf == NULL || transform_is_identity(p_xf)) {
      return (p_prev);
   }
   if (p_xf->i_quarter != 0) {
      p_prev = _append_quarter_turn(p_graph, p_prev, p_xf->i_quarter);
   }
   if (p_xf->d_degrees != 0.0) {
      p_prev = _append_straighten(p_graph, p_prev, p_xf);
   }
   if (p_xf->b_crop) {
      p_prev = _append_user_crop(p_graph, p_prev, p_xf);
   }
   return (p_prev);
}

/* Append the presets enabled in u_mask after p_prev; returns the new tail,
 * or NULL with p_err. *p_any is set when at least one was appended. */
static GeglNode *
_append_presets(GeglNode *p_graph, GeglNode *p_prev, const GPtrArray *p_presets,
                guint8 u_mask, gboolean *p_any, GError **p_err) {
   for (guint u = 0; u < p_presets->len && u < GGAZE_ENHANCE_MAX_PRESETS; u++) {
      if ((u_mask & (guint8)(1u << u)) == 0) {
         continue;
      }
      p_prev = _append_preset(
         p_graph, p_prev, g_ptr_array_index((GPtrArray *)p_presets, u), p_err);
      if (p_prev == NULL) {
         return (NULL);
      }
      *p_any = TRUE;
   }
   return (p_prev);
}

/* Run p_in through the presets enabled in u_mask (all of them when p_mask is
 * ~0) and then the transform p_xf (nullable), and return the sink buffer.
 * gegl:buffer-sink allocates the output itself: handing it a pre-created
 * buffer leaked one GeglBuffer per apply (the sink replaced the pointer). */
static GeglBuffer *
_run_chain(GeglBuffer *p_in, const GPtrArray *p_presets, guint8 u_mask,
           const Transform *p_xf, GError **p_err) {
   GeglNode *p_graph = gegl_node_new();
   GeglNode *p_prev  = gegl_node_new_child(
      p_graph, "operation", "gegl:buffer-source", "buffer", p_in, NULL);
   gboolean b_any = FALSE;
   p_prev = _append_presets(p_graph, p_prev, p_presets, u_mask, &b_any, p_err);
   if (p_prev == NULL) {
      g_object_unref(p_graph);
      return (NULL);
   }
   if (p_xf != NULL && !transform_is_identity(p_xf)) {
      p_prev = _append_transform(p_graph, p_prev, p_xf);
      b_any  = TRUE;
   }
   if (!b_any) {
      g_object_unref(p_graph);
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: no preset enabled and no transform");
      return (NULL);
   }
   GeglBuffer *p_out  = NULL;
   GeglNode   *p_sink = gegl_node_new_child(
      p_graph, "operation", "gegl:buffer-sink", "buffer", &p_out, NULL);
   gegl_node_link(p_prev, p_sink);
   gegl_node_process(p_sink);
   g_object_unref(p_graph);
   if (p_out == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: processing produced no buffer");
   }
   return (p_out);
}

GeglBuffer *
enhancer_apply_chain(GeglBuffer *p_in, const GPtrArray *p_presets,
                     guint8 u_mask, const Transform *p_xf, GError **p_err) {
   g_return_val_if_fail(p_in != NULL, NULL);
   g_return_val_if_fail(p_presets != NULL, NULL);
   return (_run_chain(p_in, p_presets, u_mask, p_xf, p_err));
}

GeglBuffer *
enhancer_apply(GeglBuffer *p_in, const EnhancerPreset *p_preset,
               GError **p_err) {
   g_return_val_if_fail(p_in != NULL, NULL);
   g_return_val_if_fail(p_preset != NULL, NULL);
   /* A chain of one: the preset is index 0 of a one-entry list. */
   GPtrArray *p_one = g_ptr_array_new();
   g_ptr_array_add(p_one, (gpointer)p_preset);
   GeglBuffer *p_out = _run_chain(p_in, p_one, 1, NULL, p_err);
   g_ptr_array_unref(p_one);
   return (p_out);
}

/* Pick the GEGL saver op and (for jpeg) quality from the output extension.
 * Returns the op name, or NULL if the extension is unsupported / the op is
 * not installed. ju0: never write JPEG bytes into a .png. */
static const char *
_saver_for_ext(GFile *p_out) {
   char       *c_base = g_file_get_basename(p_out);
   const char *c_dot  = strrchr(c_base, '.');
   const char *c_op   = NULL;
   if (enhancer_ext_supported(c_dot)) {
      if (g_ascii_strcasecmp(c_dot, ".png") == 0) {
         c_op = "gegl:png-save";
      } else if (g_ascii_strcasecmp(c_dot, ".webp") == 0) {
         c_op = "gegl:webp-save";
      } else {
         c_op = "gegl:jpg-save";
      }
   }
   g_free(c_base);
   /* webp-save ships as a plugin; only promise it if installed. */
   if (c_op != NULL && !gegl_has_operation(c_op)) {
      return (NULL);
   }
   return (c_op);
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
      return (FALSE);
   }
   const char *c_op = _saver_for_ext(p_out);
   if (c_op == NULL) {
      g_free(c_path);
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                  "enhancer: unsupported export extension");
      return (FALSE);
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
      return (FALSE);
   }
   return (TRUE);
}

gboolean
enhancer_export(GeglBuffer *p_in, const EnhancerPreset *p_preset, GFile *p_out,
                GError **p_err) {
   g_return_val_if_fail(p_in != NULL, FALSE);
   g_return_val_if_fail(p_out != NULL, FALSE);
   GeglBuffer *p_buf = enhancer_apply(p_in, p_preset, p_err);
   if (p_buf == NULL) {
      return (FALSE);
   }
   gboolean b_ok = _save_buffer(p_buf, p_out, p_err);
   g_object_unref(p_buf);
   return (b_ok);
}

/* Export p_in with the enabled-preset chain (u_mask) and the transform
 * composed, to p_out. */
gboolean
enhancer_export_chain(GeglBuffer *p_in, const GPtrArray *p_presets,
                      guint8 u_mask, const Transform *p_xf, GFile *p_out,
                      GError **p_err) {
   g_return_val_if_fail(p_in != NULL, FALSE);
   g_return_val_if_fail(p_out != NULL, FALSE);
   g_return_val_if_fail(p_presets != NULL, FALSE);
   GeglBuffer *p_buf =
      enhancer_apply_chain(p_in, p_presets, u_mask, p_xf, p_err);
   if (p_buf == NULL) {
      return (FALSE);
   }
   gboolean b_ok = _save_buffer(p_buf, p_out, p_err);
   g_object_unref(p_buf);
   return (b_ok);
}

/* --- async export (the `s` key / Save button) ---------------------------- */

typedef struct {
   GFile     *p_src;     /* owned */
   GFile     *p_out;     /* owned */
   GPtrArray *p_presets; /* owned deep copy */
   guint8     u_mask;
   Transform  t_xf; /* by value: a snapshot the tools cannot nudge under
                     * the worker */
} _ExportReq;

static void
_export_req_free(_ExportReq *p_req) {
   g_clear_object(&p_req->p_src);
   g_clear_object(&p_req->p_out);
   g_clear_pointer(&p_req->p_presets, g_ptr_array_unref);
   g_free(p_req);
}

static void
_export_thread(GTask *p_task, gpointer p_src, gpointer p_task_data,
               GCancellable *p_cancel) {
   (void)p_src;
   (void)p_cancel;
   _ExportReq *p_req = (_ExportReq *)p_task_data;
   if (g_task_return_error_if_cancelled(p_task)) {
      return;
   }
   GError     *p_err = NULL;
   GeglBuffer *p_buf = enhancer_load(p_req->p_src, &p_err);
   gboolean    b_ok  = FALSE;
   if (p_buf != NULL) {
      b_ok = enhancer_export_chain(p_buf, p_req->p_presets, p_req->u_mask,
                                   &p_req->t_xf, p_req->p_out, &p_err);
      g_object_unref(p_buf);
   }
   if (!b_ok) {
      if (p_err == NULL) {
         g_set_error(&p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "enhancer: export failed (no detail)");
      }
      g_task_return_error(p_task, p_err);
   } else {
      g_task_return_boolean(p_task, TRUE);
   }
}

/* Copy p_xf into *p_dst, the identity when p_xf is NULL. */
static void
_snapshot_transform(Transform *p_dst, const Transform *p_xf) {
   if (p_xf != NULL) {
      *p_dst = *p_xf;
   } else {
      transform_init(p_dst);
   }
}

void
enhancer_export_chain_async(GFile *p_src, const GPtrArray *p_presets,
                            guint8 u_mask, const Transform *p_xf, GFile *p_out,
                            GCancellable *p_cancel, GAsyncReadyCallback p_cb,
                            gpointer p_data) {
   g_return_if_fail(G_IS_FILE(p_src));
   g_return_if_fail(G_IS_FILE(p_out));
   _ExportReq *p_req = g_new0(_ExportReq, 1);
   p_req->p_src      = (GFile *)g_object_ref(p_src);
   p_req->p_out      = (GFile *)g_object_ref(p_out);
   p_req->p_presets  = _presets_copy(p_presets);
   p_req->u_mask     = u_mask;
   _snapshot_transform(&p_req->t_xf, p_xf);
   GTask *p_task = g_task_new(p_src, p_cancel, p_cb, p_data);
   g_task_set_task_data(p_task, p_req, (GDestroyNotify)_export_req_free);
   g_task_run_in_thread(p_task, _export_thread);
   g_object_unref(p_task);
}

gboolean
enhancer_export_chain_finish(GAsyncResult *p_res, GError **p_err) {
   g_return_val_if_fail(G_IS_TASK(p_res), FALSE);
   return (g_task_propagate_boolean(G_TASK(p_res), p_err));
}

#if GGAZE_HAVE_GEGL

/* Plain gegl:load into a GeglBuffer (no EXIF orientation). Used as the
 * fallback in enhancer_load when the loader's texture is not the RGBA8 layout
 * the fast path handles (no backend produces such a texture today). */
static GeglBuffer *
_enhancer_load_gegl(GFile *p_file, GError **p_err) {
   char *c_path = g_file_get_path(p_file);
   if (c_path == NULL) {
      g_set_error(p_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "enhancer: non-local load path");
      return (NULL);
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
      return (NULL);
   }
   g_free(c_path);
   return (p_buf);
}

GeglBuffer *
enhancer_load(GFile *p_file, GError **p_err) {
   g_return_val_if_fail(p_file != NULL, NULL);
   /* Load via ggaze's own loader so the EXIF Orientation every backend honors
    * (decision #26) is applied -- gegl:load does NOT auto-rotate, so the
    * enhance live preview and the A-menu per-preset preview thumbnails would
    * otherwise render un-rotated for images whose stored orientation is not
    * 1 (portrait phone JPEGs, rot6.jpg, ...). The loader returns an upright
    * GdkTexture (RGBA8) for any supported format; copy its pixels into a
    * GeglBuffer for the GEGL preset chain. */
   GError     *p_load_err = NULL;
   GdkTexture *p_tex      = loader_load(p_file, NULL, &p_load_err);
   if (p_tex == NULL) {
      g_propagate_error(p_err, p_load_err);
      return (NULL);
   }
   int             i_w   = gdk_texture_get_width(p_tex);
   int             i_h   = gdk_texture_get_height(p_tex);
   GdkMemoryFormat e_fmt = gdk_texture_get_format(p_tex);
   GeglBuffer     *p_buf = NULL;
   if (i_w > 0 && i_h > 0 && e_fmt == GDK_MEMORY_R8G8B8A8) {
      const Babl   *p_fmt    = babl_format("R'G'B'A u8");
      gint          i_stride = i_w * 4;
      gpointer      p_data   = g_malloc((gsize)i_stride * (gsize)i_h);
      GeglRectangle rect     = {0, 0, i_w, i_h};
      gdk_texture_download(p_tex, p_data, (gsize)i_stride);
      p_buf = gegl_buffer_new(&rect, p_fmt);
      gegl_buffer_set(p_buf, &rect, 0, p_fmt, p_data, i_stride);
      g_free(p_data);
   }
   g_object_unref(p_tex);
   if (p_buf != NULL) {
      return (p_buf);
   }
   /* Empty or non-RGBA8 texture (no backend produces the latter today):
    * fall back to gegl:load -- un-rotated, but channel-correct. Warn once so a
    * future backend that emits a different layout does not silently
    * reintroduce the orientation bug this function exists to fix. */
   static gboolean b_warned = FALSE;
   if (!b_warned) {
      b_warned = TRUE;
      g_warning("enhancer: texture is not RGBA8 (format %d); falling back "
                "to gegl:load without EXIF orientation",
                (gint)e_fmt);
   }
   return (_enhancer_load_gegl(p_file, p_err));
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
      return (NULL);
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
   return (p_tex);
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
   GFile     *p_file;    /* owned */
   GPtrArray *p_presets; /* owned deep copy (thread-safe snapshot) */
   guint8     u_mask;
   Transform  t_xf; /* by value: a snapshot (see _ExportReq) */
} _AsyncApplyReq;

/* The worker's result: the texture plus the original's upright size, which
 * the controller records for the crop tool (see the finish doc). */
typedef struct {
   GdkTexture *p_tex; /* owned */
   gint        i_orig_w;
   gint        i_orig_h;
} _ApplyResult;

static void
_apply_result_free(_ApplyResult *p_res) {
   g_clear_object(&p_res->p_tex);
   g_free(p_res);
}

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
   GError       *p_err = NULL;
   GeglBuffer   *p_buf = enhancer_load(p_req->p_file, &p_err);
   _ApplyResult *p_res = g_new0(_ApplyResult, 1);
   if (p_buf != NULL) {
      p_res->i_orig_w   = gegl_buffer_get_width(p_buf);
      p_res->i_orig_h   = gegl_buffer_get_height(p_buf);
      GeglBuffer *p_enh = enhancer_apply_chain(
         p_buf, p_req->p_presets, p_req->u_mask, &p_req->t_xf, &p_err);
      if (p_enh != NULL) {
         p_res->p_tex = enhancer_buffer_to_texture(p_enh, &p_err);
         g_object_unref(p_enh);
      }
      g_object_unref(p_buf);
   }
   (void)p_cancel;
   if (p_res->p_tex == NULL) {
      _apply_result_free(p_res);
      g_task_return_error(p_task, p_err);
   } else {
      g_task_return_pointer(p_task, p_res, (GDestroyNotify)_apply_result_free);
   }
}

void
enhancer_apply_chain_async(GFile *p_file, const GPtrArray *p_presets,
                           guint8 u_mask, const Transform *p_xf,
                           GCancellable *p_cancel, GAsyncReadyCallback p_cb,
                           gpointer p_data) {
   g_return_if_fail(p_file != NULL);
   _AsyncApplyReq *p_req = g_new0(_AsyncApplyReq, 1);
   p_req->p_file         = (GFile *)g_object_ref(p_file);
   p_req->p_presets      = _presets_copy(p_presets);
   p_req->u_mask         = u_mask;
   _snapshot_transform(&p_req->t_xf, p_xf);
   GTask *p_task = g_task_new(p_file, p_cancel, p_cb, p_data);
   g_task_set_task_data(p_task, p_req, (GDestroyNotify)_async_apply_req_free);
   g_task_run_in_thread(p_task, _apply_chain_thread);
   g_object_unref(p_task);
}

GdkTexture *
enhancer_apply_chain_finish(GAsyncResult *p_res, gint *p_orig_w, gint *p_orig_h,
                            GError **p_err) {
   g_return_val_if_fail(G_IS_TASK(p_res), NULL);
   _ApplyResult *p_out =
      (_ApplyResult *)g_task_propagate_pointer((GTask *)p_res, p_err);
   if (p_out == NULL) {
      return (NULL);
   }
   if (p_orig_w != NULL) {
      *p_orig_w = p_out->i_orig_w;
   }
   if (p_orig_h != NULL) {
      *p_orig_h = p_out->i_orig_h;
   }
   GdkTexture *p_tex = g_steal_pointer(&p_out->p_tex);
   _apply_result_free(p_out);
   return (p_tex);
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

/* Complete p_task as cancelled, releasing p_small/p_out. Returns TRUE iff it
 * did (the caller then returns). */
static gboolean
_preview_bail_if_cancelled(GTask *p_task, GCancellable *p_cancel,
                           GeglBuffer *p_small, GPtrArray *p_out) {
   if (!g_cancellable_is_cancelled(p_cancel)) {
      return (FALSE);
   }
   g_clear_object(&p_small);
   g_clear_pointer(&p_out, g_ptr_array_unref);
   g_task_return_new_error(p_task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                           "enhancer preview cancelled");
   return (TRUE);
}

/* One preset's preview texture from the downscaled original, or NULL when
 * that preset cannot be applied (the card then stays empty). */
static GdkTexture *
_preview_one(GeglBuffer *p_small, const EnhancerPreset *p_preset) {
   GError     *p_err    = NULL;
   GeglBuffer *p_effect = enhancer_apply(p_small, p_preset, &p_err);
   GdkTexture *p_tex =
      p_effect != NULL ? enhancer_buffer_to_texture(p_effect, &p_err) : NULL;
   g_clear_object(&p_effect);
   g_clear_error(&p_err);
   return (p_tex);
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
   if (p_full != NULL &&
       _preview_bail_if_cancelled(p_task, p_cancel, p_full, NULL)) {
      return;
   }
   GeglBuffer *p_small =
      p_full != NULL ? _preview_downscale(p_full, &p_err) : NULL;
   g_clear_object(&p_full);
   if (p_small == NULL) {
      g_task_return_error(p_task, p_err);
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
   for (guint u = 0; u < p_req->p_presets->len; u++) {
      if (_preview_bail_if_cancelled(p_task, p_cancel, p_small, p_out)) {
         return;
      }
      g_ptr_array_add(
         p_out, _preview_one(p_small, g_ptr_array_index(p_req->p_presets, u)));
   }
   g_object_unref(p_small);
   g_task_return_pointer(p_task, p_out, (GDestroyNotify)g_ptr_array_unref);
}

void
enhancer_preview_thumbnails_async(GFile *p_file, const GPtrArray *p_presets,
                                  GCancellable       *p_cancel,
                                  GAsyncReadyCallback p_cb, gpointer p_data) {
   g_return_if_fail(G_IS_FILE(p_file));
   _PreviewReq *p_req = g_new0(_PreviewReq, 1);
   p_req->p_file      = (GFile *)g_object_ref(p_file);
   p_req->p_presets   = _presets_copy(p_presets);
   if (p_req->p_presets->len > GGAZE_ENHANCE_MAX_PRESETS) {
      g_ptr_array_set_size(p_req->p_presets, GGAZE_ENHANCE_MAX_PRESETS);
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
