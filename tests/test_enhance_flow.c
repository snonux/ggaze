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
 * Teardown note: windows here are torn down with gtk_window_destroy(), NOT
 * the plain g_object_unref() the older tests in this suite use. GTK4's
 * gtk_window_constructed() hands the caller's initial reference over to the
 * internal toplevel list, and only gtk_window_destroy() takes the window
 * back out of that list -- unreffing alone finalizes it while the list still
 * points at it. That went unnoticed for as long as nothing iterated the
 * list, but the dirty-gate subtests below open a real modal GtkAlertDialog,
 * and presenting a modal grab walks gtk_window_list_toplevels() and refs
 * every entry: with ASan poisoning freed memory it aborts on the stale one.
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

#include "gridview.h"
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

/* Depth-first search for the GtkFlowBox GgazeGrid keeps inside its
 * GtkScrolledWindow (the grid does not expose it). */
static GtkFlowBox *
find_flow_box(GtkWidget *p_w) {
   if (GTK_IS_FLOW_BOX(p_w)) {
      return (GTK_FLOW_BOX(p_w));
   }
   for (GtkWidget *p_c = gtk_widget_get_first_child(p_w); p_c != NULL;
        p_c            = gtk_widget_get_next_sibling(p_c)) {
      GtkFlowBox *p_f = find_flow_box(p_c);
      if (p_f != NULL) {
         return (p_f);
      }
   }
   return (NULL);
}

/* Activate cell i_idx exactly as a double-click / Enter on it would: the
 * flowbox emits "child-activated", gridview.c's _on_child_activated routes
 * the cell's file through the installed select gate. Needs no laid-out
 * geometry (unlike ggaze_grid_move_cursor), so the window never has to be
 * presented -- see test_grid_select_gates_dirty_enhance for why that
 * matters. */
static void
activate_cell(GgazeGrid *p_grid, gint i_idx) {
   GtkFlowBox *p_flow = find_flow_box(GTK_WIDGET(p_grid));
   g_assert_nonnull(p_flow);
   GtkFlowBoxChild *p_child = gtk_flow_box_get_child_at_index(p_flow, i_idx);
   g_assert_nonnull(p_child);
   g_signal_emit_by_name(p_flow, "child-activated", p_child);
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
   gtk_window_destroy(GTK_WINDOW(p_win));
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
   gtk_window_destroy(GTK_WINDOW(p_win));
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
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_free(c_path);
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* Requirement 4 regression (tu0 review round 2, issue 4): the internal
 * hold-compare flag must never get stuck TRUE when the enhance mask is
 * cleared while Space is physically still held down. ggaze_window_set_hold_
 * original() deliberately no-ops when nothing is dirty, so the RELEASE that
 * eventually arrives cannot clear the flag by itself -- every mask-clearing
 * path has to force it off instead (window.c's _enhance_apply_async mask==0
 * branch). Stuck TRUE meant the next Space press was swallowed as "already
 * in that state" and hold-compare silently did nothing for one whole
 * press/release cycle. */
static void
test_hold_flag_not_stuck_after_mask_cleared(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-enhance-stuck-XXXXXX", &p_err);
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
   g_assert_true(viewer_texture(p_win) != p_orig_tex);

   ggaze_window_set_hold_original(p_win, TRUE); /* Space down */
   g_assert_true(viewer_texture(p_win) == p_orig_tex);

   /* Mask cleared WHILE Space is still down -- toggling the last enabled
    * preset back off, the path that does not go through _enhance_discard. */
   fire(p_win, "win.enhance-1");
   drain_main(200);
   g_assert_false(ggaze_window_enhance_is_dirty(p_win));

   ggaze_window_set_hold_original(p_win, FALSE); /* Space up, arrives late:
                                                  * a no-op, nothing dirty */

   /* Fresh press/release cycle on a fresh preview: hold-compare must work on
    * the FIRST press. It did not before the fix -- the stale TRUE flag made
    * this a no-op and left the modified texture on screen. */
   GdkTexture *p_orig2 = viewer_texture(p_win);
   fire(p_win, "win.enhance-1");
   wait_for_texture_change(p_win, p_orig2);
   GdkTexture *p_mod2 = viewer_texture(p_win);
   g_assert_true(p_mod2 != p_orig2);

   ggaze_window_set_hold_original(p_win, TRUE);
   g_assert_true(viewer_texture(p_win) == p_orig2);
   ggaze_window_set_hold_original(p_win, FALSE);
   g_assert_true(viewer_texture(p_win) == p_mod2);

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
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
   gtk_window_destroy(GTK_WINDOW(p_win));
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
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* Requirement 6 (tu0 review round 2, issue 1): grid/thumbnail selection must
 * go through the same Save/Discard/Cancel dirty gate as every other
 * navigation trigger, instead of gridview.c calling
 * navigator_set_current_file() directly and letting nav_changed_cb silently
 * zero the mask before the window ever gets a chance to prompt (repro:
 * enhance an image, switch to grid, click a different thumbnail -> the
 * preview vanished with no prompt). This drives the REAL window-level
 * wiring end to end (window.c's _grid_select_gate, installed on every grid
 * via ggaze_grid_set_select_func) -- see tests/test_grid_select_gate.c for
 * gridview.c's own side of the fix, tested in isolation with no GEGL/dialog
 * involved at all.
 *
 * Triggering the real Save/Discard/Cancel GtkAlertDialog here is a
 * deliberate departure from this suite's usual "never actually dirty when
 * navigating" convention (see test_navigate_when_not_dirty_is_immediate
 * above, and test_delete_safety.c's own documented precedent) -- exercising
 * this exact regression for real requires it, and _SaveCtx's new window ref
 * (round-2 issue 3 fix) removes the dangling-pointer risk that made earlier
 * tests avoid it. This test only checks the SYNCHRONOUS consequence (mask
 * still set, current file unchanged immediately after) and does not attempt
 * to drive the dialog to a button press.
 *
 * Selection is driven by emitting the flowbox's "child-activated" (what a
 * double-click / Enter on a thumbnail does) rather than
 * ggaze_grid_move_cursor, so no toplevel has to be presented: realizing one
 * mid-suite drags in GTK's AT-SPI bridge, whose async registration reply
 * then walks a stale accessible context left by an earlier subtest's
 * finalized window and aborts under ASan -- a GTK-internal problem with
 * nothing to do with this feature. */
static void
test_grid_select_gates_dirty_enhance(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-enhance-grid-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   copy_fixture(c_dir, "rot6.jpg");
   char        *c_p0  = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile       *p_f0  = g_file_new_for_path(c_p0);
   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_f0);
   wait_for_load(p_win);

   GdkTexture *p_orig_tex = viewer_texture(p_win);
   fire(p_win, "win.enhance-1");
   wait_for_texture_change(p_win, p_orig_tex);
   g_assert_true(ggaze_window_enhance_is_dirty(p_win));

   GtkStack  *p_stack  = ggaze_window_get_stack(p_win);
   GtkWidget *p_grid_w = gtk_stack_get_child_by_name(p_stack, "grid");
   g_assert_nonnull(p_grid_w);
   GgazeGrid *p_grid = GGAZE_GRID(p_grid_w);
   g_assert_cmpuint(ggaze_grid_get_count(p_grid), ==, 2); /* a second cell to
                                                           * actually go to */

   activate_cell(p_grid, 1); /* "double-click the other thumbnail" -- one of
                              * gridview.c's 4 gated call sites; silently
                              * discarded the preview pre-fix */
   drain_main(100);

   /* Still dirty, still on the same file: the gate deferred the change
    * behind the (unresolved, not driven) Save/Discard/Cancel prompt instead
    * of letting nav_changed_cb silently discard it. */
   g_assert_true(ggaze_window_enhance_is_dirty(p_win));
   const gchar *c_title = gtk_window_get_title(GTK_WINDOW(p_win));
   g_assert_nonnull(g_strstr_len(c_title, -1, "plain.jpg"));
   /* ... and what is on screen still matches that dirty state: the
    * activation's switch-to-large must not repaint the plain original under
    * a preview the user has not been asked about yet. */
   g_assert_true(viewer_texture(p_win) != p_orig_tex);

   /* Positive control, so the assertions above cannot pass merely because
    * the activation never reached a real target: toggle the preset back off
    * (mask -> 0, nothing dirty any more) and repeat the very same
    * activation -- now it must go straight through. */
   fire(p_win, "win.enhance-1");
   drain_main(200);
   g_assert_false(ggaze_window_enhance_is_dirty(p_win));
   activate_cell(p_grid, 1);
   drain_main(200);
   c_title = gtk_window_get_title(GTK_WINDOW(p_win));
   g_assert_nonnull(g_strstr_len(c_title, -1, "rot6.jpg"));

   g_object_unref(p_f0);
   g_free(c_p0);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* Requirement 6 (tu0 review round 2, issue 2): native window-close (WM "X"
 * button / Alt+F4 / etc., all of which fire GTK's "close-request" signal,
 * same as gtk_window_close()) must gate on the same dirty-preview prompt as
 * win.quit instead of silently discarding an unsaved preview -- pre-fix, no
 * "close-request" handler existed anywhere in this codebase (confirmed via
 * grep), so only the `q` keybinding was ever gated.
 *
 * Emits "close-request" directly (rather than driving a real WM close) and
 * checks its boolean return: TRUE means "stop, do not close" (dirty -- the
 * Save/Discard/Cancel prompt is now pending; not driven further here, see
 * test_grid_select_gates_dirty_enhance's comment above for why); FALSE means
 * "let the default handling proceed" (clean), which -- like a real WM close
 * -- may tear the window down, so the clean case uses its own throwaway
 * window rather than the one used for the dirty assertion. */
static void
test_close_request_gates_dirty_enhance(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-enhance-close-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   char  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);

   {
      GgazeWindow *p_clean = new_window();
      ggaze_window_open(p_clean, p_file);
      wait_for_load(p_clean);
      gboolean b_stop = FALSE;
      g_signal_emit_by_name(p_clean, "close-request", &b_stop);
      g_assert_false(b_stop); /* not dirty: default close proceeds */
      drain_main(100);
      gtk_window_destroy(GTK_WINDOW(p_clean));
   }

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_file);
   wait_for_load(p_win);
   GdkTexture *p_orig_tex = viewer_texture(p_win);
   fire(p_win, "win.enhance-1");
   wait_for_texture_change(p_win, p_orig_tex);
   g_assert_true(ggaze_window_enhance_is_dirty(p_win));

   gboolean b_stop = FALSE;
   g_signal_emit_by_name(p_win, "close-request", &b_stop);
   g_assert_true(b_stop);                               /* close blocked */
   g_assert_true(ggaze_window_enhance_is_dirty(p_win)); /* still dirty */

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_free(c_path);
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
   g_test_add_func("/enhance_flow/hold_flag_not_stuck_after_mask_cleared",
                   test_hold_flag_not_stuck_after_mask_cleared);
   g_test_add_func("/enhance_flow/save_exports_collision_safe_copy",
                   test_save_exports_collision_safe_copy);
   g_test_add_func("/enhance_flow/navigate_when_not_dirty_is_immediate",
                   test_navigate_when_not_dirty_is_immediate);
   g_test_add_func("/enhance_flow/grid_select_gates_dirty_enhance",
                   test_grid_select_gates_dirty_enhance);
   g_test_add_func("/enhance_flow/close_request_gates_dirty_enhance",
                   test_close_request_gates_dirty_enhance);
   return (g_test_run());
}
