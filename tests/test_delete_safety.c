/*:*
 * ggaze — delete-safety regression integration test
 *
 * Regression for eu0: _action_delete opened a GtkAlertDialog for >1 marks and
 * _delete_confirm_cb re-read p_win->p_nav marks after the async dialog
 * resolved. Because ggaze is single-instance, another open / drop can replace
 * the folder while the dialog is pending; confirming then permanently deleted
 * marks from the NEW folder rather than the files named by the prompt.
 *
 * The fix captures an immutable target list + owning directory at prompt time
 * and refuses to delete if the folder was replaced (window.h exposes
 * ggaze_window_delete_targets_still_current + ggaze_window_delete_captured so
 * the post-dialog decision is testable without driving the async native
 * dialog). This test exercises both branches directly:
 *
 *   1. folder-replaced: capture folder A's dir + targets, open folder B, then
 *      ggaze_window_delete_captured(win, dirA, capturedA) -> FALSE and NO file
 *      in either folder is deleted.
 *   2. happy path: capture folder A's targets, call
 *      ggaze_window_delete_captured(win, dirA, capturedA) while A is still
 *      open -> exactly the captured files are gone, the rest remain.
 *
 * The first three subtests exercise the post-dialog decision function
 * directly — it is the exact code the real callback runs — because injecting
 * a response into a GtkAlertDialog needs no API at all, only its buttons
 * (tests/helpers/gtk_helpers.h), which arrived later.
 *
 * Task aw0 added the group that DOES drive the real dialog, because the
 * questions it asks are about the dialog itself rather than about the
 * decision: what a native window close does while the confirm is on screen,
 * what a dispose underneath it does, and what "the user got rid of the
 * question" means for a dialog that permanently deletes files.
 *
 * Needs a display (integration suite; CI uses xvfb).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "gtk_helpers.h"
#include "viewer.h"
#include "window.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

/* --- helpers ------------------------------------------------------------ */

/* Windows built here are torn down with gtk_window_destroy(), never a plain
 * g_object_unref(): GTK4 hands the caller's reference to its internal
 * toplevel list and only destroy() takes the entry back out (it drops that
 * reference too, so the window still finalizes). Full rationale in
 * tests/helpers/gtk_helpers.h, "window teardown". */
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
   GFileEnumerator *p_e =
      g_file_enumerate_children(p_dir, "standard::name,standard::type",
                                G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (p_e != NULL) {
      GFileInfo *p_info;
      while ((p_info = g_file_enumerator_next_file(p_e, NULL, NULL)) != NULL) {
         GFile *p_child = g_file_get_child(p_dir, g_file_info_get_name(p_info));
         if (g_file_info_get_file_type(p_info) == G_FILE_TYPE_DIRECTORY) {
            GFileEnumerator *p_e2 = g_file_enumerate_children(
               p_child, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (p_e2 != NULL) {
               GFileInfo *p_i2;
               while ((p_i2 = g_file_enumerator_next_file(p_e2, NULL, NULL)) !=
                      NULL) {
                  GFile *p_c2 =
                     g_file_get_child(p_child, g_file_info_get_name(p_i2));
                  g_file_delete(p_c2, NULL, NULL);
                  g_object_unref(p_c2);
                  g_object_unref(p_i2);
               }
               g_object_unref(p_e2);
            }
         }
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

static gboolean
path_exists(const char *c_path) {
   GFile   *p_f = g_file_new_for_path(c_path);
   gboolean b   = g_file_query_exists(p_f, NULL);
   g_object_unref(p_f);
   return (b);
}

/* Build an owned GFile* list from c_dir + c_names (transfer-full per item). */
static GList *
build_file_list(const char *c_dir, const char *const *c_names, guint u_n) {
   GList *p_list = NULL;
   for (guint i = 0; i < u_n; i++) {
      char  *c_p = g_build_filename(c_dir, c_names[i], NULL);
      GFile *p_f = g_file_new_for_path(c_p);
      p_list     = g_list_prepend(p_list, p_f);
      g_free(c_p);
   }
   return (g_list_reverse(p_list));
}

/* --- subtests ----------------------------------------------------------- */

/* Folder-replaced branch: capture folder A's dir + targets, open folder B
 * (replacing p_nav, as a single-instance open / drop would), then call the
 * post-dialog decision. It must refuse (FALSE) and delete NOTHING in either
 * folder. This is the data-loss scenario from eu0. */
static void
test_delete_refused_after_folder_replaced(void) {
   GError *p_err  = NULL;
   char   *c_dirA = g_dir_make_tmp("ggaze-delsafe-a-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   char *c_dirB = g_dir_make_tmp("ggaze-delsafe-b-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dirA, "plain.jpg");
   copy_fixture(c_dirA, "rot6.jpg");
   copy_fixture(c_dirA, "small.png");
   copy_fixture(c_dirB, "plain.jpg");
   copy_fixture(c_dirB, "rot6.jpg");

   char        *c_a0  = g_build_filename(c_dirA, "plain.jpg", NULL);
   GFile       *p_a0  = g_file_new_for_path(c_a0);
   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_a0);
   wait_for_load(p_win);

   /* Mark all 3 in folder A (the bulk-delete prompt condition: >1 marks). */
   fire(p_win, "win.mark-all");
   drain_main(100);

   /* Capture the prompt-time context: the owning dir + the target list. */
   GFile            *p_dirA      = g_file_new_for_path(c_dirA);
   const char *const c_a_names[] = {"plain.jpg", "rot6.jpg", "small.png"};
   GList            *p_captured =
      build_file_list(c_dirA, c_a_names, G_N_ELEMENTS(c_a_names));

   /* The folder is still A: still-current must be TRUE before replacement. */
   g_assert_true(ggaze_window_delete_targets_still_current(p_win, p_dirA));

   /* Replace the folder while the (notional) dialog is pending, exactly as a
    * single-instance open / drop does. */
   char  *c_b0 = g_build_filename(c_dirB, "plain.jpg", NULL);
   GFile *p_b0 = g_file_new_for_path(c_b0);
   ggaze_window_open(p_win, p_b0); /* swaps p_nav to folder B */
   drain_main(200);
   GFile *p_dirB = g_file_new_for_path(c_dirB);
   g_assert_true(ggaze_window_delete_targets_still_current(p_win, p_dirB));
   /* The captured folder-A dir is no longer current -> must be FALSE. */
   g_assert_false(ggaze_window_delete_targets_still_current(p_win, p_dirA));

   /* Confirm the dialog affirmatively: the safety check must refuse. */
   gboolean b_proceeded =
      ggaze_window_delete_captured(p_win, p_dirA, p_captured);
   g_assert_false(b_proceeded);
   drain_main(200);

   /* No file in either folder was deleted. */
   for (guint i = 0; i < G_N_ELEMENTS(c_a_names); i++) {
      char *c_p = g_build_filename(c_dirA, c_a_names[i], NULL);
      g_assert_true(path_exists(c_p));
      g_free(c_p);
   }
   const char *const c_b_names[] = {"plain.jpg", "rot6.jpg"};
   for (guint i = 0; i < G_N_ELEMENTS(c_b_names); i++) {
      char *c_p = g_build_filename(c_dirB, c_b_names[i], NULL);
      g_assert_true(path_exists(c_p));
      g_free(c_p);
   }

   g_list_free_full(p_captured, (GDestroyNotify)g_object_unref);
   g_object_unref(p_dirA);
   g_object_unref(p_dirB);
   g_object_unref(p_b0);
   g_free(c_b0);
   g_object_unref(p_a0);
   g_free(c_a0);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dirA);
   cleanup_temp_dir(c_dirB);
}

/* Happy path: with the folder still open, ggaze_window_delete_captured deletes
 * EXACTLY the captured files (not a re-read of marks) and leaves the rest. */
static void
test_delete_captured_deletes_only_targets(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-delsafe-happy-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   copy_fixture(c_dir, "rot6.jpg");
   copy_fixture(c_dir, "small.png");

   char        *c_p0  = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile       *p_f0  = g_file_new_for_path(c_p0);
   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_f0);
   wait_for_load(p_win);

   /* Capture two of the three files as the prompt-time targets. */
   GFile            *p_dir    = g_file_new_for_path(c_dir);
   const char *const c_del[]  = {"plain.jpg", "rot6.jpg"};
   const char *const c_keep[] = {"small.png"};
   GList *p_captured = build_file_list(c_dir, c_del, G_N_ELEMENTS(c_del));

   g_assert_true(ggaze_window_delete_targets_still_current(p_win, p_dir));
   gboolean b_proceeded =
      ggaze_window_delete_captured(p_win, p_dir, p_captured);
   g_assert_true(b_proceeded);
   drain_main(300);

   /* The captured files are gone; the un-captured file remains. */
   for (guint i = 0; i < G_N_ELEMENTS(c_del); i++) {
      char *c_p = g_build_filename(c_dir, c_del[i], NULL);
      g_assert_false(path_exists(c_p));
      g_free(c_p);
   }
   for (guint i = 0; i < G_N_ELEMENTS(c_keep); i++) {
      char *c_p = g_build_filename(c_dir, c_keep[i], NULL);
      g_assert_true(path_exists(c_p));
      g_free(c_p);
   }

   g_list_free_full(p_captured, (GDestroyNotify)g_object_unref);
   g_object_unref(p_dir);
   g_object_unref(p_f0);
   g_free(c_p0);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   cleanup_temp_dir(c_dir);
}

/* NULL-nav branch: with nothing open, no captured dir is "still current" and
 * ggaze_window_delete_captured refuses without touching anything. */
static void
test_delete_targets_still_current_no_nav(void) {
   GgazeWindow *p_win = new_window();
   GFile       *p_any = g_file_new_for_path("/tmp");
   g_assert_false(ggaze_window_delete_targets_still_current(p_win, p_any));
   g_assert_false(ggaze_window_delete_captured(p_win, p_any, NULL));
   g_object_unref(p_any);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(100);
}

/* --- aw0: the real confirm dialog, on screen ----------------------------
 *
 * Everything above answers "what does the decision function do?". This group
 * answers "what happens to the dialog itself?", which needs a real one up:
 * a `D` on >1 marked files, raised through the actual action.
 *
 * The three files are all marked, so the confirm dialog names all three and
 * "nothing was deleted" is checkable file by file. */
typedef struct {
   GgazeWindow *p_win; /* held with a ref of the TEST's own, so the window
                        * stays a valid pointer even after a close/dispose
                        * takes it out of the toplevel list */
   char *c_dir;        /* temp folder (owned) */
   guint u_ref;        /* p_win's refcount with the marks set and everything
                        * settled, i.e. BEFORE the confirm dialog's _DeleteCtx
                        * takes its own window ref. A ctx that is never freed
                        * pins the whole GgazeWindow, so comparing against this
                        * catches it in the plain lanes too, not only ASan */
} ConfirmFixture;

static const char *const CONFIRM_FILES[] = {"plain.jpg", "rot6.jpg",
                                            "small.png"};

/* Open a window on a fresh folder of CONFIRM_FILES, mark them all, press `D`
 * and wait for the confirm dialog. Leaves that dialog UP: every caller must
 * resolve it (answer, dismiss or dispose) before fixture_confirm_teardown,
 * because an unanswered dialog holds the _DeleteCtx's window ref and would
 * both keep the window alive and show up as a leak under ASan. */
static void
fixture_confirm_open(ConfirmFixture *p_fx, const char *c_tmpl) {
   GError *p_err = NULL;
   p_fx->c_dir   = g_dir_make_tmp(c_tmpl, &p_err);
   g_assert_no_error(p_err);
   for (guint u = 0; u < G_N_ELEMENTS(CONFIRM_FILES); u++) {
      copy_fixture(p_fx->c_dir, CONFIRM_FILES[u]);
   }
   char  *c_first = g_build_filename(p_fx->c_dir, CONFIRM_FILES[0], NULL);
   GFile *p_first = g_file_new_for_path(c_first);
   p_fx->p_win    = new_window();
   g_object_ref(p_fx->p_win);
   ggaze_window_open(p_fx->p_win, p_first);
   wait_for_load(p_fx->p_win);
   /* Presented on purpose: gtk_window_close() is a no-op on a window that was
    * never realized, and it is the path Alt+F4 and the WM button take. */
   gtk_window_present(GTK_WINDOW(p_fx->p_win));
   drain_main(200);
   fire(p_fx->p_win, "win.mark-all");
   drain_main(100);
   p_fx->u_ref = ((GObject *)p_fx->p_win)->ref_count;
   fire(p_fx->p_win, "win.delete"); /* >1 marks -> the confirm dialog */
   GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(p_fx->p_win), "Delete");
   /* The dialog's _DeleteCtx owns a window ref for as long as it lives. */
   g_assert_cmpuint(((GObject *)p_fx->p_win)->ref_count, ==, p_fx->u_ref + 1);
   g_object_unref(p_first);
   g_free(c_first);
}

static void
fixture_confirm_teardown(ConfirmFixture *p_fx) {
   gtk_window_destroy(GTK_WINDOW(p_fx->p_win)); /* no-op if already closed */
   g_object_unref(p_fx->p_win);                 /* the fixture's own ref */
   drain_main(300);
   cleanup_temp_dir(p_fx->c_dir);
}

/* Assert every file the fixture created is still on disk (nothing deleted). */
static void
assert_all_files_present(ConfirmFixture *p_fx) {
   for (guint u = 0; u < G_N_ELEMENTS(CONFIRM_FILES); u++) {
      char *c_p = g_build_filename(p_fx->c_dir, CONFIRM_FILES[u], NULL);
      g_assert_true(path_exists(c_p));
      g_free(c_p);
   }
}

/* aw0's main bug. Task 2w0 gated _on_close_request on b_save_prompt, which is
 * the SAVE prompt's flag: the `D` confirm is raised after that prompt has
 * already resolved, so the flag is FALSE and the enhance mask is clear while
 * the confirm is on screen. A native close (Alt+F4 / the WM button -- not
 * input events, so the dialog's modal grab does not swallow them) therefore
 * propagated into gtk_window_destroy(), which cannot dispose the window (the
 * _DeleteCtx holds an owned window ref), so nothing cancelled the dialog: it
 * stayed on screen, orphaned, still answerable, and answering it would have
 * run ggaze_window_delete_captured against a destroyed window.
 *
 * Both close entry points are exercised deliberately: the raw signal emission
 * pins the handler's own verdict, and gtk_window_close() is the path Alt+F4
 * actually takes and the one that would destroy the window.
 *
 * Blocking is only half of it -- the user must keep a way out. Unlike the Save
 * prompt, no request is queued behind the confirm: the queue belongs to the
 * Save prompt, which is not up here, so routing this close through
 * _maybe_save_then would either do nothing whatsoever (clean mask -- the
 * re-entrant gtk_window_close() is a no-op) or stack a second modal dialog on
 * the confirm (dirty mask); see _on_close_request. The way out is the dialog
 * itself: answer it, and the next close goes through. */
static void
test_close_request_blocked_while_confirm_is_up(void) {
   ConfirmFixture fx;
   fixture_confirm_open(&fx, "ggaze-delsafe-closeconfirm-XXXXXX");

   gboolean b_stop = FALSE;
   g_signal_emit_by_name(fx.p_win, "close-request", &b_stop);
   g_assert_true(b_stop); /* the handler must refuse the close */

   gtk_window_close(GTK_WINDOW(fx.p_win)); /* the real Alt+F4 path */
   drain_main(300);
   g_assert_true(ggtest_is_open_toplevel(GTK_WINDOW(fx.p_win)));
   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Delete"), ==,
                    1); /* not orphaned: still parented, still answerable */
   assert_all_files_present(&fx);

   /* The way out: answering the confirm clears the gate. */
   g_assert_true(ggtest_click_dialog_button(GTK_WINDOW(fx.p_win), "Cancel"));
   drain_main(300);
   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Delete"), ==,
                    0);
   assert_all_files_present(&fx); /* Cancel deletes nothing */

   gtk_window_close(GTK_WINDOW(fx.p_win));
   drain_main(300);
   g_assert_false(ggtest_is_open_toplevel(GTK_WINDOW(fx.p_win)));

   fixture_confirm_teardown(&fx); /* the fixture ref is what keeps this valid */
}

/* The dispose-time cancel (aw0, mirroring 2w0's for the Save prompt). Without
 * a GCancellable nothing but the dialog itself could finish its GTask, so a
 * dispose underneath it abandoned the _DeleteCtx and its deep-copied target
 * list; with one, the task completes, _delete_confirm_cb runs and everything
 * is released.
 *
 * The refcount is what proves that, not the dialog's disappearance: the dialog
 * goes away either way, because GTK wires destroy-with-parent to the parent's
 * ::destroy and GtkWidget emits that from dispose. The abandoned _DeleteCtx
 * stays behind holding its window ref (and its deep copy of the target list),
 * which is exactly what the ref_count assertion below catches.
 *
 * The files must survive that cancel, and NOT because the delete happened to
 * be refused downstream: gtk_alert_dialog_choose_finish() reports a cancel as
 * -1 plus an error, and -1 read as a gboolean is TRUE, so the callback's old
 * `gboolean b_ok = gtk_alert_dialog_choose_finish(...)` would have taken the
 * cancel for a confirmed permanent delete. (Dispose clears p_nav first, so
 * ggaze_window_delete_captured would refuse anyway -- this asserts the files
 * because that is what the user would lose if either guard went away.)
 *
 * g_object_run_dispose() is the only way in, exactly as in
 * test_dispose_under_a_live_prompt_releases_it: gtk_window_destroy() cannot
 * dispose a window whose confirm dialog still holds a ref on it. */
static void
test_dispose_under_a_live_confirm_cancels_it(void) {
   ConfirmFixture fx;
   fixture_confirm_open(&fx, "ggaze-delsafe-disposeconfirm-XXXXXX");

   g_object_run_dispose(G_OBJECT(fx.p_win));
   drain_main(400);

   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Delete"), ==,
                    0);
   /* The cancel completed the dialog's GTask, so its callback ran and the
    * _DeleteCtx released the window ref it had taken. */
   g_assert_cmpuint(((GObject *)fx.p_win)->ref_count, ==, fx.u_ref);
   assert_all_files_present(&fx);

   fixture_confirm_teardown(&fx);
}

/* Getting rid of the question is not an answer. Escape / the dialog's own WM
 * close end in gtk_window_close() on the dialog toplevel, which GTK reports
 * as -1 plus GTK_DIALOG_ERROR_DISMISSED unless a cancel button is configured
 * -- and -1 as a gboolean is TRUE, so before aw0 the callback took a dismissed
 * confirm for a confirmed PERMANENT delete of every marked file. Two things
 * now stop that: _delete_confirm_ask names button 0 as the cancel button (so
 * the dismissal arrives as a plain Cancel answer), and
 * _delete_confirm_answered_yes requires no error AND the Delete button's own
 * index. */
static void
test_dismissing_the_confirm_deletes_nothing(void) {
   ConfirmFixture fx;
   fixture_confirm_open(&fx, "ggaze-delsafe-dismissconfirm-XXXXXX");

   GtkWindow *p_dlg = GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(fx.p_win), "Delete");
   gtk_window_close(p_dlg); /* what Escape does */
   drain_main(300);

   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Delete"), ==,
                    0);
   assert_all_files_present(&fx);
   g_assert_true(ggtest_is_open_toplevel(GTK_WINDOW(fx.p_win)));

   fixture_confirm_teardown(&fx);
}

/* The other half of the classification: pressing Delete really does delete.
 * Without this, a wrong button index in _delete_confirm_answered_yes would
 * quietly turn `D` into a no-op and every "nothing was deleted" assertion
 * above would still pass. */
static void
test_confirming_deletes_the_marked_files(void) {
   ConfirmFixture fx;
   fixture_confirm_open(&fx, "ggaze-delsafe-confirmdelete-XXXXXX");

   g_assert_true(ggtest_click_dialog_button(GTK_WINDOW(fx.p_win), "Delete"));
   drain_main(400);

   for (guint u = 0; u < G_N_ELEMENTS(CONFIRM_FILES); u++) {
      char *c_p = g_build_filename(fx.c_dir, CONFIRM_FILES[u], NULL);
      g_assert_false(path_exists(c_p));
      g_free(c_p);
   }

   fixture_confirm_teardown(&fx);
}

/* Registration split so neither function runs past the ~30-line convention:
 * the original decision-function coverage, and aw0's real-dialog group. */
static void
add_confirm_dialog_tests(void) {
   g_test_add_func("/delete_safety/close_request_blocked_while_confirm_is_up",
                   test_close_request_blocked_while_confirm_is_up);
   g_test_add_func("/delete_safety/dispose_under_a_live_confirm_cancels_it",
                   test_dispose_under_a_live_confirm_cancels_it);
   g_test_add_func("/delete_safety/dismissing_the_confirm_deletes_nothing",
                   test_dismissing_the_confirm_deletes_nothing);
   g_test_add_func("/delete_safety/confirming_deletes_the_marked_files",
                   test_confirming_deletes_the_marked_files);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
   if (!gtk_init_check()) {
      /* Exit 77 is meson's "skipped". Returning g_test_run() here
       * instead exits 0 after a "1..0" plan -- a lane that reports OK
       * while running nothing, which is how displayless runs used to
       * hide the GDK backend leaks. See tests/meson.build "Lane
       * determinism" (1w0). */
      g_print("1..0 # SKIP no display available (run under xvfb)\n");
      return (77);
   }
   g_test_add_func("/delete_safety/refused_after_folder_replaced",
                   test_delete_refused_after_folder_replaced);
   g_test_add_func("/delete_safety/captured_deletes_only_targets",
                   test_delete_captured_deletes_only_targets);
   g_test_add_func("/delete_safety/still_current_no_nav",
                   test_delete_targets_still_current_no_nav);
   add_confirm_dialog_tests();
   return (g_test_run());
}