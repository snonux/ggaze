/*:*
 * ggaze — GEGL enhance flow integration test (tu0, gated on HAVE_GEGL)
 *
 * Exercises the window-level wiring added in tu0 on top of the already-
 * hardened enhancer_export()/enhancer_export_chain() (ju0/ku0):
 *
 *   - win.enhance-N applies the preset chain off the GTK main thread
 *     (enhancer_apply_chain_async -> a GTask worker) and swaps in a NEW
 *     GdkTexture once it lands (asserted by polling for the texture POINTER
 *     to change, since _enhance_apply_done_cb always builds a fresh one).
 *   - the preview never touches the original file: byte-for-byte identical
 *     before/after apply, toggle-off (discard), and hold-Space compare.
 *   - ggaze_window_enhance_is_dirty() tracks the mask (TRUE once a preset is
 *     active, FALSE again once every preset is toggled back off).
 *   - ggaze_window_set_hold_original() swaps the displayed texture to the
 *     (cached) original and back without touching u_enhance_mask.
 *   - win.enhance-save exports a NEW file next to the original
 *     (<stem>-enhanced[-<n>].<ext>, collision-suffixed like mover.c), never
 *     overwriting the original or a pre-existing same-named export.
 *
 * Driving the real Save/Discard/Cancel GtkAlertDialog is deliberately NOT
 * done here, for the same reason tests/test_delete_safety.c gives for its
 * own confirm dialog: GTK4 exposes no API to inject a response, and leaving
 * one perpetually pending across window teardown is not a scenario worth
 * risking. The dirty-gate's *decision* (dialog vs. immediate proceed) is
 * covered indirectly: every subtest below that calls a navigation action
 * does so only while ggaze_window_enhance_is_dirty() is FALSE, so the
 * non-dialog branch of _maybe_save_then is what actually runs -- the same
 * code path exercised by every pre-existing navigation test in this suite.
 *
 * Needs a display (integration suite; CI uses xvfb). Gated at the meson
 * level (tests/meson.build registers this binary only `if gegl_dep.found()`,
 * mirroring test_enhancer.c) rather than with a C-level #ifdef, so the
 * minimal (GEGL-disabled) lane does not even build it -- it "skips cleanly"
 * by simply not existing as a test target there.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "viewer.h"
#include "window.h"

#include <gdk/gdk.h>
#include <gegl.h>
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <string.h>

/* --- helpers -------------------------------------------------------------
 */

static GgazeWindow *
new_window(void) {
   return (GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL)));
}

static void
drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

static void
copy_fixture(const char *c_dir, const char *c_name) {
   const gchar *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char   *c_src = g_build_filename(c_fx, c_name, NULL);
   char   *c_dst = g_build_filename(c_dir, c_name, NULL);
   GFile  *p_src = g_file_new_for_path(c_src);
   GFile  *p_dst = g_file_new_for_path(c_dst);
   GError *p_err = NULL;
   g_assert_true(g_file_copy(p_src, p_dst, G_FILE_COPY_OVERWRITE, NULL, NULL,
                             NULL, &p_err));
   g_assert_no_error(p_err);
   g_object_unref(p_src);
   g_object_unref(p_dst);
   g_free(c_src);
   g_free(c_dst);
}

static void
cleanup_temp_dir(char *c_dir) {
   GFile           *p_dir = g_file_new_for_path(c_dir);
   GFileEnumerator *p_e   = g_file_enumerate_children(
      p_dir, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (p_e != NULL) {
      GFileInfo *p_info;
      while ((p_info = g_file_enumerator_next_file(p_e, NULL, NULL)) != NULL) {
         GFile *p_child = g_file_get_child(p_dir, g_file_info_get_name(p_info));
         g_file_delete(p_child, NULL, NULL);
         g_object_unref(p_child);
         g_object_unref(p_info);
      }
      g_object_unref(p_e);
   }
   g_file_delete(p_dir, NULL, NULL);
   g_object_unref(p_dir);
   g_free(c_dir);
}

static GdkTexture *
viewer_texture(GgazeWindow *p_win) {
   GtkStack  *p_stack = ggaze_window_get_stack(p_win);
   GtkWidget *p_large = gtk_stack_get_child_by_name(p_stack, "large");
   return (ggaze_viewer_get_texture(GGAZE_VIEWER(p_large)));
}

static void
wait_for_load(GgazeWindow *p_win) {
   for (guint u = 0; u < 3000 && viewer_texture(p_win) == NULL; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(viewer_texture(p_win));
   drain_main(200);
}

static void
fire(GgazeWindow *p_win, const char *c_action) {
   gtk_widget_activate_action(GTK_WIDGET(p_win), c_action, NULL);
}

/* Poll until the viewer's texture pointer differs from p_before (a fresh
 * async enhance apply always builds a brand-new GdkTexture) or a generous
 * timeout elapses. */
static void
wait_for_texture_change(GgazeWindow *p_win, GdkTexture *p_before) {
   for (guint u = 0; u < 5000 && viewer_texture(p_win) == p_before; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   drain_main(50);
}

static char *
load_bytes(const char *c_path, gsize *pu_len) {
   char   *c_data = NULL;
   GError *p_err  = NULL;
   g_assert_true(g_file_get_contents(c_path, &c_data, pu_len, &p_err));
   g_assert_no_error(p_err);
   return (c_data);
}

/* --- subtests ------------------------------------------------------------
 */

/* Requirement 2/3: applying a preset runs off the main thread and swaps in a
 * new texture; requirement 9: the original file's bytes never change. */
static void
test_apply_is_async_and_original_untouched(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-enhance-flow-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   char *c_path = g_build_filename(c_dir, "plain.jpg", NULL);

   gsize u_before_len;
   char *c_before = load_bytes(c_path, &u_before_len);

   GFile       *p_file = g_file_new_for_path(c_path);
   GgazeWindow *p_win  = new_window();
   ggaze_window_open(p_win, p_file);
   wait_for_load(p_win);
   g_assert_false(ggaze_window_enhance_is_dirty(p_win));

   GdkTexture *p_orig_tex = viewer_texture(p_win);
   fire(p_win, "win.enhance-1"); /* Auto-fix */
   /* Not yet applied synchronously: the async worker has not necessarily run
    * a single main-loop iteration yet, so a change is not guaranteed this
    * instant -- but the mask flips immediately (the toggle itself is
    * synchronous; only the GEGL processing is offloaded). */
   g_assert_true(ggaze_window_enhance_is_dirty(p_win));
   wait_for_texture_change(p_win, p_orig_tex);
   g_assert_true(viewer_texture(p_win) != p_orig_tex);
   g_assert_true(ggaze_window_enhance_is_dirty(p_win));

   gsize u_after_len;
   char *c_after = load_bytes(c_path, &u_after_len);
   g_assert_cmpuint(u_after_len, ==, u_before_len);
   g_assert_cmpint(memcmp(c_before, c_after, u_before_len), ==, 0);

   g_free(c_before);
   g_free(c_after);
   g_object_unref(p_file);
   g_object_unref(p_win);
   g_free(c_path);
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* Requirement 5: toggling the same preset off again is a full reset -- the
 * dirty flag clears and the displayed texture reverts to the original. */
static void
test_toggle_off_resets_to_original(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-enhance-reset-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   char        *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile       *p_file = g_file_new_for_path(c_path);
   GgazeWindow *p_win  = new_window();
   ggaze_window_open(p_win, p_file);
   wait_for_load(p_win);

   GdkTexture *p_orig_tex = viewer_texture(p_win);
   fire(p_win, "win.enhance-1");
   wait_for_texture_change(p_win, p_orig_tex);
   g_assert_true(ggaze_window_enhance_is_dirty(p_win));

   GdkTexture *p_enhanced_tex = viewer_texture(p_win);
   fire(p_win, "win.enhance-1"); /* toggle the same preset back off */
   wait_for_texture_change(p_win, p_enhanced_tex);
   g_assert_false(ggaze_window_enhance_is_dirty(p_win));
   /* Back to the (cache-hit, synchronous) original texture. */
   g_assert_true(viewer_texture(p_win) == p_orig_tex);

   g_object_unref(p_file);
   g_object_unref(p_win);
   g_free(c_path);
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* Requirement 4: hold-Space shows the original while "held", then restores
 * the modified preview on "release", without touching the dirty mask. */
static void
test_hold_space_compares_then_restores(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-enhance-hold-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   char        *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile       *p_file = g_file_new_for_path(c_path);
   GgazeWindow *p_win  = new_window();
   ggaze_window_open(p_win, p_file);
   wait_for_load(p_win);

   GdkTexture *p_orig_tex = viewer_texture(p_win);
   fire(p_win, "win.enhance-1");
   wait_for_texture_change(p_win, p_orig_tex);
   GdkTexture *p_enhanced_tex = viewer_texture(p_win);
   g_assert_true(p_enhanced_tex != p_orig_tex);

   ggaze_window_set_hold_original(p_win, TRUE);
   g_assert_true(viewer_texture(p_win) == p_orig_tex);
   g_assert_true(ggaze_window_enhance_is_dirty(p_win)); /* unchanged by hold */

   ggaze_window_set_hold_original(p_win, FALSE);
   g_assert_true(viewer_texture(p_win) == p_enhanced_tex);
   g_assert_true(ggaze_window_enhance_is_dirty(p_win));

   /* Key-repeat guard: re-requesting the same state is a no-op. */
   ggaze_window_set_hold_original(p_win, FALSE);
   g_assert_true(viewer_texture(p_win) == p_enhanced_tex);

   g_object_unref(p_file);
   g_object_unref(p_win);
   g_free(c_path);
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* Requirements 6/9: `s` exports a collision-suffixed copy next to the
 * original; the original is never overwritten, and re-saving does not clobber
 * a pre-existing enhanced copy either. */
static void
test_save_exports_collision_safe_copy(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-enhance-save-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   char        *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile       *p_file = g_file_new_for_path(c_path);
   GgazeWindow *p_win  = new_window();
   ggaze_window_open(p_win, p_file);
   wait_for_load(p_win);

   gsize u_before_len;
   char *c_before = load_bytes(c_path, &u_before_len);

   GdkTexture *p_orig_tex = viewer_texture(p_win);
   fire(p_win, "win.enhance-1");
   wait_for_texture_change(p_win, p_orig_tex);

   char  *c_out1 = g_build_filename(c_dir, "plain-enhanced.jpg", NULL);
   GFile *p_out1 = g_file_new_for_path(c_out1);
   g_assert_false(g_file_query_exists(p_out1, NULL));
   fire(p_win, "win.enhance-save");
   drain_main(300);
   g_assert_true(g_file_query_exists(p_out1, NULL));

   /* Original is still exactly what it was. */
   gsize u_after_len;
   char *c_after = load_bytes(c_path, &u_after_len);
   g_assert_cmpuint(u_after_len, ==, u_before_len);
   g_assert_cmpint(memcmp(c_before, c_after, u_before_len), ==, 0);

   /* Save again (still dirty -- toggling didn't clear the mask) -> the first
    * export must not be clobbered; a "-1" suffixed sibling appears instead
    * (mover.c's collision convention). */
   fire(p_win, "win.enhance-save");
   drain_main(300);
   char  *c_out2 = g_build_filename(c_dir, "plain-enhanced-1.jpg", NULL);
   GFile *p_out2 = g_file_new_for_path(c_out2);
   g_assert_true(g_file_query_exists(p_out2, NULL));

   g_free(c_before);
   g_free(c_after);
   g_object_unref(p_out1);
   g_object_unref(p_out2);
   g_free(c_out1);
   g_free(c_out2);
   g_object_unref(p_file);
   g_object_unref(p_win);
   g_free(c_path);
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* Requirement 7 (partial, see file header): the dirty gate's non-dialog
 * branch is what every OTHER navigation test in this suite already
 * exercises. This subtest instead asserts the flag itself is correctly FALSE
 * before navigating and stays consistent across a plain (non-dirty)
 * win.next, i.e. the common case is not accidentally treated as dirty. */
static void
test_navigate_when_not_dirty_is_immediate(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-enhance-nav-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   copy_fixture(c_dir, "rot6.jpg");
   char        *c_p0  = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile       *p_f0  = g_file_new_for_path(c_p0);
   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_f0);
   wait_for_load(p_win);

   g_assert_false(ggaze_window_enhance_is_dirty(p_win));
   fire(p_win, "win.next"); /* not dirty -> _maybe_save_then proceeds inline */
   const gchar *c_title = gtk_window_get_title(GTK_WINDOW(p_win));
   g_assert_nonnull(g_strstr_len(c_title, -1, "rot6.jpg"));

   g_object_unref(p_f0);
   g_free(c_p0);
   g_object_unref(p_win);
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

int
main(int i_argc, char **c_argv) {
   /* Production always calls gegl_init() at GApplication startup (app.c)
    * well before any window/enhance action exists; this bare test window
    * (built via g_object_new, bypassing GgazeApp) needs the same explicit
    * call test_enhancer.c already makes -- without it, GEGL's operation
    * registry is uninitialized and gegl_node_new_child() aborts, as
    * discovered by this suite's first real run. */
   gegl_init(&i_argc, &c_argv);
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);

   if (!gtk_init_check()) {
      g_test_skip("no display available (run under xvfb)");
      return (g_test_run());
   }

   g_test_add_func("/enhance_flow/apply_is_async_and_original_untouched",
                   test_apply_is_async_and_original_untouched);
   g_test_add_func("/enhance_flow/toggle_off_resets_to_original",
                   test_toggle_off_resets_to_original);
   g_test_add_func("/enhance_flow/hold_space_compares_then_restores",
                   test_hold_space_compares_then_restores);
   g_test_add_func("/enhance_flow/save_exports_collision_safe_copy",
                   test_save_exports_collision_safe_copy);
   g_test_add_func("/enhance_flow/navigate_when_not_dirty_is_immediate",
                   test_navigate_when_not_dirty_is_immediate);
   return (g_test_run());
}
