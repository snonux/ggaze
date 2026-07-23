/* test_settings.c — plain-C unit tests for the GSettings wrapper.
 *
 * Uses the memory GSettings backend (GSETTINGS_BACKEND=memory, set in the
 * meson env) and the schema compiled into the build tree (GSETTINGS_SCHEMA_DIR
 * -> build/data). No display needed; no user dconf is touched. */
#include "settings.h"

#include <glib.h>

static Settings *
new_settings(void) {
   Settings *p_s = settings_new();
   g_assert_nonnull(p_s);
   /* Start each test from a clean slate: reset everything to defaults. */
   g_settings_reset(settings_get_gsettings(p_s), "sort");
   g_settings_reset(settings_get_gsettings(p_s), "wrap");
   g_settings_reset(settings_get_gsettings(p_s), "background");
   g_settings_reset(settings_get_gsettings(p_s), "scroll-behavior");
   g_settings_reset(settings_get_gsettings(p_s), "slideshow-delay");
   g_settings_reset(settings_get_gsettings(p_s), "thumbnail-size");
   g_settings_reset(settings_get_gsettings(p_s), "hide-trashed");
   g_settings_reset(settings_get_gsettings(p_s), "hide-raw-sidecars");
   g_settings_reset(settings_get_gsettings(p_s), "destinations");
   g_settings_reset(settings_get_gsettings(p_s), "editors");
   g_settings_reset(settings_get_gsettings(p_s), "scripts");
   g_settings_reset(settings_get_gsettings(p_s), "enhance-presets");
   return p_s;
}

static void
test_scalars_roundtrip(void) {
   Settings *p_s = new_settings();
   /* Defaults from the schema. */
   g_assert_cmpint(settings_get_sort(p_s), ==, GGAZE_SORT_NAME);
   g_assert_cmpint(settings_get_background(p_s), ==, GGAZE_BG_DARK);
   g_assert_cmpint(settings_get_scroll_behavior(p_s), ==, GGAZE_SCROLL_ZOOM);
   g_assert_true(settings_get_wrap(p_s));
   g_assert_false(settings_get_hide_trashed(p_s));
   g_assert_true(settings_get_hide_raw(p_s));
   g_assert_cmpfloat(settings_get_slideshow_delay(p_s), ==, 3.0);
   g_assert_cmpint(settings_get_thumbnail_size(p_s), ==, 128);

   /* Round-trip every scalar. */
   settings_set_sort(p_s, GGAZE_SORT_SIZE);
   g_assert_cmpint(settings_get_sort(p_s), ==, GGAZE_SORT_SIZE);
   settings_set_background(p_s, GGAZE_BG_CHECKER);
   g_assert_cmpint(settings_get_background(p_s), ==, GGAZE_BG_CHECKER);
   settings_set_scroll_behavior(p_s, GGAZE_SCROLL_NAVIGATE);
   g_assert_cmpint(settings_get_scroll_behavior(p_s), ==,
                   GGAZE_SCROLL_NAVIGATE);
   settings_set_wrap(p_s, FALSE);
   g_assert_false(settings_get_wrap(p_s));
   settings_set_hide_trashed(p_s, TRUE);
   g_assert_true(settings_get_hide_trashed(p_s));
   settings_set_hide_raw(p_s, FALSE);
   g_assert_false(settings_get_hide_raw(p_s));
   settings_set_slideshow_delay(p_s, 0.25);
   g_assert_cmpfloat(settings_get_slideshow_delay(p_s), ==, 0.25);
   settings_set_thumbnail_size(p_s, 256);
   g_assert_cmpint(settings_get_thumbnail_size(p_s), ==, 256);

   settings_delete(p_s);
}

static void
test_pair_valid(void) {
   g_assert_true(settings_pair_valid("Photos", "/home/p/Photos", TRUE));
   g_assert_true(settings_pair_valid("gimp", "gimp %f", FALSE));
   g_assert_false(settings_pair_valid("", "/x", TRUE));
   g_assert_false(settings_pair_valid("x", "", TRUE));
   g_assert_false(settings_pair_valid(NULL, "/x", TRUE));
   g_assert_false(settings_pair_valid("x", NULL, TRUE));
   /* destinations require an absolute path. */
   g_assert_false(settings_pair_valid("Rel", "relative/path", TRUE));
   g_assert_true(settings_pair_valid("Rel", "relative/path", FALSE));
}

static GPtrArray *
make_pairs(const char *const *c_specs, guint u_n) {
   GPtrArray *p = g_ptr_array_new_with_free_func(settings_pair_free);
   for (guint i = 0; i < u_n; i++) {
      SettingsPair *pr = g_new(SettingsPair, 1);
      pr->c_name       = g_strdup(c_specs[i * 2]);
      pr->c_value      = g_strdup(c_specs[i * 2 + 1]);
      g_ptr_array_add(p, pr);
   }
   return p;
}

static void
check_pairs(const GPtrArray *p, const char *const *c_specs, guint u_n) {
   g_assert_cmpint(p->len, ==, u_n);
   for (guint i = 0; i < u_n; i++) {
      const SettingsPair *pr = g_ptr_array_index((GPtrArray *)p, i);
      g_assert_cmpstr(pr->c_name, ==, c_specs[i * 2]);
      g_assert_cmpstr(pr->c_value, ==, c_specs[i * 2 + 1]);
   }
}

static void
test_destinations_roundtrip(void) {
   Settings  *p_s = new_settings();
   GPtrArray *p   = settings_get_destinations(p_s);
   g_assert_cmpint(p->len, ==, 0);
   g_ptr_array_unref(p);

   const char *const specs[] = {"Photos", "/home/p/Photos", "Out", "/tmp/out"};
   GPtrArray        *in      = make_pairs(specs, 2);
   guint             u_ok    = settings_set_destinations(p_s, in);
   g_assert_cmpuint(u_ok, ==, 2);
   g_ptr_array_unref(in);

   p = settings_get_destinations(p_s);
   check_pairs(p, specs, 2);
   g_ptr_array_unref(p);
   settings_delete(p_s);
}

static void
test_destinations_reject_bad(void) {
   Settings *p_s = new_settings();
   /* mixed valid/invalid: empty name, relative path, empty value. */
   const char *const specs[] = {
      "",      "/home/p/Photos", /* bad: empty name */
      "Rel",   "relative",       /* bad: relative path */
      "Good",  "/tmp/g",         /* good */
      "Empty", "",               /* bad: empty value */
      "Also",  "/var/x",         /* good */
   };
   GPtrArray *in   = make_pairs(specs, 5);
   guint      u_ok = settings_set_destinations(p_s, in);
   g_assert_cmpuint(u_ok, ==, 2);
   g_ptr_array_unref(in);

   GPtrArray *p = settings_get_destinations(p_s);
   g_assert_cmpint(p->len, ==, 2);
   const SettingsPair *a = g_ptr_array_index(p, 0);
   g_assert_cmpstr(a->c_name, ==, "Good");
   const SettingsPair *b = g_ptr_array_index(p, 1);
   g_assert_cmpstr(b->c_name, ==, "Also");
   g_ptr_array_unref(p);
   settings_delete(p_s);
}

static void
test_editors_scripts_presets(void) {
   Settings *p_s = new_settings();
   /* editors/scripts/presets accept arbitrary non-empty commands (no path
    * requirement), and round-trip order. */
   const char *const ed[]  = {"gimp", "gimp %f", "identify", "identify %f"};
   GPtrArray        *in_ed = make_pairs(ed, 2);
   g_assert_cmpuint(settings_set_editors(p_s, in_ed), ==, 2);
   g_ptr_array_unref(in_ed);
   GPtrArray *p_ed = settings_get_editors(p_s);
   check_pairs(p_ed, ed, 2);
   g_ptr_array_unref(p_ed);

   const char *const sc[]  = {"resize", "convert %f -resize 50%% %f.out"};
   GPtrArray        *in_sc = make_pairs(sc, 1);
   g_assert_cmpuint(settings_set_scripts(p_s, in_sc), ==, 1);
   g_ptr_array_unref(in_sc);
   GPtrArray *p_sc = settings_get_scripts(p_s);
   check_pairs(p_sc, sc, 1);
   g_ptr_array_unref(p_sc);

   const char *const pr[]  = {"Warm", "gegl:exposure exposure=0.5"};
   GPtrArray        *in_pr = make_pairs(pr, 1);
   g_assert_cmpuint(settings_set_enhance_presets(p_s, in_pr), ==, 1);
   g_ptr_array_unref(in_pr);
   GPtrArray *p_pr = settings_get_enhance_presets(p_s);
   check_pairs(p_pr, pr, 1);
   g_ptr_array_unref(p_pr);

   /* Clearing: NULL empties the list and returns 0. */
   g_assert_cmpuint(settings_set_editors(p_s, NULL), ==, 0);
   GPtrArray *empty = settings_get_editors(p_s);
   g_assert_cmpint(empty->len, ==, 0);
   g_ptr_array_unref(empty);
   settings_delete(p_s);
}

static void
test_get_gsettings_nonnull(void) {
   Settings *p_s = new_settings();
   g_assert_nonnull(settings_get_gsettings(p_s));
   g_assert_true(G_IS_SETTINGS(settings_get_gsettings(p_s)));
   settings_delete(p_s);
}

int
main(int argc, char **argv) {
   g_test_init(&argc, &argv, NULL);
   g_test_add_func("/settings/scalars_roundtrip", test_scalars_roundtrip);
   g_test_add_func("/settings/pair_valid", test_pair_valid);
   g_test_add_func("/settings/destinations_roundtrip",
                   test_destinations_roundtrip);
   g_test_add_func("/settings/destinations_reject_bad",
                   test_destinations_reject_bad);
   g_test_add_func("/settings/editors_scripts_presets",
                   test_editors_scripts_presets);
   g_test_add_func("/settings/get_gsettings", test_get_gsettings_nonnull);
   return g_test_run();
}