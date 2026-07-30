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
 * The real Save/Discard/Cancel GtkAlertDialog IS driven here, button by
 * button. The older claim (inherited from tests/test_delete_safety.c) that
 * GTK4 offers no way to answer such a dialog is simply wrong: the window
 * gtk_alert_dialog_choose() puts up is an ordinary GtkWindow in
 * gtk_window_list_toplevels(), so walking it for the GtkButton with the
 * wanted label and emitting its "clicked" drives the async callback
 * to completion (tests/helpers/gtk_helpers.h ggtest_click_dialog_button).
 * That is what lets the subtests below cover the half of the dirty-gate that
 * used to be entirely untested: what happens AFTER the user answers --
 * _proceed_grid_select / _proceed_open / _proceed_move_idx / _proceed_quit
 * actually running, a failed Save refusing to proceed, and Cancel releasing
 * its continuation ctx instead of pinning the whole window.
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
#include "gtk_helpers.h"
#include "settings.h"
#include "viewer.h"
#include "window.h"

#include <gdk/gdk.h>
#include <gegl.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>
#include <unistd.h>

/* --- helpers -------------------------------------------------------------
 */

static GgazeWindow *
new_window(void) {
   return (GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL)));
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

/* Recursive: the trash subtests leave a "./Trash" folder inside the temp dir,
 * and a non-empty directory cannot be g_file_delete()d. */
static void
remove_tree(GFile *p_dir) {
   GFileEnumerator *p_e =
      g_file_enumerate_children(p_dir, "standard::name,standard::type",
                                G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (p_e != NULL) {
      GFileInfo *p_info;
      while ((p_info = g_file_enumerator_next_file(p_e, NULL, NULL)) != NULL) {
         GFile *p_child = g_file_get_child(p_dir, g_file_info_get_name(p_info));
         if (g_file_info_get_file_type(p_info) == G_FILE_TYPE_DIRECTORY) {
            remove_tree(p_child);
         }
         g_file_delete(p_child, NULL, NULL);
         g_object_unref(p_child);
         g_object_unref(p_info);
      }
      g_object_unref(p_e);
   }
}

static void
cleanup_temp_dir(char *c_dir) {
   GFile *p_dir = g_file_new_for_path(c_dir);
   remove_tree(p_dir);
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
   ggtest_drain_main(200);
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
   ggtest_drain_main(50);
}

static char *
load_bytes(const char *c_path, gsize *pu_len) {
   char   *c_data = NULL;
   GError *p_err  = NULL;
   g_assert_true(g_file_get_contents(c_path, &c_data, pu_len, &p_err));
   g_assert_no_error(p_err);
   return (c_data);
}

/* --- dirty-preview fixture ----------------------------------------------- */

/* Every dialog-driving subtest below needs the same setup: a temp folder with
 * two images, a window opened on the first one, and an applied preset so the
 * window is dirty. Bundling it keeps each subtest about its own scenario
 * instead of 20 lines of boilerplate. */
typedef struct {
   char        *c_dir;
   char        *c_path; /* the opened image ("plain.jpg") */
   GFile       *p_file;
   GgazeWindow *p_win;  /* an EXTRA ref is held (see fixture_open) */
   GdkTexture  *p_orig; /* texture before any preset was applied */
   GdkTexture  *p_mod;  /* the enhance preview texture */
   guint        u_ref;  /* p_win's refcount once dirty and settled */
} DirtyFixture;

/* Open a window on a fresh folder holding c_names (NULL-terminated), on the
 * FIRST of them, and leave it clean. Split out of fixture_open so a subtest
 * can do setup that must happen while nothing is dirty yet -- marking files
 * means navigating to them, and navigating with a live preview would raise
 * the very prompt the subtest is about to raise itself.
 *
 * The extra g_object_ref is deliberate: some subtests here let the window
 * actually close (Discard on a close-request runs _proceed_quit), and GTK4
 * hands the caller's initial reference to the internal toplevel list, which
 * drops it on destroy -- without a ref of our own the fixture pointer would
 * dangle the moment the close goes through. */
static void
fixture_open_clean(DirtyFixture *p_fx, const char *c_tmpl,
                   const char *const *c_names) {
   GError *p_err = NULL;
   p_fx->c_dir   = g_dir_make_tmp(c_tmpl, &p_err);
   g_assert_no_error(p_err);
   for (guint u = 0; c_names[u] != NULL; u++) {
      copy_fixture(p_fx->c_dir, c_names[u]);
   }
   p_fx->c_path = g_build_filename(p_fx->c_dir, c_names[0], NULL);
   p_fx->p_file = g_file_new_for_path(p_fx->c_path);
   p_fx->p_win  = new_window();
   g_object_ref(p_fx->p_win);
   ggaze_window_open(p_fx->p_win, p_fx->p_file);
   wait_for_load(p_fx->p_win);
   g_assert_false(ggaze_window_enhance_is_dirty(p_fx->p_win));
}

/* Apply preset 1, so the window ends up dirty with a live preview, and record
 * the reference count every assert_ref_settled() call compares against. */
static void
fixture_make_dirty(DirtyFixture *p_fx) {
   p_fx->p_orig = viewer_texture(p_fx->p_win);
   fire(p_fx->p_win, "win.enhance-1");
   wait_for_texture_change(p_fx->p_win, p_fx->p_orig);
   p_fx->p_mod = viewer_texture(p_fx->p_win);
   g_assert_true(p_fx->p_mod != p_fx->p_orig);
   g_assert_true(ggaze_window_enhance_is_dirty(p_fx->p_win));
   p_fx->u_ref = ((GObject *)p_fx->p_win)->ref_count;
}

/* The common case: a 2-image folder, opened on plain.jpg, already dirty. */
static void
fixture_open(DirtyFixture *p_fx, const char *c_tmpl) {
   static const char *const c_two[] = {"plain.jpg", "rot6.jpg", NULL};
   fixture_open_clean(p_fx, c_tmpl, c_two);
   fixture_make_dirty(p_fx);
}

static void
fixture_teardown(DirtyFixture *p_fx) {
   g_object_unref(p_fx->p_file);
   gtk_window_destroy(GTK_WINDOW(p_fx->p_win)); /* no-op if already closed */
   g_object_unref(p_fx->p_win);                 /* the fixture's own ref */
   g_free(p_fx->c_path);
   ggtest_drain_main(300);
   cleanup_temp_dir(p_fx->c_dir);
}

/* The window's refcount must be back where it was before the prompt: every
 * outstanding continuation ctx owns a window ref, so a ctx that is never
 * freed (Cancel, dismissal, a failed Save) pins the whole GgazeWindow --
 * exactly round 2's finding (b), measured there as 1 -> 3 -> 2. Checking the
 * refcount catches it here even in the non-ASan lanes. */
static void
assert_ref_settled(DirtyFixture *p_fx) {
   g_assert_cmpuint(((GObject *)p_fx->p_win)->ref_count, ==, p_fx->u_ref);
}

/* Answer the outstanding Save/Discard/Cancel prompt. Asserts one really is
 * up (so a subtest can never silently "pass" because no dialog appeared). */
static void
answer_prompt(DirtyFixture *p_fx, const char *c_button) {
   GtkWindow *p_own = GTK_WINDOW(p_fx->p_win);
   g_assert_nonnull(ggtest_wait_for_dialog(p_own, c_button, 2000));
   g_assert_true(ggtest_click_dialog_button(p_own, c_button));
   ggtest_drain_main(400);
   g_assert_cmpuint(ggtest_count_dialogs(p_own, "Cancel"), ==, 0);
}

static const char *
window_title(GgazeWindow *p_win) {
   return (gtk_window_get_title(GTK_WINDOW(p_win)));
}

static void
assert_showing(GgazeWindow *p_win, const char *c_name) {
   g_assert_nonnull(g_strstr_len(window_title(p_win), -1, c_name));
}

/* Mark c_name, then return to the file the fixture opened on. Must run while
 * the window is still CLEAN (between fixture_open_clean and
 * fixture_make_dirty): win.mark acts on navigator.current in the large view,
 * so marking means navigating there, and navigating with a live preview would
 * raise the very prompt the caller is about to raise deliberately. */
static void
mark_file_and_return(DirtyFixture *p_fx, const char *c_name) {
   for (guint u = 0; u < 8; u++) {
      if (g_strstr_len(window_title(p_fx->p_win), -1, c_name) != NULL) {
         break;
      }
      ggaze_window_next(p_fx->p_win);
      ggtest_drain_main(120);
   }
   assert_showing(p_fx->p_win, c_name);
   fire(p_fx->p_win, "win.mark");
   ggtest_drain_main(120);
   ggaze_window_first(p_fx->p_win);
   ggtest_drain_main(150);
}

/* Assert the header's "· N marked" suffix, i.e. the navigator's LIVE mark
 * count -- the state the delete path must NOT consult once a prompt is up. */
static void
assert_marked_count(GgazeWindow *p_win, guint u_n) {
   if (u_n == 0) {
      g_assert_null(g_strstr_len(window_title(p_win), -1, "marked"));
      return;
   }
   char *c_want = g_strdup_printf("%u marked", u_n);
   g_assert_nonnull(g_strstr_len(window_title(p_win), -1, c_want));
   g_free(c_want);
}

/* Delete c_name from the fixture folder the way an external process would,
 * behind whatever modal prompt is currently up, and wait for the folder's
 * GFileMonitor (250 ms debounce, navigator.c) to notice and _relist().
 *
 * That relist is the mechanism the capture rules exist for: GTK4 modality is
 * INPUT-only, the monitor is a plain GSource, and _relist() prunes marks
 * whose file has left the listing. */
static void
remove_behind_the_prompt(DirtyFixture *p_fx, const char *c_name) {
   char *c_path = g_build_filename(p_fx->c_dir, c_name, NULL);
   g_assert_cmpint(g_unlink(c_path), ==, 0);
   g_free(c_path);
   ggtest_drain_main(1500);
}

/* Switch to the grid and double-click the OTHER thumbnail, i.e. the exact
 * repro the select gate exists for. Returns once the click has been
 * delivered (the prompt, if any, is then outstanding). */
static void
activate_other_cell(DirtyFixture *p_fx) {
   GtkStack  *p_stack  = ggaze_window_get_stack(p_fx->p_win);
   GtkWidget *p_grid_w = gtk_stack_get_child_by_name(p_stack, "grid");
   g_assert_nonnull(p_grid_w);
   GgazeGrid *p_grid = GGAZE_GRID(p_grid_w);
   g_assert_cmpuint(ggaze_grid_get_count(p_grid), ==, 2);
   ggtest_activate_cell(p_grid, 1);
   ggtest_drain_main(100);
}

/* --- move-destination helpers (only the move subtest needs these) --------- */

/* Configure exactly one move destination (name, absolute path). The window
 * reads the list at construction, so this must run BEFORE fixture_open. */
static void
set_one_destination(const char *c_name, const char *c_path) {
   GPtrArray    *p_pairs = g_ptr_array_new_with_free_func(settings_pair_free);
   SettingsPair *p_pair  = g_new(SettingsPair, 1);
   p_pair->c_name        = g_strdup(c_name);
   p_pair->c_value       = g_strdup(c_path);
   g_ptr_array_add(p_pairs, p_pair);
   Settings *p_s = settings_new();
   g_assert_nonnull(p_s);
   g_assert_cmpint(settings_set_destinations(p_s, p_pairs), ==, 1);
   settings_delete(p_s);
   g_ptr_array_unref(p_pairs);
}

/* Do not leak destinations into the other subtests (shared memory backend). */
static void
reset_destinations(void) {
   Settings *p_s = settings_new();
   g_assert_nonnull(p_s);
   g_settings_reset(settings_get_gsettings(p_s), "destinations");
   settings_delete(p_s);
}

static gboolean
file_exists_in(const char *c_dir, const char *c_name) {
   char    *c_path = g_build_filename(c_dir, c_name, NULL);
   gboolean b_hit  = g_file_test(c_path, G_FILE_TEST_EXISTS);
   g_free(c_path);
   return (b_hit);
}

/* Open the `m` popover and click its single destination row -- the real UI
 * entry point into _move_go, which is where the dirty gate sits (the public
 * ggaze_window_move_index is deliberately ungated, see window.h). Rows are
 * labelled "<hotkey>  <name>" (window.c's popup row builder). */
static void
click_move_row(DirtyFixture *p_fx) {
   fire(p_fx->p_win, "win.move");
   ggtest_drain_main(150);
   GtkWidget *p_row =
      ggtest_find_button(GTK_WIDGET(p_fx->p_win), "1  dest one");
   g_assert_nonnull(p_row);
   ggtest_click_button(p_row);
   ggtest_drain_main(150);
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
   ggtest_drain_main(300);
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
   ggtest_drain_main(300);
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
   ggtest_drain_main(300);
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
   ggtest_drain_main(200);
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
   ggtest_drain_main(300);
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
   ggtest_drain_main(300);
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
   ggtest_drain_main(300);
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
   ggtest_drain_main(300);
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
   ggtest_drain_main(300);
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
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-grid-XXXXXX");

   activate_other_cell(&fx); /* one of gridview.c's 4 gated call sites; this
                              * silently discarded the preview pre-fix */

   /* Still dirty, still on the same file: the gate deferred the change behind
    * the Save/Discard/Cancel prompt instead of letting nav_changed_cb
    * silently discard it -- and what is on screen still matches that dirty
    * state, i.e. the activation's switch-to-large did not repaint the plain
    * original under a preview the user has not been asked about yet. */
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   assert_showing(fx.p_win, "plain.jpg");
   g_assert_true(viewer_texture(fx.p_win) != fx.p_orig);
   answer_prompt(&fx, "Cancel"); /* leave no dialog behind for later subtests */

   /* Positive control, so the assertions above cannot pass merely because the
    * activation never reached a real target: toggle the preset back off (mask
    * -> 0, nothing dirty any more) and repeat the very same activation -- now
    * it must go straight through, with no prompt at all. */
   fire(fx.p_win, "win.enhance-1");
   ggtest_drain_main(200);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   activate_other_cell(&fx);
   ggtest_drain_main(200);
   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    0);
   assert_showing(fx.p_win, "rot6.jpg");

   fixture_teardown(&fx);
}

/* Requirement 6 (tu0 review round 2, issue 2): native window-close (WM "X"
 * button / Alt+F4 / etc., all of which fire GTK's "close-request" signal,
 * same as gtk_window_close()) must gate on the same dirty-preview prompt as
 * win.quit instead of silently discarding an unsaved preview -- pre-fix, no
 * "close-request" handler existed anywhere in this codebase (confirmed via
 * grep), so only the `q` keybinding was ever gated.
 *
 * The DIRTY half is the regression assertion: without the handler the signal
 * returns FALSE and no prompt appears, so both assertions fail.
 *
 * The CLEAN half is a control, not a test, and is labelled as one: GtkWindow's
 * own default close-request returns FALSE, so `g_assert_false(b_stop)` passes
 * identically with the handler deleted (round 2 called this out). It earns
 * its place by guarding the opposite mistake -- a future over-eager gate that
 * blocks or prompts on a clean window -- which is why it also asserts that no
 * dialog appeared. */
static void
test_close_request_gates_dirty_enhance(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-enhance-close-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   char  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);

   { /* control: a clean window is never blocked and never prompts */
      GgazeWindow *p_clean = new_window();
      ggaze_window_open(p_clean, p_file);
      wait_for_load(p_clean);
      gboolean b_stop = FALSE;
      g_signal_emit_by_name(p_clean, "close-request", &b_stop);
      g_assert_false(b_stop);
      ggtest_drain_main(100);
      g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(p_clean), "Cancel"), ==,
                       0);
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
   g_assert_nonnull(ggtest_wait_for_dialog(GTK_WINDOW(p_win), "Cancel", 2000));

   /* Answer it, so nothing is left pending across teardown. */
   g_assert_true(ggtest_click_dialog_button(GTK_WINDOW(p_win), "Cancel"));
   ggtest_drain_main(300);

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_free(c_path);
   ggtest_drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* --- driving the prompt to a decision ------------------------------------
 *
 * Everything below answers the real GtkAlertDialog (see the file header), so
 * these cover the half of the dirty gate that used to be untested: what
 * happens once the user picks a button.
 */

/* Cancel: the deferred change must NOT happen, the preview must survive --
 * and the continuation ctx must be released anyway. Pre-fix (round 2 finding
 * b) _maybe_save_then only ever freed the ctx by running the continuation, so
 * Cancel leaked a _GridSelectCtx holding an owned window ref, pinning the
 * whole GgazeWindow forever (measured 1 -> 3 -> 2). */
static void
test_cancel_keeps_preview_and_frees_ctx(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-cancel-XXXXXX");

   activate_other_cell(&fx);
   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    1);
   answer_prompt(&fx, "Cancel");

   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win)); /* preview kept */
   assert_showing(fx.p_win, "plain.jpg");                  /* did not move */
   g_assert_true(viewer_texture(fx.p_win) == fx.p_mod);    /* still shown */
   assert_ref_settled(&fx);

   fixture_teardown(&fx);
}

/* Discard: the deferred change is applied once the user answers. This is the
 * first test in the suite that runs _proceed_grid_select at all -- until now
 * every assertion stopped at "the change was deferred". */
static void
test_discard_applies_deferred_grid_select(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-discard-XXXXXX");

   activate_other_cell(&fx);
   answer_prompt(&fx, "Discard");

   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win)); /* mask cleared */
   assert_showing(fx.p_win, "rot6.jpg"); /* the deferred select happened */

   fixture_teardown(&fx);
}

/* Save: exports the enhanced copy AND then applies the deferred change. */
static void
test_save_exports_then_applies_deferred_select(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-savego-XXXXXX");

   gsize u_len_before;
   char *c_before = load_bytes(fx.c_path, &u_len_before);

   activate_other_cell(&fx);
   answer_prompt(&fx, "Save");

   char *c_out = g_build_filename(fx.c_dir, "plain-enhanced.jpg", NULL);
   g_assert_true(g_file_test(c_out, G_FILE_TEST_EXISTS));
   g_free(c_out);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   assert_showing(fx.p_win, "rot6.jpg");

   /* The original is still byte-identical (requirement 9). */
   gsize u_len_after;
   char *c_after = load_bytes(fx.c_path, &u_len_after);
   g_assert_cmpuint(u_len_after, ==, u_len_before);
   g_assert_cmpint(memcmp(c_before, c_after, u_len_before), ==, 0);
   g_free(c_before);
   g_free(c_after);

   fixture_teardown(&fx);
}

/* A FAILED Save must not be silently downgraded to Discard (round 2 finding
 * a): _save_dialog_cb used to drop _enhance_do_save()'s return value, so a
 * read-only folder / full disk lost the enhancement AND navigated away,
 * painting the error message onto a window nobody would look at again.
 *
 * The failure is provoked by making the folder read-only, so the export
 * cannot be created; enhancer.c's _save_buffer stats the destination
 * afterwards and reports failure. */
static void
test_failed_save_keeps_preview_and_aborts(void) {
   if (geteuid() == 0) {
      /* root ignores the folder's write bit, so the export SUCCEEDS and the
       * "nothing was written" assertion below fails with a confusing error
       * instead of the intended skip -- which made the whole suite unrunnable
       * in a root container (round 3, finding o). */
      g_test_skip("running as root: a read-only folder cannot be provoked");
      return;
   }
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-failsave-XXXXXX");

   g_assert_cmpint(g_chmod(fx.c_dir, 0500), ==, 0); /* r-x: no new files */
   activate_other_cell(&fx);
   answer_prompt(&fx, "Save");
   g_assert_cmpint(g_chmod(fx.c_dir, 0700), ==, 0); /* restore for cleanup */

   /* Nothing was written ... */
   char *c_out = g_build_filename(fx.c_dir, "plain-enhanced.jpg", NULL);
   g_assert_false(g_file_test(c_out, G_FILE_TEST_EXISTS));
   g_free(c_out);
   /* ... so the preview is still there to retry with ... */
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_mod);
   /* ... and the continuation was aborted, not run. */
   assert_showing(fx.p_win, "plain.jpg");
   assert_ref_settled(&fx);

   fixture_teardown(&fx);
}

/* The gate's already-current early return: re-activating the cell that IS
 * the current file changes nothing, so it must not prompt at all (an
 * unnecessary Save/Discard/Cancel dialog for a no-op would be its own bug).
 */
static void
test_activating_current_cell_does_not_prompt(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-nocell-XXXXXX");

   GtkStack  *p_stack = ggaze_window_get_stack(fx.p_win);
   GgazeGrid *p_grid = GGAZE_GRID(gtk_stack_get_child_by_name(p_stack, "grid"));
   ggtest_activate_cell(p_grid, 0); /* plain.jpg == the current file */
   ggtest_drain_main(200);

   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    0);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   assert_showing(fx.p_win, "plain.jpg");
   g_assert_true(viewer_texture(fx.p_win) == fx.p_mod);
   assert_ref_settled(&fx);

   fixture_teardown(&fx);
}

/* Round 2 finding (c): `t` `t` (large -> grid -> large) went through
 * _action_toggle_view, which -- unlike _on_grid_activate -- never got the
 * preview-repaint fix, so the viewer showed the plain original while the
 * window was still dirty and `s` would still have exported the enhanced
 * version. No dialog is involved: the highlighted cell already IS the current
 * file, so the gate short-circuits. Fixed centrally in _show_texture, which
 * is why this passes for the async load path too. */
static void
test_toggle_view_keeps_dirty_preview(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-toggle-XXXXXX");

   fire(fx.p_win, "win.toggle-view"); /* -> grid */
   ggtest_drain_main(100);
   fire(fx.p_win, "win.toggle-view"); /* -> large, syncs current */
   ggtest_drain_main(300);

   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    0);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) != fx.p_orig);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_mod);

   fixture_teardown(&fx);
}

/* Round 2 finding (d): repeated native closes must not STACK dialogs. Alt+F4
 * is not an input event, so the modal grab does not swallow it and every
 * emission reached _maybe_save_then; three of them used to open three live
 * dialogs, and answering Save on each wrote three separate "-enhanced" copies
 * and ran the continuation three times. */
static void
test_repeated_close_request_does_not_stack_dialogs(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-restack-XXXXXX");

   for (guint u = 0; u < 3; u++) {
      gboolean b_stop = FALSE;
      g_signal_emit_by_name(fx.p_win, "close-request", &b_stop);
      g_assert_true(b_stop); /* every one of them blocks the close */
      ggtest_drain_main(100);
   }
   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    1);

   answer_prompt(&fx, "Cancel");
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   assert_ref_settled(&fx);

   fixture_teardown(&fx);
}

/* The close-request gate must actually let go once answered: Discard clears
 * the mask and _proceed_quit's gtk_window_close() re-emits "close-request",
 * which the now-clean handler propagates -- so the window really closes
 * instead of staying blocked forever. That second half was untested (round
 * 2's test-quality section) and is the difference between "we prompt" and "we
 * prompt and then honour the answer".
 *
 * Two GTK4 details shape this subtest. gtk_window_close() is a no-op on an
 * unrealized window, so this is the one place here that presents its
 * toplevel. And gtk_window_destroy() neither emits a watchable signal nor
 * finalizes a window the test still references -- it hides it, unrealizes it
 * and drops it from the toplevel list -- so "closed" is asked as "no longer a
 * live toplevel" (ggtest_is_open_toplevel). */
static void
test_close_request_discard_closes_window(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-closego-XXXXXX");
   gtk_window_present(GTK_WINDOW(fx.p_win));
   ggtest_drain_main(300);
   g_assert_true(ggtest_is_open_toplevel(GTK_WINDOW(fx.p_win)));

   gboolean b_stop = FALSE;
   g_signal_emit_by_name(fx.p_win, "close-request", &b_stop);
   g_assert_true(b_stop); /* blocked while the prompt is up */
   g_assert_true(ggtest_is_open_toplevel(GTK_WINDOW(fx.p_win)));

   answer_prompt(&fx, "Discard");

   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_false(ggtest_is_open_toplevel(GTK_WINDOW(fx.p_win)));
   g_assert_false(gtk_widget_get_realized(GTK_WIDGET(fx.p_win)));

   fixture_teardown(&fx); /* the fixture ref is what keeps this valid */
}

/* ggaze_window_open() while dirty (File->Open, drag-and-drop, single-instance
 * re-activation): Cancel must abort the open and free the _OpenCtx; a second
 * attempt answered with Discard must actually open the new folder, running
 * _proceed_open -- which no test had ever executed. */
static void
test_open_while_dirty_cancel_then_discard(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-open-XXXXXX");

   GError *p_err   = NULL;
   char   *c_other = g_dir_make_tmp("ggaze-enhance-open2-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_other, "small.png");
   char  *c_target = g_build_filename(c_other, "small.png", NULL);
   GFile *p_target = g_file_new_for_path(c_target);

   ggaze_window_open(fx.p_win, p_target);
   ggtest_drain_main(100);
   answer_prompt(&fx, "Cancel");
   assert_showing(fx.p_win, "plain.jpg"); /* open aborted */
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   assert_ref_settled(&fx); /* _OpenCtx released despite Cancel */

   ggaze_window_open(fx.p_win, p_target);
   ggtest_drain_main(100);
   answer_prompt(&fx, "Discard");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   assert_showing(fx.p_win, "small.png"); /* _proceed_open ran */

   g_object_unref(p_target);
   g_free(c_target);
   fixture_teardown(&fx);
   cleanup_temp_dir(c_other);
}

/* `m` -> destination row while dirty: same two halves for _MoveIdxCtx and
 * _proceed_move_idx (also never executed before). The popover row is an
 * ordinary GtkButton, so it is activated the same way the dialog buttons
 * are. */
static void
test_move_while_dirty_cancel_then_discard(void) {
   GError *p_err  = NULL;
   char   *c_dest = g_dir_make_tmp("ggaze-enhance-movedest-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   set_one_destination("dest one", c_dest);

   DirtyFixture fx; /* the window reads destinations at construction */
   fixture_open(&fx, "ggaze-enhance-move-XXXXXX");

   click_move_row(&fx);
   answer_prompt(&fx, "Cancel");
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_false(file_exists_in(c_dest, "plain.jpg")); /* nothing moved */
   assert_ref_settled(&fx);

   click_move_row(&fx);
   answer_prompt(&fx, "Discard");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(file_exists_in(c_dest, "plain.jpg")); /* _proceed_move_idx */

   fixture_teardown(&fx);
   cleanup_temp_dir(c_dest);
   reset_destinations();
}

/* --- round 3: what happens BEHIND an outstanding prompt ------------------
 *
 * GTK4 modality is INPUT-only. The slideshow timer is a plain g_timeout_add
 * and the folder's GFileMonitor is a plain GSource, so both keep firing while
 * a modal GtkAlertDialog is on screen and can move navigator.current (and
 * therefore clear the enhance mask) out from under a prompt that is still
 * worded for the old image. So can a single-instance `ggaze other.jpg`, which
 * arrives over D-Bus rather than as an input event.
 *
 * The subtests below drive that with ggaze_window_next() and
 * ggaze_window_open() -- exactly what _slideshow_tick and ggaze_app_open call,
 * and, like them, not input, so the modal grab does not stop them.
 */

/* Round 3, finding (h) -- the data-loss regression. The deferred continuation
 * used to re-read navigator_get_current() when the user finally answered, so
 * Discard binned a file the user never selected: prompt raised for plain.jpg,
 * current moved on to rot6.jpg behind it, click Discard -> rot6.jpg went to
 * ./Trash and plain.jpg survived. window.c now captures the victim at
 * key-press time (_FileCtx), the same discipline _DeleteCtx already had. */
static void
test_prompt_acts_on_the_file_it_was_raised_for(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-captured-XXXXXX");
   char *c_other = g_build_filename(fx.c_dir, "rot6.jpg", NULL);

   fire(fx.p_win, "win.trash"); /* `d`, pressed on plain.jpg */
   ggtest_drain_main(150);
   g_assert_nonnull(
      ggtest_wait_for_dialog(GTK_WINDOW(fx.p_win), "Cancel", 2000));

   ggaze_window_next(fx.p_win); /* what the slideshow tick does */
   ggtest_drain_main(300);
   assert_showing(fx.p_win, "rot6.jpg");                    /* really moved */
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win)); /* mask cleared */

   answer_prompt(&fx, "Discard");

   g_assert_false(g_file_test(fx.c_path, G_FILE_TEST_EXISTS)); /* binned ... */
   g_assert_true(g_file_test(c_other, G_FILE_TEST_EXISTS));    /* ... only it */

   g_free(c_other);
   fixture_teardown(&fx);
}

/* Round 3, finding (j): the one-prompt guard used to be checked AFTER the
 * "nothing is dirty" fast path, so the moment something cleared the mask under
 * a live dialog a new request ran its continuation immediately -- two actions
 * out of one visible prompt. The guard is now checked first, so the second
 * request is parked instead. */
static void
test_request_behind_stale_prompt_is_not_run(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-stale-XXXXXX");
   GError *p_err   = NULL;
   char   *c_other = g_dir_make_tmp("ggaze-enhance-stale2-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_other, "small.png");
   char  *c_target = g_build_filename(c_other, "small.png", NULL);
   GFile *p_target = g_file_new_for_path(c_target);

   fire(fx.p_win, "win.trash"); /* prompt, raised for plain.jpg */
   ggtest_drain_main(150);
   g_assert_nonnull(
      ggtest_wait_for_dialog(GTK_WINDOW(fx.p_win), "Cancel", 2000));
   ggaze_window_next(fx.p_win); /* clears the mask under the live dialog */
   ggtest_drain_main(200);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));

   ggaze_window_open(fx.p_win, p_target); /* the second, different request */
   ggtest_drain_main(300);
   assert_showing(fx.p_win, "rot6.jpg"); /* did NOT run behind the dialog */
   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    1); /* and did not stack a second one either */

   answer_prompt(&fx, "Cancel"); /* Cancel: neither action happens */
   g_assert_true(g_file_test(fx.c_path, G_FILE_TEST_EXISTS));
   assert_showing(fx.p_win, "rot6.jpg");

   g_object_unref(p_target);
   g_free(c_target);
   fixture_teardown(&fx);
   cleanup_temp_dir(c_other);
}

/* Round 3, finding (k): answering Save on a prompt whose preview has since
 * vanished must not be a silent no-op that ALSO cancels the user's action.
 * _enhance_do_save returns FALSE both for "nothing to save" and "the export
 * failed", and _save_dialog_cb used to treat both as a failure -- so with the
 * mask cleared under the dialog, clicking Save wrote nothing, showed nothing,
 * and aborted the trash the user had asked for. */
static void
test_save_with_no_preview_left_reports_and_proceeds(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-nosave-XXXXXX");

   fire(fx.p_win, "win.trash"); /* `d`, pressed on plain.jpg */
   ggtest_drain_main(150);
   g_assert_nonnull(
      ggtest_wait_for_dialog(GTK_WINDOW(fx.p_win), "Cancel", 2000));
   ggaze_window_next(fx.p_win); /* clears the mask under the live dialog */
   ggtest_drain_main(200);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));

   answer_prompt(&fx, "Save");

   char *c_out = g_build_filename(fx.c_dir, "plain-enhanced.jpg", NULL);
   g_assert_false(
      g_file_test(c_out, G_FILE_TEST_EXISTS)); /* nothing to write */
   g_free(c_out);
   g_assert_false(g_file_test(fx.c_path, G_FILE_TEST_EXISTS)); /* still
                                                                * trashed it */
   g_assert_cmpstr(
      gtk_label_get_text(GTK_LABEL(ggaze_window_get_info_label(fx.p_win))), ==,
      "Nothing to save \u2014 the preview is gone"); /* ... and said why */

   fixture_teardown(&fx);
}

/* Round 3, finding (i), the proceed half: a second, DIFFERENT request while
 * the prompt is up used to be dropped on the floor with no status, no
 * re-prompt and no re-queue -- `ggaze other.jpg` against a live instance
 * exited successfully having done nothing at all, not even after the prompt
 * was answered. It is now parked (one slot, newest wins) and retried through
 * the same gate once the prompt resolves in favour of proceeding. */
static void
test_second_request_is_queued_then_retried(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-queue-XXXXXX");
   GError *p_err   = NULL;
   char   *c_other = g_dir_make_tmp("ggaze-enhance-queue2-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_other, "small.png");
   char  *c_target = g_build_filename(c_other, "small.png", NULL);
   GFile *p_target = g_file_new_for_path(c_target);

   activate_other_cell(&fx);              /* request 1: grid select rot6.jpg */
   ggaze_window_open(fx.p_win, p_target); /* request 2: D-Bus-style open */
   ggtest_drain_main(200);
   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    1);
   assert_showing(fx.p_win, "plain.jpg"); /* neither has run yet */

   answer_prompt(&fx, "Discard");
   ggtest_drain_main(400);
   /* Request 1 ran (the deferred select), then request 2 was retried through
    * the gate and ran too -- so the window ends up on the queued file. */
   assert_showing(fx.p_win, "small.png");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));

   g_object_unref(p_target);
   g_free(c_target);
   fixture_teardown(&fx);
   cleanup_temp_dir(c_other);
}

/* Round 3, finding (i), the drop half. Cancel means "stay on this image, keep
 * the preview", so running the parked request anyway would do exactly what the
 * user just refused: it is dropped instead -- but visibly, via the status
 * label, not silently. This is also the only test that exercises the drop path
 * with fn_free_data != NULL: the parked _OpenCtx holds an owned window ref, so
 * assert_ref_settled is what proves it was actually released. */
static void
test_queued_request_is_dropped_on_cancel(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-drop-XXXXXX");
   GError *p_err   = NULL;
   char   *c_other = g_dir_make_tmp("ggaze-enhance-drop2-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_other, "small.png");
   char  *c_target = g_build_filename(c_other, "small.png", NULL);
   GFile *p_target = g_file_new_for_path(c_target);

   activate_other_cell(&fx);
   ggaze_window_open(fx.p_win, p_target);
   ggtest_drain_main(200);

   answer_prompt(&fx, "Cancel");
   ggtest_drain_main(200);
   assert_showing(fx.p_win, "plain.jpg"); /* neither request happened */
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    0); /* dropped, not re-prompted */
   g_assert_cmpstr(
      gtk_label_get_text(GTK_LABEL(ggaze_window_get_info_label(fx.p_win))), ==,
      "Queued request dropped"); /* and the user is told */
   assert_ref_settled(&fx);

   g_object_unref(p_target);
   g_free(c_target);
   fixture_teardown(&fx);
   cleanup_temp_dir(c_other);
}

/* Save on close-request, the success half: the export lands AND the window
 * really closes. Only the Discard half of this was covered (round 3's
 * "still untested" list). See test_close_request_discard_closes_window for
 * why the window has to be presented and why "closed" is asked as "no longer
 * a live toplevel". */
static void
test_close_request_save_closes_window(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-closesave-XXXXXX");
   gtk_window_present(GTK_WINDOW(fx.p_win));
   ggtest_drain_main(300);

   gboolean b_stop = FALSE;
   g_signal_emit_by_name(fx.p_win, "close-request", &b_stop);
   g_assert_true(b_stop);
   answer_prompt(&fx, "Save");

   char *c_out = g_build_filename(fx.c_dir, "plain-enhanced.jpg", NULL);
   g_assert_true(g_file_test(c_out, G_FILE_TEST_EXISTS));
   g_free(c_out);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_false(ggtest_is_open_toplevel(GTK_WINDOW(fx.p_win)));

   fixture_teardown(&fx);
}

/* Save on close-request, the failure half: a failed export must leave the
 * window OPEN, still dirty, and still blocked -- i.e. a further close-request
 * is stopped again and raises a fresh prompt rather than letting the
 * enhancement escape unnoticed. */
static void
test_close_request_failed_save_keeps_window(void) {
   if (geteuid() == 0) {
      g_test_skip("running as root: a read-only folder cannot be provoked");
      return;
   }
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-closefail-XXXXXX");
   gtk_window_present(GTK_WINDOW(fx.p_win));
   ggtest_drain_main(300);
   g_assert_cmpint(g_chmod(fx.c_dir, 0500), ==, 0); /* r-x: no new files */

   gboolean b_stop = FALSE;
   g_signal_emit_by_name(fx.p_win, "close-request", &b_stop);
   g_assert_true(b_stop);
   answer_prompt(&fx, "Save");
   g_assert_cmpint(g_chmod(fx.c_dir, 0700), ==, 0); /* restore for cleanup */

   g_assert_true(ggtest_is_open_toplevel(GTK_WINDOW(fx.p_win)));
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_mod);

   b_stop = FALSE; /* still blocked, and re-asks rather than going quiet */
   g_signal_emit_by_name(fx.p_win, "close-request", &b_stop);
   g_assert_true(b_stop);
   answer_prompt(&fx, "Cancel");
   g_assert_true(ggtest_is_open_toplevel(GTK_WINDOW(fx.p_win)));

   fixture_teardown(&fx);
}

/* The cache-MISS leg of the enhance-preview override. Round 2 claimed this was
 * untestable; it is not -- ggaze_window_clear_texture_cache (window.h, an
 * explicit test hook) empties the LRU, so the next _load_current has to take
 * the async GTask path, where _load_finish_cb used to repaint the plain
 * original over an unanswered preview. The JPEG backend also emits a
 * phase-1 low-res partial for every load, so _on_progress_main -- the other
 * changed-but-untested leg -- runs here too; both reach the viewer only
 * through _show_texture, which is where the override lives. */
static void
test_cache_miss_reload_keeps_dirty_preview(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-miss-XXXXXX");

   ggaze_window_clear_texture_cache(fx.p_win);
   fire(fx.p_win, "win.toggle-view"); /* -> grid */
   ggtest_drain_main(100);
   fire(fx.p_win, "win.toggle-view"); /* -> large; _load_current MISSES */
   ggtest_drain_main(700);

   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    0);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_mod);

   /* Proof the async load really landed (rather than the assertion above
    * passing because nothing ever repainted): hold-Space reads the ORIGINAL
    * straight out of the texture cache, which was emptied above, so a
    * non-NULL, brand-new texture there can only have come from the reload.
    * This is also _preview_override's hold-original short-circuit under the
    * miss path. */
   ggaze_window_set_hold_original(fx.p_win, TRUE);
   GdkTexture *p_reloaded = viewer_texture(fx.p_win);
   g_assert_nonnull(p_reloaded);
   g_assert_true(p_reloaded != fx.p_mod);
   g_assert_true(p_reloaded != fx.p_orig);
   ggaze_window_set_hold_original(fx.p_win, FALSE);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_mod);

   fixture_teardown(&fx);
}

/* --- round 4: the MARKED delete legs behind the prompt --------------------
 *
 * `D` deletes PERMANENTLY -- no trash, no undo -- so it is the one action
 * where re-deriving anything at answer time is unrecoverable. Round 3
 * captured only navigator.current and left the marks-vs-current DECISION to
 * be re-made in the continuation, on the grounds that "nothing but user input
 * can change the mark set". navigator.c's _relist() prunes marks whose file
 * left the listing, and the folder GFileMonitor driving it keeps firing
 * behind the input-only modal grab, so that was simply untrue (round 4,
 * finding p). The three subtests below pin the whole leg down: the control
 * (nothing external happens), the 1-mark-pruned data-loss repro, and the
 * 3-marks-pruned-to-1 confirm-dialog repro. */

/* Control: `D` with exactly one file marked deletes THAT file when the prompt
 * is answered -- not the current one, and not nothing at all. Without this,
 * the repro below could pass simply because delete had stopped working. */
static void
test_delete_behind_prompt_deletes_the_marked_file(void) {
   DirtyFixture             fx;
   static const char *const c_two[] = {"plain.jpg", "rot6.jpg", NULL};
   fixture_open_clean(&fx, "ggaze-enhance-delctl-XXXXXX", c_two);
   mark_file_and_return(&fx, "rot6.jpg");
   fixture_make_dirty(&fx);
   assert_showing(fx.p_win, "plain.jpg");
   assert_marked_count(fx.p_win, 1);

   fire(fx.p_win, "win.delete"); /* `D`: delete the MARKED set */
   ggtest_drain_main(150);
   ggtest_drain_main(1500); /* same wait as the repro, nothing removed */
   answer_prompt(&fx, "Discard");
   ggtest_drain_main(200);

   g_assert_false(file_exists_in(fx.c_dir, "rot6.jpg")); /* the marked one */
   g_assert_true(file_exists_in(fx.c_dir, "plain.jpg")); /* and only it */
   assert_ref_settled(&fx);

   fixture_teardown(&fx);
}

/* The finding (p) repro. One file marked, `D` pressed, then an external
 * process removes that file while the prompt is up: the monitor relists, the
 * dangling mark is pruned, and the live count goes 1 -> 0. Re-reading it in
 * the continuation therefore took the "no marks" leg and PERMANENTLY deleted
 * plain.jpg -- never marked, never chosen, and the file the prompt existed to
 * protect. The captured set still says "rot6.jpg", which is gone, so the
 * honest outcome is: delete nothing, say so, leave plain.jpg alone. */
static void
test_delete_ignores_marks_pruned_behind_the_prompt(void) {
   DirtyFixture             fx;
   static const char *const c_two[] = {"plain.jpg", "rot6.jpg", NULL};
   fixture_open_clean(&fx, "ggaze-enhance-delprune-XXXXXX", c_two);
   mark_file_and_return(&fx, "rot6.jpg");
   fixture_make_dirty(&fx);

   fire(fx.p_win, "win.delete");
   ggtest_drain_main(150);
   g_assert_nonnull(
      ggtest_wait_for_dialog(GTK_WINDOW(fx.p_win), "Cancel", 2000));

   remove_behind_the_prompt(&fx, "rot6.jpg");
   assert_marked_count(fx.p_win, 0); /* the mark really WAS pruned */

   answer_prompt(&fx, "Discard");
   ggtest_drain_main(200);

   /* The whole point: the unmarked file the user was looking at survives. */
   g_assert_true(file_exists_in(fx.c_dir, "plain.jpg"));
   g_assert_cmpstr(
      gtk_label_get_text(GTK_LABEL(ggaze_window_get_info_label(fx.p_win))), ==,
      "Nothing deleted — the file is gone");
   assert_ref_settled(&fx);

   fixture_teardown(&fx);
}

/* The milder variant, and the one decision #38 cares about: three files
 * marked, `D` pressed, two of them removed behind the prompt. Re-reading the
 * marks gave a count of 1, which took the single-file leg and deleted the
 * survivor with NO confirm dialog at all. With the set captured at press time
 * it is still three targets, so the ">1 marked" confirmation is still
 * required -- and answering Cancel there leaves everything on disk. */
static void
test_delete_confirm_uses_the_captured_count(void) {
   DirtyFixture             fx;
   static const char *const c_four[] = {"plain.jpg", "rgba.png", "rot6.jpg",
                                        "small.png", NULL};
   fixture_open_clean(&fx, "ggaze-enhance-delthree-XXXXXX", c_four);
   mark_file_and_return(&fx, "rgba.png");
   mark_file_and_return(&fx, "rot6.jpg");
   mark_file_and_return(&fx, "small.png");
   fixture_make_dirty(&fx);
   assert_showing(fx.p_win, "plain.jpg");
   assert_marked_count(fx.p_win, 3);

   fire(fx.p_win, "win.delete");
   ggtest_drain_main(150);
   g_assert_nonnull(
      ggtest_wait_for_dialog(GTK_WINDOW(fx.p_win), "Cancel", 2000));
   remove_behind_the_prompt(&fx, "rgba.png");
   remove_behind_the_prompt(&fx, "rot6.jpg");
   assert_marked_count(fx.p_win, 1); /* pruned 3 -> 1 */

   /* Answered by hand rather than through answer_prompt(): the confirm
    * dialog this must raise has a "Cancel" button of its own, so the
    * "no dialog remains" assertion there would fire on the very thing this
    * subtest is looking for. */
   g_assert_true(ggtest_click_dialog_button(GTK_WINDOW(fx.p_win), "Discard"));
   ggtest_drain_main(400);

   /* The >1-mark confirm must still be raised, for the captured three. */
   g_assert_nonnull(
      ggtest_wait_for_dialog(GTK_WINDOW(fx.p_win), "Delete", 2000));
   g_assert_true(ggtest_click_dialog_button(GTK_WINDOW(fx.p_win), "Cancel"));
   ggtest_drain_main(300);
   g_assert_true(file_exists_in(fx.c_dir, "small.png")); /* Cancel: intact */
   g_assert_true(file_exists_in(fx.c_dir, "plain.jpg"));

   fixture_teardown(&fx);
}

/* Round 4, finding (q): _do_delete_files advanced the cursor unconditionally,
 * so deleting a captured target that is no longer current skipped an image
 * the user never saw. `D` on plain.jpg, current moves on to rgba.png behind
 * the prompt, Discard -> plain.jpg is deleted and the window must STAY on
 * rgba.png (it used to jump on to rot6.jpg). */
static void
test_delete_does_not_advance_past_an_unseen_image(void) {
   DirtyFixture             fx;
   static const char *const c_three[] = {"plain.jpg", "rgba.png", "rot6.jpg",
                                         NULL};
   fixture_open_clean(&fx, "ggaze-enhance-delnav-XXXXXX", c_three);
   fixture_make_dirty(&fx);

   fire(fx.p_win, "win.delete"); /* `D` on plain.jpg, nothing marked */
   ggtest_drain_main(150);
   g_assert_nonnull(
      ggtest_wait_for_dialog(GTK_WINDOW(fx.p_win), "Cancel", 2000));
   ggaze_window_next(fx.p_win); /* what the slideshow tick does */
   ggtest_drain_main(300);
   assert_showing(fx.p_win, "rgba.png");

   answer_prompt(&fx, "Discard");
   ggtest_drain_main(300);

   g_assert_false(file_exists_in(fx.c_dir, "plain.jpg")); /* the captured one */
   assert_showing(fx.p_win, "rgba.png"); /* and rgba.png was NOT skipped */

   fixture_teardown(&fx);
}

/* Round 4, finding (u): a captured target removed externally used to sail
 * past _target_still_in_folder (a pure path comparison) and fail inside
 * trash_bin, with a bare g_warning and nothing on screen. `d` this time, so
 * both the trash and the delete side of the guard are covered. */
static void
test_trash_refuses_a_target_that_vanished(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-gonetrash-XXXXXX");

   fire(fx.p_win, "win.trash"); /* `d`, captured on plain.jpg */
   ggtest_drain_main(150);
   g_assert_nonnull(
      ggtest_wait_for_dialog(GTK_WINDOW(fx.p_win), "Cancel", 2000));
   remove_behind_the_prompt(&fx, "plain.jpg");

   answer_prompt(&fx, "Discard");
   ggtest_drain_main(200);

   g_assert_cmpstr(
      gtk_label_get_text(GTK_LABEL(ggaze_window_get_info_label(fx.p_win))), ==,
      "Nothing trashed — the file is gone");
   /* Nothing was binned, so no ./Trash bin was ever created. */
   g_assert_false(file_exists_in(fx.c_dir, "Trash"));
   g_assert_true(file_exists_in(fx.c_dir, "rot6.jpg"));
   assert_ref_settled(&fx);

   fixture_teardown(&fx);
}

/* Round 4: the queue's DISPLACE branch (window.c _save_prompt_queue), which
 * every earlier test missed because they all park exactly one request -- so
 * the classic double-free/leak site had zero coverage. Park two DIFFERENT
 * requests behind one prompt: the first must be released without ever
 * running (newest wins), the second must be the one retried. */
static void
test_second_queued_request_displaces_the_first(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-displace-XXXXXX");
   GError *p_err = NULL;
   char   *c_a   = g_dir_make_tmp("ggaze-enhance-dispA-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   char *c_b = g_dir_make_tmp("ggaze-enhance-dispB-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_a, "small.png");
   copy_fixture(c_b, "rgba.png");
   char  *c_pa = g_build_filename(c_a, "small.png", NULL);
   char  *c_pb = g_build_filename(c_b, "rgba.png", NULL);
   GFile *p_a  = g_file_new_for_path(c_pa);
   GFile *p_b  = g_file_new_for_path(c_pb);

   activate_other_cell(&fx);         /* request 1: raises the prompt */
   ggaze_window_open(fx.p_win, p_a); /* request 2: parked */
   ggaze_window_open(fx.p_win, p_b); /* request 3: DISPLACES request 2 */
   ggtest_drain_main(200);
   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    1);

   answer_prompt(&fx, "Discard");
   ggtest_drain_main(500);
   assert_showing(fx.p_win, "rgba.png"); /* the newest won ... */
   g_assert_null(
      g_strstr_len(window_title(fx.p_win), -1, "small.png")); /* ... only it */
   assert_ref_settled(&fx); /* the displaced _OpenCtx released its window ref */

   g_object_unref(p_a);
   g_object_unref(p_b);
   g_free(c_pa);
   g_free(c_pb);
   fixture_teardown(&fx);
   cleanup_temp_dir(c_a);
   cleanup_temp_dir(c_b);
}

/* Round 4, finding (r): when the answered prompt's OWN continuation is the
 * quit, the flushed request used to run against a window gtk_window_close()
 * had already taken down -- _open_now building a fresh Navigator,
 * GFileMonitor and texture load inside it. A closing window can honour no
 * request, so the queue is dropped instead. */
static void
test_quit_continuation_drops_the_queued_request(void) {
   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-quitdrop-XXXXXX");
   gtk_window_present(GTK_WINDOW(fx.p_win));
   ggtest_drain_main(300);
   GError *p_err   = NULL;
   char   *c_other = g_dir_make_tmp("ggaze-enhance-quitdrop2-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_other, "small.png");
   char  *c_target = g_build_filename(c_other, "small.png", NULL);
   GFile *p_target = g_file_new_for_path(c_target);

   gboolean b_stop = FALSE;
   g_signal_emit_by_name(fx.p_win, "close-request", &b_stop); /* Alt+F4 */
   ggtest_drain_main(200);
   g_assert_true(b_stop);                 /* blocked pending the prompt */
   ggaze_window_open(fx.p_win, p_target); /* D-Bus open, parked */
   ggtest_drain_main(200);

   answer_prompt(&fx, "Discard");
   ggtest_drain_main(600);

   g_assert_false(ggtest_is_open_toplevel(GTK_WINDOW(fx.p_win))); /* closed */
   /* And the queued open never ran: no fresh navigator was built into the
    * dying window, so its title still names the file it was opened on. */
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "small.png"));

   g_object_unref(p_target);
   g_free(c_target);
   fixture_teardown(&fx);
   cleanup_temp_dir(c_other);
}

/* Round 4: _move_go's capture behind a prompt -- the move twin of
 * test_prompt_acts_on_the_file_it_was_raised_for, listed as untested in both
 * of the last two review rounds. The row click captures plain.jpg; current
 * moves on to rot6.jpg behind the dialog; Discard must move plain.jpg. */
static void
test_move_acts_on_the_targets_captured_at_click(void) {
   GError *p_err = NULL;
   char   *c_dst = g_dir_make_tmp("ggaze-enhance-movecap-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   set_one_destination("dest one", c_dst);

   DirtyFixture fx;
   fixture_open(&fx, "ggaze-enhance-movecapsrc-XXXXXX");
   click_move_row(&fx); /* captures plain.jpg, raises the prompt */
   g_assert_nonnull(
      ggtest_wait_for_dialog(GTK_WINDOW(fx.p_win), "Cancel", 2000));
   ggaze_window_next(fx.p_win); /* what the slideshow tick does */
   ggtest_drain_main(300);
   assert_showing(fx.p_win, "rot6.jpg");

   answer_prompt(&fx, "Discard");
   ggtest_drain_main(300);

   g_assert_true(file_exists_in(c_dst, "plain.jpg"));   /* the captured one */
   g_assert_false(file_exists_in(c_dst, "rot6.jpg"));   /* not the current */
   g_assert_true(file_exists_in(fx.c_dir, "rot6.jpg")); /* still at home */

   fixture_teardown(&fx);
   reset_destinations();
   cleanup_temp_dir(c_dst);
}

/* Round 5, finding (x): _move_captured advanced the cursor whenever ANY file
 * moved, so moving a marked set that does not contain the current image took
 * the user off the image they were looking at and skipped the next one
 * unseen -- the surviving twin of (q) and of the round-3 trash fix. No
 * prompt is needed to show it: mark rot6.jpg only, stand on plain.jpg, move.
 * rot6.jpg must land in the destination and the window must STAY on
 * plain.jpg (it used to jump to rgba.png, skipping it). */
static void
test_move_does_not_advance_past_an_unseen_image(void) {
   GError *p_err = NULL;
   char   *c_dst = g_dir_make_tmp("ggaze-enhance-movenav-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   set_one_destination("dest one", c_dst);

   DirtyFixture             fx;
   static const char *const c_three[] = {"plain.jpg", "rgba.png", "rot6.jpg",
                                         NULL};
   fixture_open_clean(&fx, "ggaze-enhance-movenavsrc-XXXXXX", c_three);
   mark_file_and_return(&fx, "rot6.jpg"); /* back on plain.jpg, 1 marked */
   assert_showing(fx.p_win, "plain.jpg");

   g_assert_true(ggaze_window_move_index(fx.p_win, 0));
   ggtest_drain_main(300);

   g_assert_true(file_exists_in(c_dst, "rot6.jpg"));    /* the marked one */
   g_assert_true(file_exists_in(fx.c_dir, "rgba.png")); /* untouched */
   assert_showing(fx.p_win, "plain.jpg"); /* and rgba.png was NOT skipped */

   fixture_teardown(&fx);
   reset_destinations();
   cleanup_temp_dir(c_dst);
}

/* The other direction of the same gate: when the moved set DOES contain the
 * current image, the cursor must still advance -- otherwise the fix for (x)
 * would leave `m` parked on a file that is no longer there. Nothing marked,
 * so the target is plain.jpg itself. */
static void
test_move_advances_when_the_current_image_moves(void) {
   GError *p_err = NULL;
   char   *c_dst = g_dir_make_tmp("ggaze-enhance-movecur-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   set_one_destination("dest one", c_dst);

   DirtyFixture             fx;
   static const char *const c_three[] = {"plain.jpg", "rgba.png", "rot6.jpg",
                                         NULL};
   fixture_open_clean(&fx, "ggaze-enhance-movecursrc-XXXXXX", c_three);
   assert_showing(fx.p_win, "plain.jpg");

   g_assert_true(ggaze_window_move_index(fx.p_win, 0));
   ggtest_drain_main(300);

   g_assert_true(file_exists_in(c_dst, "plain.jpg")); /* the current one */
   assert_showing(fx.p_win, "rgba.png");              /* advanced past it */

   fixture_teardown(&fx);
   reset_destinations();
   cleanup_temp_dir(c_dst);
}

/* Registration split in two so neither function runs past the ~30-line
 * convention: the original feature coverage, and the round-2 subtests that
 * answer the real Save/Discard/Cancel prompt (see the file header). */
static void
add_feature_tests(void) {
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
}

static void
add_prompt_outcome_tests(void) {
   g_test_add_func("/enhance_flow/cancel_keeps_preview_and_frees_ctx",
                   test_cancel_keeps_preview_and_frees_ctx);
   g_test_add_func("/enhance_flow/discard_applies_deferred_grid_select",
                   test_discard_applies_deferred_grid_select);
   g_test_add_func("/enhance_flow/save_exports_then_applies_deferred_select",
                   test_save_exports_then_applies_deferred_select);
   g_test_add_func("/enhance_flow/failed_save_keeps_preview_and_aborts",
                   test_failed_save_keeps_preview_and_aborts);
   g_test_add_func("/enhance_flow/activating_current_cell_does_not_prompt",
                   test_activating_current_cell_does_not_prompt);
   g_test_add_func("/enhance_flow/toggle_view_keeps_dirty_preview",
                   test_toggle_view_keeps_dirty_preview);
   g_test_add_func("/enhance_flow/repeated_close_request_does_not_stack",
                   test_repeated_close_request_does_not_stack_dialogs);
   g_test_add_func("/enhance_flow/close_request_discard_closes_window",
                   test_close_request_discard_closes_window);
   g_test_add_func("/enhance_flow/open_while_dirty_cancel_then_discard",
                   test_open_while_dirty_cancel_then_discard);
   g_test_add_func("/enhance_flow/move_while_dirty_cancel_then_discard",
                   test_move_while_dirty_cancel_then_discard);
}

/* Round 3: everything that happens BEHIND an outstanding prompt (see the
 * section comment above test_prompt_acts_on_the_file_it_was_raised_for), plus
 * the two legs the earlier rounds left uncovered. */
static void
add_behind_the_prompt_tests(void) {
   g_test_add_func("/enhance_flow/prompt_acts_on_the_file_it_was_raised_for",
                   test_prompt_acts_on_the_file_it_was_raised_for);
   g_test_add_func("/enhance_flow/request_behind_stale_prompt_is_not_run",
                   test_request_behind_stale_prompt_is_not_run);
   g_test_add_func("/enhance_flow/save_with_no_preview_left_proceeds",
                   test_save_with_no_preview_left_reports_and_proceeds);
   g_test_add_func("/enhance_flow/second_request_is_queued_then_retried",
                   test_second_request_is_queued_then_retried);
   g_test_add_func("/enhance_flow/queued_request_is_dropped_on_cancel",
                   test_queued_request_is_dropped_on_cancel);
   g_test_add_func("/enhance_flow/close_request_save_closes_window",
                   test_close_request_save_closes_window);
   g_test_add_func("/enhance_flow/close_request_failed_save_keeps_window",
                   test_close_request_failed_save_keeps_window);
   g_test_add_func("/enhance_flow/cache_miss_reload_keeps_dirty_preview",
                   test_cache_miss_reload_keeps_dirty_preview);
}

/* Round 4: the permanent-delete legs behind the prompt (finding p and its
 * confirm-dialog variant), the cursor-advance twin (q), the quit/queue
 * interaction (r), the vanished-target guard (u), and the queue's displace
 * branch, which no earlier subtest reached.
 *
 * NOT covered here: _enhance_dispose's "a request is still parked" branch.
 * Driving it needs g_object_run_dispose() on a window with a live
 * GtkAlertDialog, which leaves the dialog's GTask unfinished and leaks its
 * _SaveCtx (474 B measured under ASan) -- a real ggaze-side leak on any
 * teardown under a live prompt, filed as task 2w0. The subtest was dropped
 * rather than shipped leaking; it comes back with 2w0's fix. */
static void
add_round4_tests(void) {
   g_test_add_func("/enhance_flow/delete_behind_prompt_deletes_marked_file",
                   test_delete_behind_prompt_deletes_the_marked_file);
   g_test_add_func("/enhance_flow/delete_ignores_marks_pruned_behind_prompt",
                   test_delete_ignores_marks_pruned_behind_the_prompt);
   g_test_add_func("/enhance_flow/delete_confirm_uses_the_captured_count",
                   test_delete_confirm_uses_the_captured_count);
   g_test_add_func("/enhance_flow/delete_does_not_advance_past_unseen_image",
                   test_delete_does_not_advance_past_an_unseen_image);
   g_test_add_func("/enhance_flow/trash_refuses_a_target_that_vanished",
                   test_trash_refuses_a_target_that_vanished);
   g_test_add_func("/enhance_flow/second_queued_request_displaces_the_first",
                   test_second_queued_request_displaces_the_first);
   g_test_add_func("/enhance_flow/quit_continuation_drops_queued_request",
                   test_quit_continuation_drops_the_queued_request);
   g_test_add_func("/enhance_flow/move_acts_on_targets_captured_at_click",
                   test_move_acts_on_the_targets_captured_at_click);
}

/* Round 5: both directions of `m`'s cursor advance (finding x), the gap that
 * let (q)'s twin survive five review rounds -- the round-4 move subtest
 * checks WHICH files moved, never where the cursor ended up. */
static void
add_round5_tests(void) {
   g_test_add_func("/enhance_flow/move_does_not_advance_past_unseen_image",
                   test_move_does_not_advance_past_an_unseen_image);
   g_test_add_func("/enhance_flow/move_advances_when_current_image_moves",
                   test_move_advances_when_the_current_image_moves);
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

   add_feature_tests();
   add_prompt_outcome_tests();
   add_behind_the_prompt_tests();
   add_round4_tests();
   add_round5_tests();
   return (g_test_run());
}
