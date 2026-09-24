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

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <lcms2.h>

#include "enhancer-gegl.h"
#include "ggaze-config.h"
#include "icc.h"                /* is there a profile to manage at all? */
#include "info.h"               /* EXIF Orientation for the GEGL decode */
#include "loader/detect.h"      /* format sniff + the shared size caps */
#include "loader/intact.h"      /* GEGL's loaders spin on a truncated file */
#include "loader/loader.h"      /* orientation-aware load -> upright texture */
#include "loader/pixbuf-util.h" /* the orientation permutation */
#include "pathutil.h"
#include "transform.h"

/* Test seam (enhancer-gegl.h): an op name to treat as not installed. */
static char *c_missing_op = NULL;

void
enhancer_test_set_missing_op(const char *c_op) {
   g_free(c_missing_op);
   c_missing_op = g_strdup(c_op);
}

/* Test seams (enhancer-gegl.h): a hook run as each managed load starts,
 * and the number of loader-path decodes so far. */
static EnhancerTestHook p_load_hook      = NULL;
static gpointer         p_load_hook_data = NULL;
static gint             i_loader_decodes = 0;

void
enhancer_test_set_load_hook(EnhancerTestHook p_hook, gpointer p_data) {
   p_load_hook      = p_hook;
   p_load_hook_data = p_data;
}

guint
enhancer_test_loader_decodes(void) {
   return ((guint)g_atomic_int_get(&i_loader_decodes));
}

/* gegl_has_operation(), unless the test seam hides c_op. */
static gboolean
_has_op(const char *c_op) {
   return (g_strcmp0(c_op, c_missing_op) != 0 && gegl_has_operation(c_op));
}

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
      if (!_has_op(c_tok)) {
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

/* --- export (decision #45: the profile travels with the pixels) ---------
 *
 * The buffer's babl format carries the image's colour space (see the
 * ICC-aware load below). gegl:png-save and gegl:jpg-save embed that space's
 * profile, i.e. the bytes babl keeps for the space: the source's own when
 * babl made the space from them, but babl answers a profile equivalent to
 * one it has seen (the same curves, primaries within 0.001:
 * babl_space_match_trc_matrix) with the earlier space and ITS bytes, and
 * re-copies the latest profile onto a grey space it already has. So the
 * export carries an equivalent profile -- same colours, possibly another
 * file's description -- and the source's bytes only when it was the first
 * of its kind this process met. Embedding the source's own bytes would mean
 * writing the PNG / JPEG profile chunk ourselves after GEGL's saver; not
 * worth it for identical colours. gegl:webp-save embeds nothing -- and needs no
 * conversion node for it: it reads the buffer as "R'G'B'A u8" with no space,
 * i.e. sRGB, so babl converts the pixels on the way out (a test with
 * gegl:convert-space hidden proves it), and an untagged WebP is exactly what
 * every viewer reads as sRGB. An sRGB buffer (every file without a managed
 * profile) exports exactly as before. */

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
   if (c_op != NULL && !_has_op(c_op)) {
      return (NULL);
   }
   return (c_op);
}

/* Run the save graph: buffer-source -> the saver op at c_path. */
static void
_run_save_graph(GeglBuffer *p_buf, const char *c_op, const char *c_path) {
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
}

/* ku0: a real write is a non-empty file that is new, or changed since the
 * stat taken before the save (a pre-existing path proves nothing). */
static gboolean
_save_produced_file(const char *c_path, gboolean b_existed,
                    const GStatBuf *p_before) {
   GStatBuf st_after;
   if (g_stat(c_path, &st_after) != 0 || st_after.st_size <= 0) {
      return (FALSE);
   }
   return (!b_existed || st_after.st_mtime != p_before->st_mtime ||
           st_after.st_size != p_before->st_size);
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
   GStatBuf st_before;
   gboolean b_existed = (g_stat(c_path, &st_before) == 0);
   _run_save_graph(p_buf, c_op, c_path);
   gboolean b_ok = _save_produced_file(c_path, b_existed, &st_before);
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

/* --- load ------------------------------------------------------------------
 *
 * Two paths, one result: an upright RGBA8 GeglBuffer whose babl format
 * carries the image's colour space.
 *
 * The MANAGED path (decision #45) is for a local PNG or JPEG that embeds
 * an ICC profile babl parses to a space other than sRGB. It decodes
 * through GEGL's own gegl:png-load / gegl:jpg-load, which read that profile
 * and TAG the buffer's format with the space it describes -- they never
 * convert a pixel. That tag is the whole of the colour management: the
 * preset chain runs in the image's own space (GEGL's ops negotiate their
 * formats with the input's space), enhancer_buffer_to_texture() asks babl
 * for sRGB pixels and so gets the colorimetric conversion the preview
 * needs, and the savers write the space's profile back into the export.
 * Why not ggaze's loader plus a tag: gdk-pixbuf may or may not have
 * converted the pixels already (a glycin desktop does, fedora:40's native
 * loaders do not), and tagging converted pixels would manage them twice.
 * The EXIF Orientation is applied here, since GEGL's loaders do not.
 *
 * The managed path is an UPGRADE, never a new verdict: it either yields a
 * buffer it can vouch for or declines, and a declined file takes the
 * LOADER path -- ggaze's own loader, the same decode and the same errors
 * every file got before xb2, its pixels copied as sRGB. (Copied RIGHT
 * since xb2: before it the copy swapped red and blue and kept premultiplied
 * alpha -- see _load_via_loader -- so the enhance preview of every file,
 * untagged ones included, changed visibly with xb2: it is now what the
 * file holds.) It declines
 *   - a file with no profile, one babl cannot use (a LUT-only RGB profile:
 *     GEGL's loader would tag it sRGB anyway) or one babl identifies as
 *     sRGB -- nothing to manage, and the loader path is faster;
 *   - a profile for another number of colour components than the image
 *     stores (a grey profile on an RGB JPEG, an RGB one on a CMYK file):
 *     it cannot describe these pixels (_space_fits);
 *   - a non-local file (GEGL's loaders take a path) and a build whose GEGL
 *     lacks the loader op;
 *   - whatever the loader's own decode gate refuses (the same sniff, the
 *     same size caps), so the loader then refuses it with its usual error;
 *   - a file loader/intact.h does not vouch for: a truncated container
 *     (gegl:png-load / gegl:jpg-load start the file over on EOF and never
 *     return), a PNG whose image data libpng would reject (the op logs
 *     "failed to open file" and yields a black / partial buffer of the
 *     header's size, with no error), or a JPEG libjpeg gives up on or
 *     that ends before libjpeg is done with it (gegl:jpg-load has no
 *     longjmp handler: libjpeg's default exits the process, and at EOF its
 *     reader starts the file over into "two SOI markers") -- which, in a
 *     build without the `jpeg` feature, is every JPEG, since there is no
 *     libjpeg to check with. A camera-truncated JPEG (no EOI) decodes on
 *     the loader path as it always did;
 *   - a decode whose extent is not the header's (a corrupt header the
 *     checks above could not see);
 *   - a PNG whose iCCP libpng would drop, or that carries a gAMA, cHRM
 *     or sRGB chunk (icc_png_applied_profile): gegl:png-load would tag it
 *     with a space built from those chunks -- outside the slot cap below
 *     -- or with none, not with the profile vetted here.
 *
 * The file-swap window: the managed path opens the file four times for a
 * PNG, five for a JPEG -- the sniff, the profile walk, the completeness
 * walk, the libjpeg pass, GEGL's own open (libexif reads the orientation
 * once more) -- and only the walks before GEGL's vouch for the bytes GEGL
 * then reads. A file REPLACED between them by a truncated one can still
 * spin GEGL's loader in a worker that cannot be cancelled (GEGL processing
 * never can), and one replaced by a JPEG libjpeg gives up on can still
 * exit the process. Holding one descriptor would need GEGL to read from
 * it, which its loaders cannot; the window is kept to the time between
 * the last walk and GEGL's open (the completeness walk also reads the
 * stored size, so no separate header peek opens the file), and the same
 * race exists for gdk-pixbuf's path-taking calls on the loader path
 * (tech-stack.md). The same window lets a swapped-in file's profile (or
 * gAMA) reach babl through GEGL without the profile gate or the slot cap
 * below: a crafted profile could then crash babl or add a space outside
 * the cap. Nothing cheap narrows that further -- re-reading the profile
 * after GEGL's decode would only tell the damage afterwards -- so it is
 * documented (docs/gegl.md) rather than claimed closed.
 *
 * The checks that read a whole file (the PNG inflate, the libjpeg pass)
 * take the worker's GCancellable, so a superseded render stops checking
 * between blocks and declines; GEGL's decode itself, once started, runs to
 * its end. */

/* The loader path (section comment above): ggaze's own loader, so the EXIF
 * Orientation every backend honors (decision #26) is applied, pixels
 * copied into a GeglBuffer tagged sRGB (these decoders make no other
 * promise). The pixels are read in an EXPLICIT R8G8B8A8 layout through a
 * texture downloader, which converts whatever memory format the texture
 * has: gdk_texture_download() hands back GDK_MEMORY_DEFAULT --
 * premultiplied B8G8R8A8 on a little-endian host -- and copying that into
 * an "R'G'B'A u8" buffer swapped red and blue (found by xb2's WebP round
 * trip). The downloader is also why there is no gegl:load fallback for a
 * texture that is not RGBA8 any more: every texture converts. */
static GeglBuffer *
_load_via_loader(GFile *p_file, GCancellable *p_cancel, GError **p_err) {
   g_atomic_int_inc(&i_loader_decodes);
   GdkTexture *p_tex = loader_load(p_file, p_cancel, p_err);
   if (p_tex == NULL) {
      return (NULL);
   }
   const Babl           *p_fmt = babl_format("R'G'B'A u8");
   GeglRectangle         rect  = {0, 0, gdk_texture_get_width(p_tex),
                                  gdk_texture_get_height(p_tex)};
   GdkTextureDownloader *p_dl  = gdk_texture_downloader_new(p_tex);
   gdk_texture_downloader_set_format(p_dl, GDK_MEMORY_R8G8B8A8);
   gsize       u_stride = 0;
   GBytes     *p_bytes = gdk_texture_downloader_download_bytes(p_dl, &u_stride);
   GeglBuffer *p_buf   = gegl_buffer_new(&rect, p_fmt);
   gegl_buffer_set(p_buf, &rect, 0, p_fmt, g_bytes_get_data(p_bytes, NULL),
                   (gint)u_stride);
   g_bytes_unref(p_bytes);
   gdk_texture_downloader_free(p_dl);
   g_object_unref(p_tex);
   return (p_buf);
}

/* GEGL's ICC-aware loader for p_file, or NULL (with *pe_fmt) when the
 * managed path does not apply: the loader's decode gate on the first
 * bytes (read_all, as the loader reads them), then PNG / JPEG only, and
 * only when the op is installed (both ship with core GEGL). */
static const char *
_gegl_loader_for(GFile *p_file, GgazeFormat *pe_fmt) {
   guint8 c_head[GGAZE_DETECT_SNIFF_LEN];
   gssize i_n = loader_read_header(p_file, NULL, c_head, sizeof(c_head), NULL);
   const char *c_op = NULL;
   *pe_fmt          = GGAZE_FMT_UNKNOWN;
   if (i_n < 0 || !loader_sniff_bytes(c_head, (gsize)i_n, pe_fmt, NULL)) {
      return (NULL);
   }
   if (*pe_fmt == GGAZE_FMT_PNG) {
      c_op = "gegl:png-load";
   } else if (*pe_fmt == GGAZE_FMT_JPEG) {
      c_op = "gegl:jpg-load";
   }
   return (c_op != NULL && _has_op(c_op) ? c_op : NULL);
}

/* --- which profiles babl sees ---------------------------------------------
 *
 * babl_space_from_icc() is not safe on untrusted bytes, in four ways, and
 * the managed path hands it nothing it has not first vetted:
 *   - it reads tag data unchecked, and what it builds from some well-formed
 *     curves overruns its own buffers, trips its assertions or -- for a
 *     curve it cannot invert -- aborts a later conversion:
 *     icc_profile_is_sane() (icc.c) vouches for every tag babl reads and
 *     every curve it builds first;
 *   - on a CMYK profile it keeps whatever LCMS gives back, a NULL
 *     transform included, and the first conversion through that space
 *     crashes: the transform the decode converts through is built here
 *     first (_cmyk_profile_opens), and a profile it fails for declines;
 *   - its space and tone-curve tables are fixed arrays of 100 entries
 *     that are never freed (babl itself fills ~20 spaces and ~5 curves),
 *     and a full space table makes the next babl_space_from_icc()
 *     dereference NULL. So only GGAZE_ENHANCER_MAX_PROFILES distinct
 *     profiles per process that may grow them get to babl, each worth at
 *     most one space and four curves (babl parses rTRC, gTRC, bTRC AND
 *     kTRC of every profile, whatever its colour space): at most 16 + 20
 *     spaces and 64 + 5 curves. A profile costs its slot when babl is
 *     asked about it, whatever babl answers -- a declined one may have
 *     added its curves -- UNLESS babl provably added nothing: it answered
 *     with a space it already had (sRGB, or one an earlier slot's profile
 *     gave, whose curves it therefore had too) and the profile carries no
 *     curve that space does not use (a kTRC on an RGB profile is a new
 *     curve even then). So camera files whose sRGB profiles differ only
 *     in their bytes cost nothing, while no profile the enhancer hands
 *     babl adds to its tables without costing a slot. Past the cap a new
 *     profile is declined -- its file takes the loader path, sRGB, as
 *     before xb2. What GEGL's own loaders make of a file is held to the
 *     same bound by only letting them see files whose profile they will
 *     use: gegl:png-load builds a space and curve of its own from a PNG's
 *     gAMA / cHRM when libpng drops the iCCP (110 such files filled babl's
 *     tables past the cap), so a PNG is managed only when libpng keeps
 *     the iCCP and no such chunk is there (icc_png_applied_profile).
 *     The one way past the bound left is the file-swap window (the load
 *     section): a file replaced after these checks reaches GEGL, and so
 *     babl, unvetted -- its profile or gAMA may add a space per swap;
 *   - a space babl makes may be unusable by NAME, which is how babl and
 *     GEGL find its formats: too long (babl cuts format names at 255
 *     bytes and then confuses two formats) or another space's (babl
 *     names spaces only partly by content). _space_name_ok() declines
 *     those after babl has kept them, at the cost of their slot.
 * A profile babl would decline outright (icc_babl_kind: a class or PCS it
 * does not take, both CLUT directions, no curves or primaries) is never
 * handed to babl and costs nothing. Verdicts are kept per profile (SHA-256
 * of its bytes), so a file seen again costs a checksum and babl is asked
 * about a profile once: those that cost a slot for good (at most the cap
 * of them), slot-free ones up to GGAZE_ENHANCER_MAX_FREE_VERDICTS, the
 * oldest dropped first -- asking babl again about a dropped one is safe,
 * since it adds nothing. A profile babl is not asked about (declined by
 * kind, or a CMYK one LCMS cannot open) keeps no verdict: its checks are
 * header-deep (the LCMS open a few milliseconds) and run again.
 * Whoever asks first pays: the info card's enhancer_would_manage() goes
 * through the same table as the render, so a profile the card asked
 * about has its slot (or verdict) before the enhance ever runs, and the
 * enhance of that file then costs nothing more -- the cap counts distinct
 * profiles per process, not files, whichever path meets them.
 * GEGL's own loaders then make the same babl call on the same bytes, which
 * babl answers from its tables (the same profile gives the same space).
 * They make it four times per decode (the op's bounding box is queried
 * three times -- ours, the graph's prepare and prepare-request -- and the
 * decode reads the profile once more), which is what makes babl's grey
 * branch leak: it re-copies the profile onto the grey space it found each
 * time (docs/gegl.md, "Leaks we cannot fix"). */

#ifndef TYPE_CMYKA_DBL /* not in lcms2.h; babl defines it the same way */
#define TYPE_CMYKA_DBL                                                         \
   (FLOAT_SH(1) | COLORSPACE_SH(PT_CMYK) | EXTRA_SH(1) | CHANNELS_SH(4) |      \
    BYTES_SH(0))
#endif
#ifndef TYPE_RGBA_DBL
#define TYPE_RGBA_DBL                                                          \
   (FLOAT_SH(1) | COLORSPACE_SH(PT_RGB) | EXTRA_SH(1) | CHANNELS_SH(3) |       \
    BYTES_SH(0))
#endif

/* Whether LCMS builds the transform babl reads a CMYK image through
 * (babl-icc.c: CMYKA double -> the RGBA double of babl's scRGB profile,
 * relative colorimetric with black-point compensation). babl also builds
 * the reverse one, which only a conversion INTO the CMYK space uses --
 * never done here: the chain of a CMYK file runs in sRGB -- and which a
 * profile with no B2A tags (an input-only one, like cmyk-icc.jpg's)
 * legitimately cannot give; so it is not required. */
static gboolean
_cmyk_profile_opens(const char *c_data, gsize u_len) {
   int         i_rgb_len = 0;
   const char *c_rgb     = babl_space_get_icc(babl_space("scRGB"), &i_rgb_len);
   cmsHPROFILE p_cmyk = cmsOpenProfileFromMem(c_data, (cmsUInt32Number)u_len);
   cmsHPROFILE p_rgb = cmsOpenProfileFromMem(c_rgb, (cmsUInt32Number)i_rgb_len);
   gboolean    b_ok  = FALSE;
   if (p_cmyk != NULL && p_rgb != NULL) {
      cmsHTRANSFORM p_to = cmsCreateTransform(
         p_cmyk, TYPE_CMYKA_DBL, p_rgb, TYPE_RGBA_DBL,
         INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION);
      b_ok = p_to != NULL;
      g_clear_pointer(&p_to, cmsDeleteTransform);
   }
   g_clear_pointer(&p_cmyk, cmsCloseProfile);
   g_clear_pointer(&p_rgb, cmsCloseProfile);
   return (b_ok);
}

/* babl's space for a profile of kind e_kind (icc_babl_kind, not
 * ICC_BABL_NONE), or NULL; *pb_asked says whether babl was asked at all (a
 * CMYK profile LCMS cannot open never reaches it, and touches none of its
 * tables). */
static const Babl *
_babl_space_for(GBytes *p_icc, IccBablKind e_kind, gboolean *pb_asked) {
   gsize       u_len  = 0;
   const char *c_data = g_bytes_get_data(p_icc, &u_len);
   *pb_asked          = FALSE;
   if (e_kind == ICC_BABL_CMYK && !_cmyk_profile_opens(c_data, u_len)) {
      return (NULL);
   }
   const char *c_err = NULL;
   *pb_asked         = TRUE;
   /* Relative colorimetric, what gegl:png-load / gegl:jpg-load pass (spelt
    * out: babl 0.1.112, fedora:40's, has no BABL_ICC_INTENT_DEFAULT). */
   return (babl_space_from_icc(c_data, (int)u_len,
                               BABL_ICC_INTENT_RELATIVE_COLORIMETRIC, &c_err));
}

/* One kept verdict (section comment above). */
typedef struct {
   const Babl *p_space; /* babl's answer; NULL when it declined */
   gboolean    b_slot;  /* it cost a slot: kept for good */
} ProfileVerdict;

static GMutex      t_profiles_lock;
static GHashTable *p_verdicts  = NULL; /* SHA-256 hex -> ProfileVerdict */
static GQueue      t_free_keys = G_QUEUE_INIT; /* the slot-free verdicts'
                                                * keys, oldest first
                                                * (owned by the table) */
static guint u_profile_slots = 0;

/* Whether p_space is sRGB or a space a slot's profile already gave (a
 * slot-free verdict's space is one of those by construction). */
static gboolean
_space_known(const Babl *p_space) {
   if (p_space == babl_space("sRGB")) {
      return (TRUE);
   }
   GHashTableIter t_it;
   gpointer       p_val = NULL;
   g_hash_table_iter_init(&t_it, p_verdicts);
   while (g_hash_table_iter_next(&t_it, NULL, &p_val)) {
      const ProfileVerdict *p_v = p_val;
      if (p_v->b_slot && p_v->p_space == p_space) {
         return (TRUE);
      }
   }
   return (FALSE);
}

/* Whether babl's answer p_space for p_icc (kind e_kind) provably added
 * nothing to its tables: a space it already had, and no curve tag besides
 * the ones that space uses. A CMYK profile always adds a space (babl keeps
 * one per distinct profile), and a declined one may have added curves. */
static gboolean
_grew_nothing(const Babl *p_space, IccBablKind e_kind, GBytes *p_icc) {
   guint u_used = e_kind == ICC_BABL_GRAY ? 1 : 3;
   return (p_space != NULL && e_kind != ICC_BABL_CMYK &&
           _space_known(p_space) && icc_babl_curve_tags(p_icc) == u_used);
}

/* Keep the verdict for c_key (taken). A slot-free one joins the eviction
 * queue, and the oldest of those goes once there are more than
 * GGAZE_ENHANCER_MAX_FREE_VERDICTS. */
static void
_keep_verdict(char *c_key, const Babl *p_space, gboolean b_slot) {
   ProfileVerdict *p_v = g_new(ProfileVerdict, 1);
   p_v->p_space        = p_space;
   p_v->b_slot         = b_slot;
   g_hash_table_insert(p_verdicts, c_key, p_v);
   if (b_slot) {
      u_profile_slots++;
      return;
   }
   g_queue_push_tail(&t_free_keys, c_key);
   if (t_free_keys.length > GGAZE_ENHANCER_MAX_FREE_VERDICTS) {
      g_hash_table_remove(p_verdicts, g_queue_pop_head(&t_free_keys));
   }
}

/* Keep the longest format encoding (the name a format has in the default
 * space) in *(gsize *)p_data. */
static int
_longest_encoding(Babl *p_format, void *p_data) {
   const char *c_enc  = babl_format_get_encoding(p_format);
   gsize      *pu_max = p_data;
   if (c_enc != NULL) {
      *pu_max = MAX(*pu_max, strlen(c_enc));
   }
   return (0);
}

static guint u_max_space_name = 0; /* test seam; 0: babl's own limit */

/* babl's own limit (_space_name_limit), from its format table as it is
 * now. babl_format_class_for_each() walks that table without babl's own
 * lock, while a GEGL worker converting in a new space inserts formats
 * into it (a realloc under the walk): so it runs once, g_once, on the
 * thread that calls it first -- enhancer_babl_ready(), right after
 * gegl_init() on the main thread (app.c) and before any GEGL worker
 * exists. babl's and GEGL's own formats (and every GEGL plugin's) are
 * registered by then; formats a managed space adds later reuse their
 * encodings, so the limit holds for them, and no verdict depends on when
 * it was asked for. */
static gpointer
_babl_name_limit(gpointer p_unused) {
   (void)p_unused;
   gsize u_enc = 0;
   babl_format_class_for_each(_longest_encoding, &u_enc);
   guint u_max = u_enc + 1 < GGAZE_ENHANCER_FORMAT_NAME_MAX
                    ? (guint)(GGAZE_ENHANCER_FORMAT_NAME_MAX - 1 - u_enc)
                    : 0;
   return (GUINT_TO_POINTER(u_max + 1)); /* never NULL: g_once's "unset" */
}

static GOnce t_name_limit_once = G_ONCE_INIT;

void
enhancer_babl_ready(void) {
   (void)g_once(&t_name_limit_once, _babl_name_limit, NULL);
}

/* The longest space name a managed space may have: babl names each
 * format of a space "<encoding>-<space name>" in a 256-byte buffer, at
 * most GGAZE_ENHANCER_FORMAT_NAME_MAX characters (enhancer-gegl.h), so a
 * space name longer than that less the dash and the longest encoding
 * babl has registered would cut some format's name short: 229 with
 * babl's and GEGL's own formats ("CIE LCH(ab) alpha double", 24
 * characters). Computed once (_babl_name_limit), or the test seam's. */
static guint
_space_name_limit(void) {
   if (u_max_space_name > 0) {
      return (u_max_space_name);
   }
   gpointer p_max = g_once(&t_name_limit_once, _babl_name_limit, NULL);
   return (GPOINTER_TO_UINT(p_max) - 1);
}

guint
enhancer_max_space_name(void) {
   g_mutex_lock(&t_profiles_lock);
   guint u_max = _space_name_limit();
   g_mutex_unlock(&t_profiles_lock);
   return (u_max);
}

/* Whether babl's new space p_space can be managed by name: its name short
 * enough (_space_name_limit) and naming no other space. babl names spaces
 * it makes from a profile by its content only in part -- every grey
 * table-curve space is "space-gray-lut-trc", an RGB space's primaries go
 * to four decimals -- and babl_format_with_space() and GEGL find a space's
 * formats by that name: a second space under the first one's name would
 * decode and convert with the FIRST one's formats, silently in the wrong
 * colours. So a space babl_space() does not return by its own name is
 * declined. */
static gboolean
_space_name_ok(const Babl *p_space) {
   const char *c_name = babl_get_name(p_space);
   if (strlen(c_name) > _space_name_limit()) {
      g_debug("enhancer: not managed, babl's space name is %" G_GSIZE_FORMAT
              " characters long",
              strlen(c_name));
      return (FALSE);
   }
   if (babl_space(c_name) != p_space) {
      g_debug("enhancer: not managed, babl has another space named %s", c_name);
      return (FALSE);
   }
   return (TRUE);
}

/* The inputs _space_curves_ok() converts, and how far babl may be off
 * the profile's curve at them (xb2 review 8).
 *
 * Inputs: TRC_NDENSE points evenly over [0, 1] in the encoded domain, a
 * step of 1/63, plus TRC_KNEE_STEP either side of each channel's own knee
 * (icc_formula_curve_knee). Two curves babl < 0.1.114 confuses share
 * their type and gamma; where their linear segments or knees differ they
 * differ over a whole interval, which the even points find once it is
 * wider than a step and the knee points find down to TRC_KNEE_STEP (a
 * knee inside the other curve's power segment, or the reverse).
 *
 * Tolerance, in linear light: TRC_ABS_TOLERANCE plus TRC_REL_TOLERANCE of
 * the profile's value. Measured over 4097 inputs, identically on babl
 * 0.1.112 (fedora:40) and 0.1.128, for sRGB (type 3 and 4), Adobe RGB and
 * ProPhoto ('curv' gamma and type 0), ROMM, Rec. 709 / 2020, grey and
 * linear curves, gamma 1.02 to 3.0: babl's own polynomial approximation
 * of a formula curve is off by at most 3.1e-4, for a type 3 'para' of d 0
 * and gamma 1.1 to 1.2 at black alone, and by 7e-5 or less everywhere
 * else. The absolute term sits just above that worst case -- a relative
 * one cannot cover it, the profile's value there being 0 -- and near
 * black it is about one step of an 8-bit sRGB display (sRGB encodes
 * linear light 12.92 times steeper there), finer than that above. So a
 * curve babl swapped in that differs by more is caught: an offset of
 * 0.0019 (a Rec. 709 type 4 'para' with e = f = 0.0019 against the plain
 * type 3, ~6 sRGB steps at black), a knee at 0.025 against one at 0.095
 * (0.0047 apart at 0.095, ~16 steps against ~3). A swapped curve closer
 * than that passes, off by at most about one display step near black. A
 * curve babl approximates worse than the tolerance on its own -- a to-
 * linear gamma under 1 of d 0, 0.0053 off at black for 0.45 -- is
 * declined on any babl. The relative term keeps float rounding of values
 * near 1 from ever tipping a verdict. */
#define TRC_NDENSE 64u
#define TRC_KNEE_STEP (1.0 / 1024.0)
#define TRC_NMAX (TRC_NDENSE + 3u * 2u) /* three knees at most */
#define TRC_ABS_TOLERANCE 4e-4
#define TRC_REL_TOLERANCE 1e-3

/* The probe inputs for p_icc (grey: kTRC only, else rTRC gTRC bTRC). */
typedef struct {
   double f_x[TRC_NMAX];
   guint  u_n;
} TrcProbes;

static const char *const TRC_SIGS[] = {"rTRC", "gTRC", "bTRC"};

/* The tag of channel u_c (of 3, or kTRC alone for grey). */
static const char *
_trc_sig(gboolean b_gray, guint u_c) {
   return (b_gray ? "kTRC" : TRC_SIGS[u_c]);
}

/* TRC_NDENSE even inputs, then TRC_KNEE_STEP either side of each
 * channel's knee that leaves both inside [0, 1]. */
static void
_trc_probes(GBytes *p_icc, gboolean b_gray, TrcProbes *p_pr) {
   for (guint u = 0; u < TRC_NDENSE; u++) {
      p_pr->f_x[u] = u / (double)(TRC_NDENSE - 1);
   }
   p_pr->u_n = TRC_NDENSE;
   for (guint u_c = 0; u_c < (b_gray ? 1u : 3u); u_c++) {
      double f_d = 0;
      if (icc_formula_curve_knee(p_icc, _trc_sig(b_gray, u_c), &f_d) &&
          f_d - TRC_KNEE_STEP >= 0 && f_d + TRC_KNEE_STEP <= 1) {
         p_pr->f_x[p_pr->u_n++] = f_d - TRC_KNEE_STEP;
         p_pr->f_x[p_pr->u_n++] = f_d + TRC_KNEE_STEP;
      }
   }
}

/* babl's linear values for p_pr's inputs through p_space's curves, into
 * f_out (u_comps values per probe: 3, or 1 for a grey space):
 * "R'G'B' float" (grey: "Y' float") converted to its linear twin in the
 * same space, which is the curves and nothing else. */
static void
_space_curves_at(const Babl *p_space, gboolean b_gray, const TrcProbes *p_pr,
                 float *f_out) {
   guint u_comps = b_gray ? 1 : 3;
   float f_in[TRC_NMAX * 3];
   for (guint u = 0; u < p_pr->u_n * u_comps; u++) {
      f_in[u] = (float)p_pr->f_x[u / u_comps];
   }
   const Babl *p_from =
      babl_format_with_space(b_gray ? "Y' float" : "R'G'B' float", p_space);
   const Babl *p_to =
      babl_format_with_space(b_gray ? "Y float" : "RGB float", p_space);
   babl_process(babl_fish(p_from, p_to), f_in, f_out, p_pr->u_n);
}

/* Whether babl's values f_got (every u_comps-th, from channel u_c's) are
 * the profile's f_want at p_pr's inputs, within the tolerance above. */
static gboolean
_curve_matches(const float *f_got, guint u_comps, guint u_c,
               const double *f_want, const TrcProbes *p_pr) {
   for (guint u = 0; u < p_pr->u_n; u++) {
      double f_g   = f_got[u * u_comps + u_c];
      double f_tol = TRC_ABS_TOLERANCE + TRC_REL_TOLERANCE * fabs(f_want[u]);
      if (!(fabs(f_g - f_want[u]) <= f_tol)) {
         g_debug("enhancer: not managed, babl's curve %u gives %g at %g, "
                 "the profile's %g",
                 u_c, f_g, p_pr->f_x[u], f_want[u]);
         return (FALSE);
      }
   }
   return (TRUE);
}

/* Whether the space p_space babl answered with for p_icc (kind e_kind)
 * converts through the profile's own formula curves: babl < 0.1.114
 * keeps one formula curve per type and gamma, so a profile whose 'para'
 * shares its gamma with an earlier profile's but not its other parameters
 * gets the earlier one's curve -- and, with the same primaries, its very
 * space (icc.c, "the curve babl builds"; fedora:40 ships 0.1.112). Each
 * channel's curve is read back through babl (_space_curves_at) and
 * compared with icc_formula_curve_at(). The same check declines a curve
 * babl approximates too coarsely on any version (a type 3 'para' of
 * gamma 0.45 and d 0: 0.0053 at black). Table curves are not checked:
 * babl tells those apart byte for byte. A CMYK space has no curves of its
 * own (LCMS). */
static gboolean
_space_curves_ok(const Babl *p_space, IccBablKind e_kind, GBytes *p_icc) {
   gboolean  b_gray  = e_kind == ICC_BABL_GRAY;
   guint     u_comps = b_gray ? 1 : 3;
   TrcProbes t_pr;
   float     f_out[TRC_NMAX * 3];
   if (e_kind == ICC_BABL_CMYK) {
      return (TRUE);
   }
   _trc_probes(p_icc, b_gray, &t_pr);
   _space_curves_at(p_space, b_gray, &t_pr, f_out);
   for (guint u_c = 0; u_c < u_comps; u_c++) {
      double f_want[TRC_NMAX];
      if (!icc_formula_curve_at(p_icc, _trc_sig(b_gray, u_c), t_pr.f_x, f_want,
                                t_pr.u_n)) {
         continue; /* a table curve */
      }
      if (!_curve_matches(f_out, u_comps, u_c, f_want, &t_pr)) {
         return (FALSE);
      }
   }
   return (TRUE);
}

/* Whether babl's answer p_space (not NULL) for p_icc can be managed: by
 * name, and converting through the profile's own curves. babl's sRGB
 * needs neither check -- it is never managed (_space_of_profile), and a
 * table curve babl found close to sRGB's would fail the second for
 * nothing. */
static gboolean
_space_usable(const Babl *p_space, IccBablKind e_kind, GBytes *p_icc) {
   return (
      p_space == babl_space("sRGB") ||
      (_space_name_ok(p_space) && _space_curves_ok(p_space, e_kind, p_icc)));
}

/* A new verdict for p_icc (kind e_kind, key c_key taken), under the lock:
 * babl's answer while the cap allows, NULL past it. A space babl answered
 * with that _space_usable() refuses is declined AFTER babl has kept it:
 * the verdict is NULL and it costs its slot like any other growth --
 * whether or not this profile grew babl's tables, which the curve check
 * cannot tell (a space babl reused for it had the wrong curve; one babl
 * made may have added curves). */
static const Babl *
_ask_babl(GBytes *p_icc, IccBablKind e_kind, char *c_key) {
   if (u_profile_slots >= GGAZE_ENHANCER_MAX_PROFILES) {
      g_debug("enhancer: not managed, %u distinct profiles already in babl",
              u_profile_slots);
      g_free(c_key);
      return (NULL);
   }
   gboolean    b_asked = FALSE;
   const Babl *p_space = _babl_space_for(p_icc, e_kind, &b_asked);
   if (!b_asked) {
      g_free(c_key);
      return (NULL);
   }
   gboolean b_slot = !_grew_nothing(p_space, e_kind, p_icc);
   if (p_space != NULL && !_space_usable(p_space, e_kind, p_icc)) {
      p_space = NULL;
      b_slot  = TRUE;
   }
   _keep_verdict(c_key, p_space, b_slot);
   return (p_space);
}

/* p_icc's babl space through the verdict table (section comment above):
 * NULL for a profile babl would decline (never asked), the kept verdict
 * for one seen before, else a new one. */
static const Babl *
_profile_space(GBytes *p_icc) {
   IccBablKind e_kind = icc_babl_kind(p_icc); /* icc_profile_is_sane too */
   if (e_kind == ICC_BABL_NONE) {
      return (NULL);
   }
   char       *c_key   = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, p_icc);
   const Babl *p_space = NULL;
   g_mutex_lock(&t_profiles_lock);
   if (p_verdicts == NULL) {
      p_verdicts =
         g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
   }
   const ProfileVerdict *p_v = g_hash_table_lookup(p_verdicts, c_key);
   if (p_v != NULL) {
      p_space = p_v->p_space;
      g_free(c_key);
   } else {
      p_space = _ask_babl(p_icc, e_kind, c_key);
   }
   g_mutex_unlock(&t_profiles_lock);
   return (p_space);
}

/* The space of a profile worth managing: one icc_babl_kind() (so
 * icc_profile_is_sane) lets through and babl parses (a matrix/TRC RGB, a
 * grey TRC, or -- through babl's LCMS -- a CMYK one) to a space other than
 * sRGB (babl hands back its own sRGB space for a profile equivalent to
 * it). NULL otherwise. */
static const Babl *
_space_of_profile(GBytes *p_icc) {
   const Babl *p_space = _profile_space(p_icc);
   return (p_space == babl_space("sRGB") ? NULL : p_space);
}

/* _space_of_profile() for the profile GEGL's loader will tag p_file's
 * decoded buffer with: a JPEG's embedded one, and for a PNG (e_fmt) the
 * iCCP libpng keeps -- no sRGB chunk beside it, any gAMA / cHRM one libpng
 * takes without dropping the iCCP (icc_png_applied_profile). Any other PNG
 * is declined, since GEGL would tag it with a space it builds from gAMA /
 * cHRM, outside the slot cap (xb2 review 5), or with none; a kept iCCP
 * wins over those chunks in gegl:png-load, so none is built then. */
static const Babl *
_managed_space(GFile *p_file, GgazeFormat e_fmt) {
   GBytes     *p_icc = e_fmt == GGAZE_FMT_PNG ? icc_png_applied_profile(p_file)
                                              : icc_read_embedded(p_file, NULL);
   const Babl *p_space = p_icc != NULL ? _space_of_profile(p_icc) : NULL;
   if (p_icc != NULL) {
      g_bytes_unref(p_icc);
   }
   return (p_space);
}

const Babl *
enhancer_test_profile_space(GBytes *p_icc) {
   g_return_val_if_fail(p_icc != NULL, NULL);
   return (_space_of_profile(p_icc));
}

gboolean
enhancer_test_profile_is_managed(GBytes *p_icc) {
   return (enhancer_test_profile_space(p_icc) != NULL);
}

guint
enhancer_test_profile_slots(void) {
   g_mutex_lock(&t_profiles_lock);
   guint u_slots = u_profile_slots;
   g_mutex_unlock(&t_profiles_lock);
   return (u_slots);
}

void
enhancer_test_set_max_space_name(guint u_max) {
   g_mutex_lock(&t_profiles_lock);
   u_max_space_name = u_max; /* 0: babl's own limit again */
   g_mutex_unlock(&t_profiles_lock);
}

guint
enhancer_test_profile_verdicts(void) {
   g_mutex_lock(&t_profiles_lock);
   guint u_n = p_verdicts != NULL ? g_hash_table_size(p_verdicts) : 0;
   g_mutex_unlock(&t_profiles_lock);
   return (u_n);
}

/* loader/intact.h vouches for p_file and its stored size is within the
 * shared caps: the only files GEGL's loaders may see (*p_size set). A
 * JPEG must also get through libjpeg once (intact_jpeg_decodes, EOF
 * fatal), after the caps: gegl:jpg-load exits the process where libjpeg
 * gives up, and on a file cut short. p_cancel (nullable) stops the whole-
 * file passes between blocks; a cancelled check declines like any other
 * (the caller's own cancellation check drops the result). */
static gboolean
_vouched(GFile *p_file, GgazeFormat e_fmt, GCancellable *p_cancel,
         IntactSize *p_size) {
   GError  *p_err = NULL;
   gboolean b_png = e_fmt == GGAZE_FMT_PNG;
   gboolean b_ok  = b_png ? intact_png(p_file, p_cancel, p_size, &p_err)
                          : intact_jpeg(p_file, p_cancel, p_size, &p_err);
   if (b_ok) {
      b_ok = detect_dims_within_bounds("enhancer", p_size->u_w, p_size->u_h,
                                       NULL, &p_err);
   }
   if (b_ok && !b_png) {
      b_ok = intact_jpeg_decodes(p_file, p_cancel, &p_err);
   }
   if (!b_ok) {
      g_debug("enhancer: not managed, the loader decodes it: %s",
              p_err->message);
      g_error_free(p_err);
   }
   return (b_ok);
}

/* Whether the profile space p_space describes an image of u_comps colour
 * components (IntactSize): a grey profile one, a CMYK profile four, an RGB
 * one three. libpng and libjpeg do not check this for GEGL's loaders --
 * an RGB JPEG carrying a grey profile decodes tagged with the grey space,
 * and the conversion then reads its RGB as something else -- so a profile
 * for another number of components is declined here: it cannot describe
 * these pixels, and the loader path shows them as sRGB, as before xb2. */
static gboolean
_space_fits(const Babl *p_space, guint u_comps) {
   guint u_want = babl_space_is_gray(p_space)   ? 1
                  : babl_space_is_cmyk(p_space) ? 4
                                                : 3;
   if (u_comps != u_want) {
      g_debug("enhancer: not managed, a %u-component profile on a "
              "%u-component image",
              u_want, u_comps);
      return (FALSE);
   }
   return (TRUE);
}

/* Whether a w x h extent is the header's stored size. */
static gboolean
_extent_is(gint i_w, gint i_h, const IntactSize *p_size) {
   return ((guint32)i_w == p_size->u_w && (guint32)i_h == p_size->u_h);
}

/* Run the loader op c_op on c_path into a new buffer, or NULL. GEGL's
 * loaders report a failed decode by yielding no buffer or an empty /
 * odd-sized one, never an error (a corrupt PNG's image data -- a black
 * header-sized buffer -- is intact_png's to catch beforehand), so the
 * extent is checked against the header's stored size twice: the op's
 * bounding box BEFORE processing (a header the decoder itself rejects --
 * libjpeg's "two SOF markers" -- gives an empty box, and processing that
 * would only earn a GEGL "0px rectangle" warning), and the produced buffer
 * after it. */
static GeglBuffer *
_load_via_gegl_op(const char *c_op, const char *c_path,
                  const IntactSize *p_size) {
   GeglBuffer *p_buf   = NULL;
   GeglNode   *p_graph = gegl_node_new();
   GeglNode   *p_load =
      gegl_node_new_child(p_graph, "operation", c_op, "path", c_path, NULL);
   GeglRectangle t_box = gegl_node_get_bounding_box(p_load);
   if (_extent_is(t_box.width, t_box.height, p_size)) {
      GeglNode *p_sink = gegl_node_new_child(
         p_graph, "operation", "gegl:buffer-sink", "buffer", &p_buf, NULL);
      gegl_node_link(p_load, p_sink);
      gegl_node_process(p_sink);
   }
   g_object_unref(p_graph);
   if (p_buf == NULL || !_extent_is(gegl_buffer_get_width(p_buf),
                                    gegl_buffer_get_height(p_buf), p_size)) {
      g_debug("enhancer: %s did not decode %s at its stored %ux%u; the "
              "loader decodes it",
              c_op, c_path, p_size->u_w, p_size->u_h);
      g_clear_object(&p_buf);
   }
   return (p_buf);
}

static void
_free_pixels(guchar *p_pixels, gpointer p_data) {
   (void)p_data;
   g_free(p_pixels);
}

/* The working space the chain runs in for a decoded buffer: the image's
 * own space when it is an RGB one (a matrix/TRC profile babl parsed), so
 * presets and export stay in the source's gamut; sRGB for a CMYK or grey
 * profile (a CMYK JPEG with its press profile), whose pixels have no
 * meaning in an "R'G'B'A" format of that space -- the gegl_buffer_get()
 * below then does the colorimetric CMYK/grey -> sRGB conversion. So an
 * sRGB-tagged buffer does NOT mean "not managed": _load reports that
 * separately (the hold-Space compare needs to know). */
static const Babl *
_working_space(GeglBuffer *p_buf) {
   const Babl *p_space = babl_format_get_space(gegl_buffer_get_format(p_buf));
   if (p_space == NULL || babl_space_is_cmyk(p_space) ||
       babl_space_is_gray(p_space)) {
      return (babl_space("sRGB"));
   }
   return (p_space);
}

/* p_buf as the RGBA8 layout the chain has always run on, in the working
 * space (above), with the EXIF Orientation i_orient (1-8, 0 = none)
 * applied. The permutation is pixbuf-util's (the one every loader backend
 * uses), fed through a GdkPixbuf wrapped around the pixels with the
 * orientation set as its option; the pixbuf borrows nothing after the copy
 * out. */
static GeglBuffer *
_rgba8_upright(GeglBuffer *p_buf, int i_orient) {
   const Babl *p_fmt =
      babl_format_with_space("R'G'B'A u8", _working_space(p_buf));
   const GeglRectangle *p_rect   = gegl_buffer_get_extent(p_buf);
   int                  i_stride = p_rect->width * 4;
   guint8 *p_px = g_malloc((gsize)i_stride * (gsize)p_rect->height);
   gegl_buffer_get(p_buf, p_rect, 1.0, p_fmt, p_px, i_stride, GEGL_ABYSS_NONE);
   GdkPixbuf *p_pix =
      gdk_pixbuf_new_from_data(p_px, GDK_COLORSPACE_RGB, TRUE, 8, p_rect->width,
                               p_rect->height, i_stride, _free_pixels, NULL);
   if (i_orient > 1) {
      char c_orient[4];
      g_snprintf(c_orient, sizeof(c_orient), "%d", i_orient);
      gdk_pixbuf_set_option(p_pix, "orientation", c_orient);
   }
   GdkPixbuf    *p_up  = pixbuf_util_upright(p_pix);
   GeglRectangle t_out = {0, 0, gdk_pixbuf_get_width(p_up),
                          gdk_pixbuf_get_height(p_up)};
   GeglBuffer   *p_out = gegl_buffer_new(&t_out, p_fmt);
   gegl_buffer_set(p_out, &t_out, 0, p_fmt, gdk_pixbuf_read_pixels(p_up),
                   gdk_pixbuf_get_rowstride(p_up));
   g_object_unref(p_up);
   g_object_unref(p_pix);
   return (p_out);
}

/* The managed path (section comment above): a buffer, or NULL when it
 * declines -- never an error of its own, the caller then decides (a
 * decline the cancellation caused included: GEGL's decode, which nothing
 * can stop, is not started once p_cancel has fired). */
static GeglBuffer *
_load_managed(GFile *p_file, const char *c_path, GCancellable *p_cancel) {
   if (p_load_hook != NULL) {
      p_load_hook(p_load_hook_data);
   }
   GgazeFormat e_fmt   = GGAZE_FMT_UNKNOWN;
   const char *c_op    = _gegl_loader_for(p_file, &e_fmt);
   const Babl *p_space = c_op != NULL ? _managed_space(p_file, e_fmt) : NULL;
   IntactSize  t_size;
   if (p_space == NULL || !_vouched(p_file, e_fmt, p_cancel, &t_size) ||
       !_space_fits(p_space, t_size.u_comps) ||
       g_cancellable_is_cancelled(p_cancel)) {
      return (NULL);
   }
   GeglBuffer *p_raw = _load_via_gegl_op(c_op, c_path, &t_size);
   if (p_raw == NULL) {
      return (NULL);
   }
   /* PNG has no EXIF orientation the loader would honour; JPEG does. */
   int i_orient = e_fmt == GGAZE_FMT_JPEG ? info_exif_orientation(c_path) : 0;
   GeglBuffer *p_out = _rgba8_upright(p_raw, i_orient);
   g_object_unref(p_raw);
   return (p_out);
}

/* _load_managed() for p_file when it is local, else NULL. */
static GeglBuffer *
_load_managed_file(GFile *p_file, GCancellable *p_cancel) {
   char       *c_path = g_file_get_path(p_file);
   GeglBuffer *p_buf =
      c_path != NULL ? _load_managed(p_file, c_path, p_cancel) : NULL;
   g_free(c_path);
   return (p_buf);
}

/* enhancer_load with the worker's GCancellable (nullable) for the managed
 * path's whole-file checks and the loader's decode, and *pb_managed
 * (nullable) told whether the managed path decoded the file -- its
 * embedded profile applied, whatever the working space (a CMYK or grey
 * file's buffer is sRGB, yet its pixels are the profile's, not the
 * loader's). A cancelled load returns G_IO_ERROR_CANCELLED WITHOUT
 * decoding: a check the cancellation cut short declines like a failed one,
 * and falling through to the loader would decode the whole file for a
 * result nobody takes. */
static GeglBuffer *
_load(GFile *p_file, GCancellable *p_cancel, gboolean *pb_managed,
      GError **p_err) {
   GeglBuffer *p_buf = _load_managed_file(p_file, p_cancel);
   if (pb_managed != NULL) {
      *pb_managed = p_buf != NULL;
   }
   if (p_buf != NULL || g_cancellable_set_error_if_cancelled(p_cancel, p_err)) {
      return (p_buf);
   }
   return (_load_via_loader(p_file, p_cancel, p_err));
}

/* The managed path's own gates, header-deep: _load_managed's choices up to
 * (not including) the completeness checks, with the size and component
 * count from the headers alone -- intact_png_header for a PNG (the caps
 * included), the (headers-only) intact_jpeg walk plus the same caps for a
 * JPEG -- and the JPEG's libjpeg requirement as a build fact, since the
 * libjpeg pass declines every JPEG without it. The whole-file checks stay
 * out: measured on ./sample-images (106 profiled files), the header gates
 * take ~0.02 ms a file, the PNG inflate / libjpeg 1/8 pass ~150 ms on
 * average and ~0.9 s at most, on every `i` press -- so the card says
 * "may be managed" instead of promising what only the enhance-time checks
 * can. */
gboolean
enhancer_would_manage(GFile *p_file) {
   g_return_val_if_fail(G_IS_FILE(p_file), FALSE);
   char       *c_path = g_file_get_path(p_file);
   GgazeFormat e_fmt  = GGAZE_FMT_UNKNOWN;
   const char *c_op  = c_path != NULL ? _gegl_loader_for(p_file, &e_fmt) : NULL;
   gboolean    b_png = e_fmt == GGAZE_FMT_PNG;
   const Babl *p_space = NULL;
   IntactSize  t_size  = {0, 0, 0};
   g_free(c_path);
   if (c_op != NULL && (b_png || GGAZE_HAVE_JPEG)) {
      p_space = _managed_space(p_file, e_fmt);
   }
   if (p_space == NULL) {
      return (FALSE);
   }
   gboolean b_hdr = b_png
                       ? intact_png_header(p_file, &t_size, NULL)
                       : intact_jpeg(p_file, NULL, &t_size, NULL) &&
                            detect_dims_within_bounds("enhancer", t_size.u_w,
                                                      t_size.u_h, NULL, NULL);
   return (b_hdr && _space_fits(p_space, t_size.u_comps));
}

GeglBuffer *
enhancer_load(GFile *p_file, GError **p_err) {
   g_return_val_if_fail(p_file != NULL, NULL);
   return (_load(p_file, NULL, NULL, p_err));
}

/* Asks babl for sRGB pixels ("R'G'B'A u8" without a space IS sRGB): for a
 * buffer tagged with another space this is the colorimetric conversion that
 * makes the preview's colours right on an sRGB display (decision #45); for
 * an sRGB buffer it is the plain copy it always was. */
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
 * the controller records for the crop tool, and whether the decode was
 * colour-managed (see the finish doc). */
typedef struct {
   GdkTexture *p_tex; /* owned */
   gint        i_orig_w;
   gint        i_orig_h;
   gboolean    b_managed;
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
   _ApplyResult *p_res = g_new0(_ApplyResult, 1);
   GeglBuffer   *p_buf =
      _load(p_req->p_file, p_cancel, &p_res->b_managed, &p_err);
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
                            gboolean *pb_managed, GError **p_err) {
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
   if (pb_managed != NULL) {
      *pb_managed = p_out->b_managed;
   }
   GdkTexture *p_tex = g_steal_pointer(&p_out->p_tex);
   _apply_result_free(p_out);
   return (p_tex);
}

/* --- the managed original (hold-Space on a colour-managed file) ---------
 *
 * Built only when asked for -- the first Space press on a render whose
 * decode was managed -- not with every render: it is a second full-size
 * texture (w x h x 4 bytes: ~100 MB at 24 MP, ~200 MB at 50 MP) outside
 * the texture cache's cap, which most enhance sessions never look at. The
 * worker decodes the file again through the same _load() as the render,
 * so the original and the render share one colour pipeline and the
 * compare shows only what the presets did. */

static void
_managed_original_thread(GTask *p_task, gpointer p_src, gpointer p_task_data,
                         GCancellable *p_cancel) {
   (void)p_task_data;
   if (g_task_return_error_if_cancelled(p_task)) {
      return;
   }
   /* The managed path alone: a decline (the file rewritten meanwhile, or
    * past the profile cap) is "no managed original", never a plain decode
    * -- the caller already shows that one. */
   GError     *p_err = NULL;
   GeglBuffer *p_buf = _load_managed_file(G_FILE(p_src), p_cancel);
   GdkTexture *p_tex = NULL;
   if (p_buf != NULL) {
      p_tex = enhancer_buffer_to_texture(p_buf, &p_err);
   } else {
      g_cancellable_set_error_if_cancelled(p_cancel, &p_err);
   }
   g_clear_object(&p_buf);
   if (p_err != NULL) {
      g_task_return_error(p_task, p_err);
   } else {
      g_task_return_pointer(p_task, p_tex, g_object_unref);
   }
}

void
enhancer_managed_original_async(GFile *p_file, GCancellable *p_cancel,
                                GAsyncReadyCallback p_cb, gpointer p_data) {
   g_return_if_fail(G_IS_FILE(p_file));
   GTask *p_task = g_task_new(p_file, p_cancel, p_cb, p_data);
   g_task_run_in_thread(p_task, _managed_original_thread);
   g_object_unref(p_task);
}

GdkTexture *
enhancer_managed_original_finish(GAsyncResult *p_res, GError **p_err) {
   g_return_val_if_fail(G_IS_TASK(p_res), NULL);
   return (g_task_propagate_pointer(G_TASK(p_res), p_err));
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
