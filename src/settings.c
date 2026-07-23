/* settings.c — GSettings wrapper (org.buetow.ggaze). */
#include "settings.h"

#include <glib.h>

struct Settings {
   GSettings *p_gs;
};

void
settings_pair_free(gpointer p) {
   SettingsPair *pr = (SettingsPair *)p;
   if (pr == NULL) {
      return;
   }
   g_free(pr->c_name);
   g_free(pr->c_value);
   g_free(pr);
}

static GPtrArray *
_new_pair_array(void) {
   return (g_ptr_array_new_with_free_func(settings_pair_free));
}

/* Read an a(ss) key into a fresh GPtrArray of SettingsPair*. The key is read
 * via g_settings_get_value so the variant type is exact; a missing/empty key
 * yields a zero-length array. */
static GPtrArray *
_get_pairs(GSettings *p_gs, const char *c_key) {
   GPtrArray   *p_out = _new_pair_array();
   GVariant    *p_v   = g_settings_get_value(p_gs, c_key);
   GVariantIter iter;
   g_variant_iter_init(&iter, p_v);
   const gchar *c_name;
   const gchar *c_value;
   while (g_variant_iter_next(&iter, "(ss)", &c_name, &c_value)) {
      SettingsPair *pr = g_new(SettingsPair, 1);
      pr->c_name       = g_strdup(c_name);
      pr->c_value      = g_strdup(c_value);
      g_ptr_array_add(p_out, pr);
      g_free((gpointer)c_name);
      g_free((gpointer)c_value);
   }
   g_variant_unref(p_v);
   return p_out;
}

gboolean
settings_pair_valid(const char *c_name, const char *c_value,
                    gboolean b_require_path) {
   if (c_name == NULL || *c_name == '\0' || c_value == NULL ||
       *c_value == '\0') {
      return (FALSE);
   }
   if (b_require_path && !g_path_is_absolute(c_value)) {
      return (FALSE);
   }
   return (TRUE);
}

/* Build an a(ss) GVariant from the validated entries of p_pairs and store it.
 * Returns the number of accepted entries. */
static guint
_set_pairs(Settings *p_s, const char *c_key, const GPtrArray *p_pairs,
           gboolean b_require_path) {
   g_return_val_if_fail(p_s != NULL, 0);
   GVariantBuilder *p_b  = g_variant_builder_new(G_VARIANT_TYPE("a(ss)"));
   guint            u_ok = 0;
   if (p_pairs != NULL) {
      for (guint i = 0; i < p_pairs->len; i++) {
         const SettingsPair *pr =
            (const SettingsPair *)g_ptr_array_index((GPtrArray *)p_pairs, i);
         if (pr == NULL) {
            continue;
         }
         if (!settings_pair_valid(pr->c_name, pr->c_value, b_require_path)) {
            continue;
         }
         g_variant_builder_add(p_b, "(ss)", pr->c_name, pr->c_value);
         u_ok++;
      }
   }
   GVariant *p_v = g_variant_builder_end(p_b);
   g_variant_builder_unref(p_b);
   g_settings_set_value(p_s->p_gs, c_key, p_v);
   return u_ok;
}

Settings *
settings_new(void) {
   GSettings *p_gs = g_settings_new(GGAZE_SETTINGS_SCHEMA_ID);
   return settings_new_for_gsettings(p_gs);
}

Settings *
settings_new_for_gsettings(GSettings *p_gs) {
   g_return_val_if_fail(G_IS_SETTINGS(p_gs), NULL);
   Settings *p_s = g_new0(Settings, 1);
   p_s->p_gs     = p_gs; /* adopt the caller's ref */
   return p_s;
}

void
settings_delete(Settings *p_s) {
   if (p_s == NULL) {
      return;
   }
   g_clear_object(&p_s->p_gs);
   g_free(p_s);
}

GSettings *
settings_get_gsettings(Settings *p_s) {
   g_return_val_if_fail(p_s != NULL, NULL);
   return p_s->p_gs;
}

/* --- scalar keys --------------------------------------------------------- */

GgazeSort
settings_get_sort(Settings *p_s) {
   g_return_val_if_fail(p_s != NULL, GGAZE_SORT_NAME);
   return ((GgazeSort)g_settings_get_enum(p_s->p_gs, "sort"));
}

void
settings_set_sort(Settings *p_s, GgazeSort e_sort) {
   g_return_if_fail(p_s != NULL);
   g_settings_set_enum(p_s->p_gs, "sort", (gint)e_sort);
}

gboolean
settings_get_wrap(Settings *p_s) {
   g_return_val_if_fail(p_s != NULL, FALSE);
   return (g_settings_get_boolean(p_s->p_gs, "wrap"));
}

void
settings_set_wrap(Settings *p_s, gboolean b_wrap) {
   g_return_if_fail(p_s != NULL);
   g_settings_set_boolean(p_s->p_gs, "wrap", b_wrap);
}

GgazeBackground
settings_get_background(Settings *p_s) {
   g_return_val_if_fail(p_s != NULL, GGAZE_BG_DARK);
   return ((GgazeBackground)g_settings_get_enum(p_s->p_gs, "background"));
}

void
settings_set_background(Settings *p_s, GgazeBackground e_bg) {
   g_return_if_fail(p_s != NULL);
   g_settings_set_enum(p_s->p_gs, "background", (gint)e_bg);
}

GgazeScrollBehavior
settings_get_scroll_behavior(Settings *p_s) {
   g_return_val_if_fail(p_s != NULL, GGAZE_SCROLL_ZOOM);
   return (
      (GgazeScrollBehavior)g_settings_get_enum(p_s->p_gs, "scroll-behavior"));
}

void
settings_set_scroll_behavior(Settings *p_s, GgazeScrollBehavior e_sb) {
   g_return_if_fail(p_s != NULL);
   g_settings_set_enum(p_s->p_gs, "scroll-behavior", (gint)e_sb);
}

gdouble
settings_get_slideshow_delay(Settings *p_s) {
   g_return_val_if_fail(p_s != NULL, 3.0);
   return (g_settings_get_double(p_s->p_gs, "slideshow-delay"));
}

void
settings_set_slideshow_delay(Settings *p_s, gdouble d_sec) {
   g_return_if_fail(p_s != NULL);
   g_settings_set_double(p_s->p_gs, "slideshow-delay", d_sec);
}

gint
settings_get_thumbnail_size(Settings *p_s) {
   g_return_val_if_fail(p_s != NULL, 128);
   return (g_settings_get_int(p_s->p_gs, "thumbnail-size"));
}

void
settings_set_thumbnail_size(Settings *p_s, gint i_px) {
   g_return_if_fail(p_s != NULL);
   g_settings_set_int(p_s->p_gs, "thumbnail-size", i_px);
}

gboolean
settings_get_hide_trashed(Settings *p_s) {
   g_return_val_if_fail(p_s != NULL, FALSE);
   return (g_settings_get_boolean(p_s->p_gs, "hide-trashed"));
}

void
settings_set_hide_trashed(Settings *p_s, gboolean b_hide) {
   g_return_if_fail(p_s != NULL);
   g_settings_set_boolean(p_s->p_gs, "hide-trashed", b_hide);
}

gboolean
settings_get_hide_raw(Settings *p_s) {
   g_return_val_if_fail(p_s != NULL, TRUE);
   return (g_settings_get_boolean(p_s->p_gs, "hide-raw-sidecars"));
}

void
settings_set_hide_raw(Settings *p_s, gboolean b_hide) {
   g_return_if_fail(p_s != NULL);
   g_settings_set_boolean(p_s->p_gs, "hide-raw-sidecars", b_hide);
}

/* --- ordered a(ss) lists ------------------------------------------------- */

GPtrArray *
settings_get_destinations(Settings *p_s) {
   g_return_val_if_fail(p_s != NULL, _new_pair_array());
   return _get_pairs(p_s->p_gs, "destinations");
}

GPtrArray *
settings_get_editors(Settings *p_s) {
   g_return_val_if_fail(p_s != NULL, _new_pair_array());
   return _get_pairs(p_s->p_gs, "editors");
}

GPtrArray *
settings_get_scripts(Settings *p_s) {
   g_return_val_if_fail(p_s != NULL, _new_pair_array());
   return _get_pairs(p_s->p_gs, "scripts");
}

GPtrArray *
settings_get_enhance_presets(Settings *p_s) {
   g_return_val_if_fail(p_s != NULL, _new_pair_array());
   return _get_pairs(p_s->p_gs, "enhance-presets");
}

guint
settings_set_destinations(Settings *p_s, const GPtrArray *p_pairs) {
   return _set_pairs(p_s, "destinations", p_pairs, TRUE);
}

guint
settings_set_editors(Settings *p_s, const GPtrArray *p_pairs) {
   return _set_pairs(p_s, "editors", p_pairs, FALSE);
}

guint
settings_set_scripts(Settings *p_s, const GPtrArray *p_pairs) {
   return _set_pairs(p_s, "scripts", p_pairs, FALSE);
}

guint
settings_set_enhance_presets(Settings *p_s, const GPtrArray *p_pairs) {
   return _set_pairs(p_s, "enhance-presets", p_pairs, FALSE);
}