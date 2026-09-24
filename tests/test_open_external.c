/*:*
 * ggaze — open-external integration test
 *
 * Exercises the `e` open-external wiring in the window: the public launch
 * helper ggaze_window_open_external_index() invokes opener_launch on the
 * ORIGINAL current file with %f expanded, and the popup it drives lists the
 * configured editors with auto-assigned 1-9,0,a-z hotkeys and a key
 * controller. A "capture editor" (sh -c 'echo "$1" > CAPTURE' sh %f) records
 * the expanded path so the test can assert %f substitution on a weird
 * filename (spaces + shell metacharacters), exactly the regression hu0
 * guards at the opener level. Uses the memory GSettings backend + build-tree
 * schema (set in the meson env); display-gated, skipped without a display.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "gtk_helpers.h"
#include "opener.h"
#include "settings.h"
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
 * tests/helpers/gtk_helpers.h, "window teardown".
 *
 * The focus grab is not cosmetic (5w0): this suite pops up the `e`
 * open-external popover, and a never-presented toplevel has no focus widget,
 * which walks GTK into an unguarded NULL on the X11 backend CI runs under.
 * See ggtest_focus_viewer() in tests/helpers/gtk_helpers.h for the full
 * mechanism. */
static GgazeWindow *
new_window(void) {
   GgazeWindow *p_win = GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL));
   ggtest_focus_viewer(p_win);
   return (p_win);
}

static void
drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

static void
copy_fixture(const char *c_dir, const char *c_src_name,
             const char *c_dst_name) {
   const gchar *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char   *c_src = g_build_filename(c_fx, c_src_name, NULL);
   char   *c_dst = g_build_filename(c_dir, c_dst_name, NULL);
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

/* Reset the editors a(ss) key to empty via the shared memory backend, so
 * tests don't leak editors into each other. */
static void
reset_editors(void) {
   Settings *p_s = settings_new();
   g_assert_nonnull(p_s);
   g_settings_reset(settings_get_gsettings(p_s), "editors");
   settings_delete(p_s);
}

/* Configure a single capture editor whose command writes the expanded %f
 * path to c_capture. The template is parsed by g_shell_parse_argv first, so
 * the single-quoted sh -c body is one argv element and %f is substituted
 * into the parsed `sh` argv0 placeholder — the weird path lands as exactly
 * one shell word ($1), never re-expanded. */
static void
set_capture_editor(const char *c_capture) {
   char *c_cmd = g_strdup_printf("sh -c 'echo \"$1\" > %s' sh %%f", c_capture);
   GPtrArray    *p_pairs = g_ptr_array_new_with_free_func(settings_pair_free);
   SettingsPair *pr      = g_new(SettingsPair, 1);
   pr->c_name            = g_strdup("Capture");
   pr->c_value           = c_cmd;
   g_ptr_array_add(p_pairs, pr);
   Settings *p_s = settings_new();
   g_assert_nonnull(p_s);
   g_assert_cmpint(settings_set_editors(p_s, p_pairs), ==, 1);
   settings_delete(p_s);
   g_ptr_array_unref(p_pairs);
}

/* Configure N named editors with harmless `true %f` commands. */
static void
set_named_editors(guint u_n) {
   GPtrArray  *p_pairs   = g_ptr_array_new_with_free_func(settings_pair_free);
   const char *c_names[] = {"GIMP", "ImageMagick identify", "Nomacs"};
   for (guint i = 0; i < u_n; i++) {
      SettingsPair *pr = g_new(SettingsPair, 1);
      pr->c_name       = g_strdup(c_names[i % G_N_ELEMENTS(c_names)]);
      pr->c_value      = g_strdup("true %f");
      g_ptr_array_add(p_pairs, pr);
   }
   Settings *p_s = settings_new();
   g_assert_nonnull(p_s);
   settings_set_editors(p_s, p_pairs);
   settings_delete(p_s);
   g_ptr_array_unref(p_pairs);
}

/* Wait until the capture file exists and is non-empty, then return its
 * contents (caller frees). Returns NULL on timeout. */
static char *
read_capture(const char *c_capture, guint u_timeout_ms) {
   for (guint u = 0; u < u_timeout_ms; u += 20) {
      gchar *c_contents = NULL;
      gsize  u_len      = 0;
      if (g_file_get_contents(c_capture, &c_contents, &u_len, NULL) &&
          u_len > 0) {
         return (c_contents);
      }
      g_free(c_contents);
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(20000);
   }
   return (NULL);
}

/* Fire a win.* action on the window. */
static void
fire(GgazeWindow *p_win, const char *c_action) {
   gtk_widget_activate_action(GTK_WIDGET(p_win), c_action, NULL);
}

/* Find the GtkPopover parented to the window's stack (the open-external
 * popover is gtk_widget_set_parent'd to p_stack). Returns the first
 * GtkPopover among the stack's direct children, or NULL. */
static GtkPopover *
find_open_external_popover(GgazeWindow *p_win) {
   GtkStack  *p_stack = ggaze_window_get_stack(p_win);
   GtkWidget *p_child = gtk_widget_get_first_child(GTK_WIDGET(p_stack));
   while (p_child != NULL) {
      if (GTK_IS_POPOVER(p_child)) {
         return (GTK_POPOVER(p_child));
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (NULL);
}

/* Count GtkButton children of the popover's content box. */
static guint
popover_button_count(GtkPopover *p_pop) {
   GtkWidget *p_box = gtk_popover_get_child(p_pop);
   if (p_box == NULL) {
      return (0);
   }
   guint      u_n     = 0;
   GtkWidget *p_child = gtk_widget_get_first_child(p_box);
   while (p_child != NULL) {
      if (GTK_IS_BUTTON(p_child)) {
         u_n++;
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (u_n);
}

/* The i-th GtkButton in the popover's content box, or NULL. */
static GtkWidget *
popover_button(GtkPopover *p_pop, guint u_idx) {
   GtkWidget *p_box = gtk_popover_get_child(p_pop);
   g_assert_nonnull(p_box);
   GtkWidget *p_child = gtk_widget_get_first_child(p_box);
   guint      u_i     = 0;
   while (p_child != NULL) {
      if (GTK_IS_BUTTON(p_child)) {
         if (u_i == u_idx) {
            return (p_child);
         }
         u_i++;
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (NULL);
}

/* The label text of the i-th GtkButton in the popover's content box, or NULL
 * when there is no such button -- the walk lives in popover_button() only. */
static const char *
popover_button_label(GtkPopover *p_pop, guint u_idx) {
   GtkWidget *p_btn = popover_button(p_pop, u_idx);
   return (p_btn == NULL ? NULL : gtk_button_get_label(GTK_BUTTON(p_btn)));
}

/* TRUE iff a GtkEventControllerKey is attached to p_pop. */
static gboolean
popover_has_key_controller(GtkPopover *p_pop) {
   GListModel *p_ctrls = gtk_widget_observe_controllers(GTK_WIDGET(p_pop));
   gboolean    b_found = FALSE;
   guint       u_n     = g_list_model_get_n_items(p_ctrls);
   for (guint i = 0; i < u_n; i++) {
      GObject *p_obj = g_list_model_get_item(p_ctrls, i);
      if (GTK_IS_EVENT_CONTROLLER_KEY(p_obj)) {
         b_found = TRUE;
      }
      g_object_unref(p_obj);
   }
   g_object_unref(p_ctrls);
   return (b_found);
}

/* --- subtests ----------------------------------------------------------- */

/* The helper launches the configured capture editor on the ORIGINAL current
 * file; the capture file receives the expanded %f path verbatim, including
 * spaces and shell metacharacters in the filename. */
static void
test_open_external_launches_capture(void) {
   reset_editors();
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-oe-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   /* A weird filename with spaces, $, and backticks — the hu0 regression
    * pattern. Copy plain.jpg (a real JPEG) so the navigator/loader accept it.
    */
   const char *c_weird = "file with spaces $HOME `whoami`.jpg";
   copy_fixture(c_dir, "plain.jpg", c_weird);
   char  *c_path   = g_build_filename(c_dir, c_weird, NULL);
   GFile *p_file   = g_file_new_for_path(c_path);
   char  *c_capdir = g_dir_make_tmp("ggaze-cap-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   char *c_capture = g_build_filename(c_capdir, "captured.txt", NULL);
   set_capture_editor(c_capture);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_file);
   drain_main(200);
   g_assert_true(ggaze_window_open_external_index(p_win, 0));

   char *c_contents = read_capture(c_capture, 3000);
   g_assert_nonnull(c_contents);
   /* echo appends a newline; strip it and compare to the file's path. */
   g_strstrip(c_contents);
   char *c_want = g_file_get_path(p_file);
   g_assert_cmpstr(c_contents, ==, c_want);
   g_free(c_want);
   g_free(c_contents);

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   g_free(c_path);
   g_free(c_capture);
   cleanup_temp_dir(c_capdir);
   cleanup_temp_dir(c_dir);
   reset_editors();
}

/* With no editors configured, the helper is a safe no-op (returns FALSE, no
 * crash, no subprocess). */
static void
test_open_external_empty_no_editors(void) {
   reset_editors();
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-oe-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg", "plain.jpg");
   char  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_file);
   drain_main(200);
   g_assert_false(ggaze_window_open_external_index(p_win, 0));
   g_assert_false(ggaze_window_open_external_index(p_win, 99));

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   g_free(c_path);
   cleanup_temp_dir(c_dir);
   reset_editors();
}

/* An out-of-range index is rejected (FALSE); a valid one launches. */
static void
test_open_external_out_of_range(void) {
   reset_editors();
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-oe-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg", "plain.jpg");
   char  *c_path   = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file   = g_file_new_for_path(c_path);
   char  *c_capdir = g_dir_make_tmp("ggaze-cap-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   char *c_capture = g_build_filename(c_capdir, "captured.txt", NULL);
   set_capture_editor(c_capture);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_file);
   drain_main(200);
   g_assert_false(ggaze_window_open_external_index(p_win, 1)); /* only 0 */
   g_assert_false(ggaze_window_open_external_index(p_win, 100));
   g_assert_true(ggaze_window_open_external_index(p_win, 0));
   char *c_contents = read_capture(c_capture, 3000);
   g_assert_nonnull(c_contents);
   g_strstrip(c_contents);
   char *c_want = g_file_get_path(p_file);
   g_assert_cmpstr(c_contents, ==, c_want);
   g_free(c_want);
   g_free(c_contents);

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   g_free(c_path);
   g_free(c_capture);
   cleanup_temp_dir(c_capdir);
   cleanup_temp_dir(c_dir);
   reset_editors();
}

/* The popup: firing win.open-external pops up a GtkPopover listing the
 * configured editors as clickable rows with auto-assigned hotkey labels
 * (1, 2, 3 ...) and a key controller; firing it again toggles it closed. */
static void
test_open_external_popup_structure(void) {
   reset_editors();
   set_named_editors(3);
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-oe-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg", "plain.jpg");
   char  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_file);
   drain_main(200);

   g_assert_null(find_open_external_popover(p_win));
   fire(p_win, "win.open-external");
   drain_main(200);
   GtkPopover *p_pop = find_open_external_popover(p_win);
   g_assert_nonnull(p_pop);
   g_assert_cmpint(popover_button_count(p_pop), ==, 3);
   /* Hotkey labels in list order: "1  GIMP", "2  ImageMagick identify",
    * "3  Nomacs". */
   g_assert_cmpstr(popover_button_label(p_pop, 0), ==, "1  GIMP");
   g_assert_cmpstr(popover_button_label(p_pop, 1), ==,
                   "2  ImageMagick identify");
   g_assert_cmpstr(popover_button_label(p_pop, 2), ==, "3  Nomacs");
   g_assert_true(popover_has_key_controller(p_pop));

   /* Toggle: a second win.open-external closes it. */
   fire(p_win, "win.open-external");
   drain_main(200);
   g_assert_null(find_open_external_popover(p_win));

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   g_free(c_path);
   cleanup_temp_dir(c_dir);
   reset_editors();
}

/* The empty-editors case still pops up a (message-only) popover rather than
 * silently doing nothing, so the user sees why `e` did nothing. */
static void
test_open_external_popup_empty_message(void) {
   reset_editors();
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-oe-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg", "plain.jpg");
   char  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_file);
   drain_main(200);
   fire(p_win, "win.open-external");
   drain_main(200);
   GtkPopover *p_pop = find_open_external_popover(p_win);
   g_assert_nonnull(p_pop);
   g_assert_cmpint(popover_button_count(p_pop), ==, 0); /* message, no rows */

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   g_free(c_path);
   cleanup_temp_dir(c_dir);
   reset_editors();
}

/* Present p_win and wait until its toplevel is really mapped. The result is
 * asserted by the caller, never ignored: a subtest that silently skipped the
 * mapping it exists to check would be the vacuous-green trap all over again.
 * Returns FALSE on timeout rather than aborting here, for the same reason as
 * wait_for_popover_mapped() below -- on a genuinely broken desktop this is
 * what fires, and "present_and_map(p_win) should be TRUE" at the call site
 * names what timed out, where g_assert_not_reached()'s "code should not be
 * reached" does not. */
static gboolean
present_and_map(GgazeWindow *p_win) {
   gtk_window_set_default_size(GTK_WINDOW(p_win), 400, 300);
   gtk_window_present(GTK_WINDOW(p_win));
   for (guint u = 0; u < 2000; u++) {
      if (gtk_widget_get_mapped(GTK_WIDGET(p_win))) {
         return (TRUE);
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   return (FALSE);
}

/* Iterate the main loop until it has dispatched a G_PRIORITY_DEFAULT idle
 * queued now: i.e. everything of higher priority that is ready runs first,
 * and nothing that waits for the default priority (a GTK tooltip timeout,
 * say) has run yet when this returns. */
static gboolean
_set_flag(gpointer p_flag) {
   *(gboolean *)p_flag = TRUE;
   return (G_SOURCE_REMOVE);
}

static void
run_until_default_priority(void) {
   gboolean b_ran = FALSE;
   g_idle_add_full(G_PRIORITY_DEFAULT, _set_flag, &b_ran, NULL);
   while (!b_ran) {
      g_main_context_iteration(g_main_context_default(), TRUE);
   }
}

/* Wait until p_win's open-external popover reports itself mapped. Returns
 * FALSE on timeout so the caller can assert with a useful message. */
static gboolean
wait_for_popover_mapped(GgazeWindow *p_win) {
   for (guint u = 0; u < 2000; u++) {
      GtkPopover *p_pop = find_open_external_popover(p_win);
      if (p_pop != NULL && gtk_widget_get_mapped(GTK_WIDGET(p_pop))) {
         return (TRUE);
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   return (FALSE);
}

/* THE one subtest in this suite that presents its window (cw0 -- commit
 * 9aeb902's subject says "bw0", which was the deleted duplicate filing of the
 * same task and resolves to nothing; the rationale lives under cw0).
 *
 * Every other popover subtest here builds a GgazeWindow with g_object_new()
 * and never presents it, which is cheap and fine for what they assert --
 * row count, hotkey labels, the key controller -- because those are all
 * construction-time facts about the widget tree.
 *
 * But it makes them silent about whether the popover ever actually appears,
 * and the two backends genuinely differ there: gdk_popup_present() refuses a
 * popup whose parent surface is unmapped, so on Wayland gtk_popover_show()
 * returns before gtk_widget_map() and the popover is visible-but-never-
 * mapped, while on X11 (what CI's xvfb-run gives it) the same code maps it
 * for real. Measured with a probe on all three backends in 5w0: Wayland
 * mapped=0, live X11 mapped=1, xvfb mapped=1.
 *
 * So a green Wayland run said nothing about presentation at all. Rather than
 * make forty subtests present -- five suites popping real windows on a
 * developer's desktop, for coverage they already have -- exactly one subtest
 * presents and asserts that the popover MAPS. It needs no backend-conditional
 * assertion: with a mapped parent the popover maps on Wayland and X11 alike.
 *
 * SCOPE, so this is not read as more than it is. Of the four presentation
 * aspects (mapping, positioning, autohide/grab behaviour, dismissal) this
 * asserts mapping; GTK's own dismissal -- "closed" -> popup_list.c's
 * _on_closed() -> popup_list_delete() -- is driven by gtk_popdown_frees_safely
 * and tall_list_closes_cleanly below (gg2); positioning is unasserted on
 * BOTH backends. And this covers the `e` builder only: `a`, `!` and `m` in
 * src/window.c build their popovers independently, so a regression confined to
 * one of them would leave that popover unmapped with nothing failing anywhere.
 * See tests/helpers/gtk_helpers.h, "window focus", for the full residual.
 *
 * WHAT THIS SUBTEST DOES AND DOES NOT BUY YOU, measured by replacing the
 * g_assert_true(present_and_map(p_win)) call below with `(void)
 * present_and_map;` and re-running (do this again if you edit it -- a mapping
 * assertion that cannot fail is precisely the thing being fixed):
 *
 *   - without present, on Wayland: FAILS. Here the presenting is doing real
 *     work and this subtest is the only thing in the tree that notices.
 *   - without present, on X11/xvfb: still PASSES, because X11 maps a popup
 *     regardless of whether its parent toplevel is mapped. So on CI the
 *     present_and_map() call is not what makes this pass.
 *
 * The mapped-ness ASSERTION is still load-bearing on both backends -- no
 * other subtest anywhere checks that a popover actually maps, so a future
 * regression that left every popover unmapped would be caught here and
 * nowhere else. But be honest about which half is working: on CI this
 * subtest guards the property, and on a developer's Wayland box it also
 * guards the presenting.
 *
 * Re-running that falsification needs a real Wayland backend, and it does NOT
 * need your session's -- a headless compositor gives one without a window ever
 * reaching your screen, which is otherwise the reason to stay in Xvfb:
 *
 *   export XDG_RUNTIME_DIR=$(mktemp -d)
 *   mutter --headless --virtual-monitor 1280x720 --wayland \
 *     --wayland-display=probe &
 *   WAYLAND_DISPLAY=probe GDK_BACKEND=wayland meson test -C build open_external
 */
static void
test_open_external_popup_really_maps(void) {
   reset_editors();
   set_named_editors(3);
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-oe-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg", "plain.jpg");
   char  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_file);
   drain_main(200);
   g_assert_true(present_and_map(p_win));

   fire(p_win, "win.open-external");
   g_assert_true(wait_for_popover_mapped(p_win));
   /* Still the popover the other subtests describe -- so this subtest also
    * proves their structural assertions hold on a really-presented window,
    * not just on the never-presented one they build. */
   GtkPopover *p_pop = find_open_external_popover(p_win);
   g_assert_nonnull(p_pop);
   g_assert_cmpint(popover_button_count(p_pop), ==, 3);
   g_assert_cmpstr(popover_button_label(p_pop, 0), ==, "1  GIMP");

   /* The first row really holds the keyboard focus, so the hotkeys the key
    * controller implements can reach it (dw0).
    *
    * Nothing in src/window.c focuses it: gtk_popover_show() ends with
    * gtk_widget_child_focus() for an autohide popover, and the builders rely
    * on that -- see "POPOVER KEYBOARD FOCUS" in src/window.c for the four
    * dead gtk_widget_grab_focus() calls this replaced and why they never ran.
    * The assertion exists so that reliance is checked rather than assumed.
    *
    * It belongs in THIS subtest and not in popup_structure, because
    * gtk_popover_show() reaches that arm only after present_popup()
    * succeeds: on Wayland a popup whose parent was never presented does not
    * map and no focus is taken, which is exactly the toplevel the other
    * popover subtests build. This one presents (present_and_map above), so
    * the assertion holds on both backends. */
   /* cmphex on guintptr, not g_assert_true(a == b), so a failure prints both
    * pointers instead of just "should be TRUE". There is no g_assert_cmpptr
    * in glib (2.88.2 has cmpint/cmpuint/cmphex/cmpstr/cmpstrv/cmpfloat/
    * cmpmem/cmpvariant and no pointer variant); cmphex is the one that takes
    * these. */
   g_assert_cmphex((guintptr)gtk_root_get_focus(GTK_ROOT(p_win)), ==,
                   (guintptr)popover_button(p_pop, 0));

   /* Closing a popover that holds the focus releases it AT ONCE (gg2).
    * With the focus inside, a bare unparent makes GtkWindow keep a ref on
    * the unrealized popover until its next after-paint; on GTK 4.14 a
    * tooltip timeout in that gap passes the popover's NULL surface to
    * gdk_surface_get_device_position() -- the critical that failed CI's
    * fedora:40 lane in popup_structure and enhance_flow. popup_list_delete()
    * moves the focus out first, so no such ref is parked; its own ref (held
    * so a close from inside gtk_popover_popdown() cannot free the popover
    * under GTK) is dropped in a G_PRIORITY_HIGH idle, which runs before any
    * default-priority tooltip timeout. So by the time the main loop first
    * dispatches a G_PRIORITY_DEFAULT source after the close, the popover
    * must be finalized. */
   g_object_add_weak_pointer(G_OBJECT(p_pop), (gpointer *)&p_pop);
   fire(p_win, "win.open-external"); /* toggle closed */
   run_until_default_priority();
   g_assert_null(p_pop);
   g_assert_null(find_open_external_popover(p_win));

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   g_free(c_path);
   cleanup_temp_dir(c_dir);
   reset_editors();
}

/* --- closing: popover lifetime and where the focus goes (gg2) ----------- */

/* A presented window on a temp copy of plain.jpg with u_editors editors
 * configured, for the subtests below that close a MAPPED popover whose first
 * row holds the focus. Presented (like popup_really_maps) so the popover maps
 * and takes the focus on Wayland as well as on X11. */
typedef struct {
   char        *c_dir;
   GFile       *p_file;
   GgazeWindow *p_win;
} MappedFixture;

static void
mapped_fixture_open(MappedFixture *p_fx, guint u_editors) {
   reset_editors();
   set_named_editors(u_editors);
   GError *p_err = NULL;
   p_fx->c_dir   = g_dir_make_tmp("ggaze-oe-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(p_fx->c_dir, "plain.jpg", "plain.jpg");
   char *c_path = g_build_filename(p_fx->c_dir, "plain.jpg", NULL);
   p_fx->p_file = g_file_new_for_path(c_path);
   g_free(c_path);
   p_fx->p_win = new_window();
   ggaze_window_open(p_fx->p_win, p_fx->p_file);
   drain_main(200);
   g_assert_true(present_and_map(p_fx->p_win));
}

static void
mapped_fixture_close(MappedFixture *p_fx) {
   g_object_unref(p_fx->p_file);
   gtk_window_destroy(GTK_WINDOW(p_fx->p_win));
   drain_main(300);
   cleanup_temp_dir(p_fx->c_dir);
   reset_editors();
}

/* Pop up the `e` popover, wait for it to map with its first row focused, and
 * return it with a weak pointer installed at *pp_pop (so the caller can see
 * it finalized). */
static void
open_mapped_popover(GgazeWindow *p_win, GtkPopover **pp_pop) {
   fire(p_win, "win.open-external");
   g_assert_true(wait_for_popover_mapped(p_win));
   *pp_pop = find_open_external_popover(p_win);
   g_assert_nonnull(*pp_pop);
   g_assert_cmphex((guintptr)gtk_root_get_focus(GTK_ROOT(p_win)), ==,
                   (guintptr)popover_button(*pp_pop, 0));
   g_object_add_weak_pointer(G_OBJECT(*pp_pop), (gpointer *)pp_pop);
}

/* After a close: the popover is finalized before the main loop dispatches
 * anything at G_PRIORITY_DEFAULT -- a tooltip timeout, say (the teardown's
 * reference is dropped in a G_PRIORITY_HIGH idle, see popup_list.c) --, no
 * popover is left on the stack, and the keyboard focus is
 * back on the viewer -- where it was when the popover opened -- and not on
 * a header button, where Enter/Space would activate it. */
static void
assert_closed_to_viewer(GgazeWindow *p_win, GtkPopover **pp_pop) {
   run_until_default_priority();
   g_assert_null(*pp_pop);
   g_assert_null(find_open_external_popover(p_win));
   g_assert_cmphex((guintptr)gtk_root_get_focus(GTK_ROOT(p_win)), ==,
                   (guintptr)ggtest_viewer_of(p_win));
}

/* Deliver u_keyval to p_pop's key controller as a key press. */
static void
press_key(GtkPopover *p_pop, guint u_keyval) {
   GListModel *p_ctrls = gtk_widget_observe_controllers(GTK_WIDGET(p_pop));
   gboolean    b_sent  = FALSE;
   for (guint i = 0; i < g_list_model_get_n_items(p_ctrls) && !b_sent; i++) {
      GObject *p_obj = g_list_model_get_item(p_ctrls, i);
      if (GTK_IS_EVENT_CONTROLLER_KEY(p_obj)) {
         gboolean b_ret = FALSE;
         g_signal_emit_by_name(p_obj, "key-pressed", u_keyval, 0,
                               (GdkModifierType)0, &b_ret);
         g_assert_true(b_ret); /* the popover consumed it */
         b_sent = TRUE;
      }
      g_object_unref(p_obj);
   }
   g_object_unref(p_ctrls);
   g_assert_true(b_sent);
}

/* Every way the user closes a mapped chooser -- the toggle key, Escape, a
 * row picked by hotkey or by click -- frees the popover promptly and returns
 * the focus to the viewer (GTK hiding it on its own: the next subtest). */
static void
test_open_external_close_paths_restore_focus(void) {
   MappedFixture s_fx;
   mapped_fixture_open(&s_fx, 3);
   GtkPopover *p_pop = NULL;

   open_mapped_popover(s_fx.p_win, &p_pop);
   fire(s_fx.p_win, "win.open-external"); /* toggle */
   assert_closed_to_viewer(s_fx.p_win, &p_pop);

   open_mapped_popover(s_fx.p_win, &p_pop);
   press_key(p_pop, GDK_KEY_Escape);
   assert_closed_to_viewer(s_fx.p_win, &p_pop);

   open_mapped_popover(s_fx.p_win, &p_pop);
   press_key(p_pop, GDK_KEY_1); /* launches `true <file>`: harmless */
   assert_closed_to_viewer(s_fx.p_win, &p_pop);

   open_mapped_popover(s_fx.p_win, &p_pop);
   ggtest_click_button(popover_button(p_pop, 1));
   assert_closed_to_viewer(s_fx.p_win, &p_pop);

   mapped_fixture_close(&s_fx);
}

/* The first GtkButton in p_w's subtree (depth first) that takes the focus
 * -- which it then has --, or NULL. */
static GtkWidget *
focus_first_button_in(GtkWidget *p_w) {
   if (GTK_IS_BUTTON(p_w) && gtk_widget_grab_focus(p_w)) {
      return (p_w);
   }
   for (GtkWidget *p_c = gtk_widget_get_first_child(p_w); p_c != NULL;
        p_c            = gtk_widget_get_next_sibling(p_c)) {
      GtkWidget *p_b = focus_first_button_in(p_c);
      if (p_b != NULL) {
         return (p_b);
      }
   }
   return (NULL);
}

/* Closing returns the focus to wherever it was when the chooser opened, not
 * to a fixed widget: opened from a focused header button, Escape puts the
 * focus back on that button. And with no focus at all when it opened, the
 * close falls back to the stack's visible page -- the viewer -- rather than
 * the window's first focusable widget (the header's "Previous image"). */
static void
test_open_external_close_restores_prior_focus(void) {
   MappedFixture s_fx;
   mapped_fixture_open(&s_fx, 3);
   GtkWidget *p_hdr_btn =
      focus_first_button_in(gtk_window_get_titlebar(GTK_WINDOW(s_fx.p_win)));
   g_assert_nonnull(p_hdr_btn);
   GtkPopover *p_pop = NULL;
   open_mapped_popover(s_fx.p_win, &p_pop);
   press_key(p_pop, GDK_KEY_Escape);
   run_until_default_priority();
   g_assert_null(p_pop);
   g_assert_cmphex((guintptr)gtk_root_get_focus(GTK_ROOT(s_fx.p_win)), ==,
                   (guintptr)p_hdr_btn);

   gtk_root_set_focus(GTK_ROOT(s_fx.p_win), NULL);
   open_mapped_popover(s_fx.p_win, &p_pop);
   press_key(p_pop, GDK_KEY_Escape);
   assert_closed_to_viewer(s_fx.p_win, &p_pop);
   mapped_fixture_close(&s_fx);
}

/* GTK hiding a mapped chooser on its own (gtk_popover_popdown(), what
 * GtkPopover does itself when it loses its grab or cannot be placed) frees
 * it safely and returns the focus to the viewer.
 *
 * This is the gg2 use-after-free: "closed" fires INSIDE
 * gtk_popover_popdown(), the teardown unparents the popover there, and
 * gtk_popover_popdown() then touches it again (cascade_popdown). Unless the
 * teardown holds a reference across that, the last one goes with the
 * unparent and GTK reads freed memory -- an ASan report, or the
 * Gtk-CRITICAL "gtk_popover_get_autohide: assertion 'GTK_IS_POPOVER
 * (popover)' failed" on the dead instance. */
static void
test_open_external_gtk_popdown_frees_safely(void) {
   MappedFixture s_fx;
   mapped_fixture_open(&s_fx, 3);
   GtkPopover *p_pop = NULL;
   open_mapped_popover(s_fx.p_win, &p_pop);
   gtk_popover_popdown(p_pop);
   assert_closed_to_viewer(s_fx.p_win, &p_pop);
   mapped_fixture_close(&s_fx);
}

/* The full 36-row chooser (every hotkey in use) on a small window: taller
 * than the room above its anchor, which is where GTK hides a popover by
 * itself -- the path that first showed the gg2 use-after-free. Whether GTK
 * keeps it open depends on the display, so the subtest lets the popover
 * settle, closes it by the toggle key if it is still up, and asserts the
 * same outcome either way: no crash, no popover left, focus on the viewer. */
static void
test_open_external_tall_list_closes_cleanly(void) {
   MappedFixture s_fx;
   mapped_fixture_open(&s_fx, 36);
   fire(s_fx.p_win, "win.open-external");
   GtkPopover *p_pop = find_open_external_popover(s_fx.p_win);
   g_assert_nonnull(p_pop);
   g_assert_cmpint(popover_button_count(p_pop), ==, 36);
   g_object_add_weak_pointer(G_OBJECT(p_pop), (gpointer *)&p_pop);
   drain_main(300);
   if (p_pop != NULL) {
      g_test_message("GTK kept the 36-row popover open: toggling it closed");
      fire(s_fx.p_win, "win.open-external");
   } else {
      g_test_message("GTK hid the 36-row popover itself");
   }
   assert_closed_to_viewer(s_fx.p_win, &p_pop);
   mapped_fixture_close(&s_fx);
}

/* Preferences edits to the editors list apply live: with the popover's
 * window already open and showing 3 editors, changing the editors setting
 * (via a separate Settings wrapper sharing the memory backend) and re-
 * firing `e` shows the new list WITHOUT a restart. Exercises the window's
 * GSettings "changed::editors" handler re-running _load_engine_lists. */
static void
test_open_external_prefs_apply_live(void) {
   reset_editors();
   set_named_editors(3);
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-oe-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg", "plain.jpg");
   char  *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_file = g_file_new_for_path(c_path);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_file);
   drain_main(200);

   /* Baseline: 3 editors. */
   fire(p_win, "win.open-external");
   drain_main(200);
   GtkPopover *p_pop = find_open_external_popover(p_win);
   g_assert_nonnull(p_pop);
   g_assert_cmpint(popover_button_count(p_pop), ==, 3);
   fire(p_win, "win.open-external"); /* close */
   drain_main(200);
   g_assert_null(find_open_external_popover(p_win));

   /* Change editors to 1 WITHOUT restarting the window. */
   reset_editors();
   set_named_editors(1);
   drain_main(300); /* let changed::editors re-load the engine */

   /* Re-fire: the popover now reflects the single editor, live. */
   fire(p_win, "win.open-external");
   drain_main(200);
   p_pop = find_open_external_popover(p_win);
   g_assert_nonnull(p_pop);
   g_assert_cmpint(popover_button_count(p_pop), ==, 1);
   g_assert_cmpstr(popover_button_label(p_pop, 0), ==, "1  GIMP");
   fire(p_win, "win.open-external"); /* close */
   drain_main(200);

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   drain_main(300);
   g_free(c_path);
   cleanup_temp_dir(c_dir);
   reset_editors();
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
   g_test_add_func("/open_external/launches_capture",
                   test_open_external_launches_capture);
   g_test_add_func("/open_external/empty_no_editors",
                   test_open_external_empty_no_editors);
   g_test_add_func("/open_external/out_of_range",
                   test_open_external_out_of_range);
   g_test_add_func("/open_external/popup_structure",
                   test_open_external_popup_structure);
   g_test_add_func("/open_external/popup_empty_message",
                   test_open_external_popup_empty_message);
   g_test_add_func("/open_external/popup_really_maps",
                   test_open_external_popup_really_maps);
   g_test_add_func("/open_external/close_paths_restore_focus",
                   test_open_external_close_paths_restore_focus);
   g_test_add_func("/open_external/close_restores_prior_focus",
                   test_open_external_close_restores_prior_focus);
   g_test_add_func("/open_external/gtk_popdown_frees_safely",
                   test_open_external_gtk_popdown_frees_safely);
   g_test_add_func("/open_external/tall_list_closes_cleanly",
                   test_open_external_tall_list_closes_cleanly);
   g_test_add_func("/open_external/prefs_apply_live",
                   test_open_external_prefs_apply_live);
   return (g_test_run());
}