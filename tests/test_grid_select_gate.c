/*:*
 * ggaze — grid select-gate regression test (tu0 review round 2, issue 1)
 *
 * gridview.c's four navigator.current-changing call sites (double-click/
 * Enter via "child-activated", middle-click mark, j/k cursor move, and
 * toggle-to-large sync) used to call navigator_set_current_file() directly.
 * That bypassed any window-level dirty-preview gate: navigator_set_current_
 * file() emits "changed" synchronously, and window.c's nav_changed_cb could
 * silently discard an active unsaved GEGL enhance preview before the window
 * ever got a chance to prompt Save/Discard/Cancel. Repro: enhance an image,
 * switch to the grid, click a different thumbnail -> the preview vanished
 * with no prompt.
 *
 * The fix adds a GgazeGridSelectFunc gate (gridview.h ggaze_grid_set_select_
 * func) that every one of those call sites now routes through instead of
 * navigator_set_current_file() directly; window.c installs a real gate
 * (_grid_select_gate) that defers behind the Save/Discard/Cancel prompt
 * when an enhance preview is dirty.
 *
 * This test exercises gridview.c's side of the fix in isolation -- no GEGL,
 * no GgazeWindow, no dialog involved at all -- by installing a fake gate
 * function and asserting gridview.c actually calls it (instead of falling
 * back to navigator_set_current_file() on its own) and respects its verdict
 * both ways (refuse -> navigator.current does not move; allow -> it does).
 * The real window-level integration (an actual dirty GEGL preview blocking
 * the change) is covered by tests/test_enhance_flow.c's
 * grid_select_gates_dirty_enhance, gated on GEGL being built in.
 *
 * Needs a display (GgazeGrid is a GtkWidget); CI runs this under xvfb, and
 * it skips cleanly when no display is available. All but one subtest avoid
 * *presenting* a toplevel: selection is driven through the flowbox's
 * "child-activated" signal, which needs no laid-out geometry, which keeps
 * them off GTK's realize path entirely -- realizing a toplevel here pulled in
 * the AT-SPI bridge and a Wayland surface, both of which leak / dangle under
 * ASan for reasons that have nothing to do with ggaze. The exception is
 * move_cursor_respects_refusing_gate: ggaze_grid_move_cursor compares laid-out
 * cell bounds, so it cannot work on an unrealized grid, and that call site
 * would otherwise stay untested.
 *
 * The fourth gated call site, the middle-click mark, is now driven too --
 * through ggaze_grid_mark_at_pos(), the seam gridview.c grew for exactly this
 * (round 4). The GtkGestureClick "pressed" handler itself really is out of
 * reach: GTK4 dropped gtk_test_widget_click and the public GdkEvent
 * constructors GTK3 tests used to synthesize a pointer press. What it does is
 * filter the button and forward the coordinates, and everything after that
 * lives in the seam, where mark_at_pos_respects_refusing_gate exercises it at
 * real laid-out coordinates.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "gridview.h"
#include "gtk_helpers.h"
#include "navigator.h"
#include "thumbnail.h"

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

/* --- helpers -------------------------------------------------------------
 */

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

static char *
make_folder(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-grid-gate-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   copy_fixture(c_dir, "rot6.jpg");
   copy_fixture(c_dir, "small.png");
   return (c_dir);
}

/* Fake select-gate: records every file it is asked about and either refuses
 * (b_allow == FALSE, simulating "deferred behind an unresolved dirty
 * prompt") or actually applies the change via navigator_set_current_file()
 * (b_allow == TRUE), mirroring window.c's real _grid_select_gate without
 * needing GEGL or a real dialog. */
typedef struct {
   guint      u_calls;
   GFile     *p_last; /* borrowed: last file the gate was asked about */
   Navigator *p_nav;  /* borrowed */
   gboolean   b_allow;
} FakeGate;

static gboolean
fake_gate(GgazeGrid *p_grid, GFile *p_file, gpointer p_data) {
   (void)p_grid;
   FakeGate *p_fg = (FakeGate *)p_data;
   p_fg->u_calls++;
   p_fg->p_last = p_file;
   if (!p_fg->b_allow) {
      return (FALSE); /* refuse: navigator.current must not move */
   }
   return (navigator_set_current_file(p_fg->p_nav, p_file));
}

/* Build a Navigator + GgazeGrid pair over a fresh 3-file folder, parented in
 * a plain GtkWindow (not GgazeWindow -- no window.c/GEGL/dialog involved).
 * The window is deliberately NEVER presented: cells exist as soon as the
 * grid syncs from the navigator (same assumption tests/test_grid_cull.c
 * relies on), and realizing a toplevel here would only buy geometry we do
 * not need -- see ggtest_activate_cell (tests/helpers/gtk_helpers.h). */
static void
build_grid(char *c_dir, GtkWindow **pp_win, Navigator **pp_nav,
           Thumbnail **pp_thumb, GgazeGrid **pp_grid) {
   GFile *p_dir = g_file_new_for_path(c_dir);
   *pp_nav      = navigator_new(p_dir, GGAZE_SORT_NAME, FALSE, FALSE);
   g_object_unref(p_dir);
   *pp_thumb = thumbnail_new();
   *pp_grid  = GGAZE_GRID(ggaze_grid_new(*pp_nav, *pp_thumb, 128, FALSE));

   *pp_win = GTK_WINDOW(gtk_window_new());
   gtk_window_set_child(*pp_win, GTK_WIDGET(*pp_grid));
   ggtest_drain_main(100);
   g_assert_cmpuint(ggaze_grid_get_count(*pp_grid), ==, 3);
}

/* --- subtests --------------------------------------------------------------
 */

/* A refusing gate must block a cell activation from changing
 * navigator.current, proving gridview.c actually asks the installed gate
 * instead of calling navigator_set_current_file() on its own (the exact bug
 * this fixes: before it, there was no gate to ask). The other three call
 * sites -- middle-click mark, j/k cursor move, toggle-to-large sync -- share
 * the same one-line _grid_select() helper; j/k gets its own subtest below,
 * toggle-to-large is covered end to end by tests/test_enhance_flow.c. */
static void
test_activate_respects_refusing_gate(void) {
   char      *c_dir = make_folder();
   GtkWindow *p_win;
   Navigator *p_nav;
   Thumbnail *p_thumb;
   GgazeGrid *p_grid;
   build_grid(c_dir, &p_win, &p_nav, &p_thumb, &p_grid);

   GFile *p_before = navigator_get_current(p_nav);
   g_assert_nonnull(p_before);
   char *c_before = g_file_get_basename(p_before);

   FakeGate fg = {0};
   fg.p_nav    = p_nav;
   fg.b_allow  = FALSE;
   ggaze_grid_set_select_func(p_grid, fake_gate, &fg);

   ggtest_activate_cell(p_grid, 2); /* last cell: never the current one */
   ggtest_drain_main(50);

   g_assert_cmpuint(fg.u_calls, >, 0); /* the gate WAS asked ... */
   g_assert_nonnull(fg.p_last);
   g_assert_false(g_file_equal(fg.p_last, p_before)); /* ... about a real,
                                                       * different target */
   GFile *p_after = navigator_get_current(p_nav);
   char  *c_after = g_file_get_basename(p_after);
   g_assert_cmpstr(c_before, ==, c_after); /* but current did not move */

   g_free(c_before);
   g_free(c_after);
   ggaze_grid_detach(p_grid);
   gtk_window_destroy(p_win);
   thumbnail_delete(p_thumb);
   navigator_delete(p_nav);
   ggtest_drain_main(100);
   cleanup_temp_dir(c_dir);
}

/* An allowing gate must let the very same activation through -- otherwise
 * the refusing test above would be tautological (nothing ever moves, gate or
 * no gate). */
static void
test_activate_allows_when_gate_allows(void) {
   char      *c_dir = make_folder();
   GtkWindow *p_win;
   Navigator *p_nav;
   Thumbnail *p_thumb;
   GgazeGrid *p_grid;
   build_grid(c_dir, &p_win, &p_nav, &p_thumb, &p_grid);

   GFile *p_before = navigator_get_current(p_nav);
   char  *c_before = g_file_get_basename(p_before);

   FakeGate fg = {0};
   fg.p_nav    = p_nav;
   fg.b_allow  = TRUE;
   ggaze_grid_set_select_func(p_grid, fake_gate, &fg);

   ggtest_activate_cell(p_grid, 2);
   ggtest_drain_main(50);

   g_assert_cmpuint(fg.u_calls, >, 0);
   GFile *p_after = navigator_get_current(p_nav);
   char  *c_after = g_file_get_basename(p_after);
   g_assert_cmpstr(c_before, !=, c_after); /* current DID move this time */

   g_free(c_before);
   g_free(c_after);
   ggaze_grid_detach(p_grid);
   gtk_window_destroy(p_win);
   thumbnail_delete(p_thumb);
   navigator_delete(p_nav);
   ggtest_drain_main(100);
   cleanup_temp_dir(c_dir);
}

/* No gate installed at all (the default, and what every non-GgazeWindow
 * embedder gets): _grid_select() must fall back to calling
 * navigator_set_current_file() itself. Untested until now (round 2's test-
 * quality section), and the fallback is what keeps gridview.c usable
 * standalone -- a typo there would silently make the grid inert rather than
 * fail loudly. */
static void
test_activate_without_gate_moves_current(void) {
   char      *c_dir = make_folder();
   GtkWindow *p_win;
   Navigator *p_nav;
   Thumbnail *p_thumb;
   GgazeGrid *p_grid;
   build_grid(c_dir, &p_win, &p_nav, &p_thumb, &p_grid);

   char *c_before = g_file_get_basename(navigator_get_current(p_nav));

   ggtest_activate_cell(p_grid, 2); /* no ggaze_grid_set_select_func call */
   ggtest_drain_main(50);

   char *c_after = g_file_get_basename(navigator_get_current(p_nav));
   g_assert_cmpstr(c_before, !=, c_after);
   g_assert_cmpstr(c_after, ==, "small.png"); /* the cell actually asked for */

   g_free(c_before);
   g_free(c_after);
   ggaze_grid_detach(p_grid);
   gtk_window_destroy(p_win);
   thumbnail_delete(p_thumb);
   navigator_delete(p_nav);
   ggtest_drain_main(100);
   cleanup_temp_dir(c_dir);
}

/* Clearing the gate again (fn == NULL) must restore that fallback rather than
 * leave the grid pointing at a stale callback -- the shape window.c relies on
 * when a grid outlives the window that installed a gate on it. */
static void
test_clearing_gate_restores_fallback(void) {
   char      *c_dir = make_folder();
   GtkWindow *p_win;
   Navigator *p_nav;
   Thumbnail *p_thumb;
   GgazeGrid *p_grid;
   build_grid(c_dir, &p_win, &p_nav, &p_thumb, &p_grid);

   FakeGate fg = {0};
   fg.p_nav    = p_nav;
   fg.b_allow  = FALSE;
   ggaze_grid_set_select_func(p_grid, fake_gate, &fg);
   ggaze_grid_set_select_func(p_grid, NULL, NULL); /* uninstall again */

   char *c_before = g_file_get_basename(navigator_get_current(p_nav));
   ggtest_activate_cell(p_grid, 2);
   ggtest_drain_main(50);
   char *c_after = g_file_get_basename(navigator_get_current(p_nav));

   g_assert_cmpuint(fg.u_calls, ==, 0);    /* the removed gate stayed quiet */
   g_assert_cmpstr(c_before, !=, c_after); /* ... and current moved anyway */

   g_free(c_before);
   g_free(c_after);
   ggaze_grid_detach(p_grid);
   gtk_window_destroy(p_win);
   thumbnail_delete(p_thumb);
   navigator_delete(p_nav);
   ggtest_drain_main(100);
   cleanup_temp_dir(c_dir);
}

/* Lay the grid out for real so the geometry-driven call sites work: j/k
 * (ggaze_grid_move_cursor) compares laid-out cell bounds and gives up while
 * they are all zero-sized. Deliberately narrow so the three cells wrap into
 * three rows and "one row down" has somewhere to go. Returns once the
 * flowbox has a non-degenerate allocation. */
static void
present_and_lay_out(GtkWindow *p_win) {
   gtk_window_set_default_size(p_win, 200, 600);
   gtk_window_present(p_win);
   for (guint u = 0; u < 2000; u++) {
      GtkWidget *p_child = gtk_window_get_child(p_win);
      if (p_child != NULL && gtk_widget_get_width(p_child) > 0) {
         return;
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_not_reached();
}

/* j/k cursor movement is one of the four gated call sites, and the only one
 * this suite could not reach before (round 3's "still untested" list): it
 * needs a laid-out grid, so this subtest is the one that presents. A refusing
 * gate must block it exactly like it blocks a double-click. */
static void
test_move_cursor_respects_refusing_gate(void) {
   char      *c_dir = make_folder();
   GtkWindow *p_win;
   Navigator *p_nav;
   Thumbnail *p_thumb;
   GgazeGrid *p_grid;
   build_grid(c_dir, &p_win, &p_nav, &p_thumb, &p_grid);
   present_and_lay_out(p_win);

   char    *c_before = g_file_get_basename(navigator_get_current(p_nav));
   FakeGate fg       = {0};
   fg.p_nav          = p_nav;
   fg.b_allow        = FALSE;
   ggaze_grid_set_select_func(p_grid, fake_gate, &fg);

   ggaze_grid_move_cursor(p_grid, +1); /* `j` */
   ggtest_drain_main(50);

   g_assert_cmpuint(fg.u_calls, >, 0); /* the gate WAS asked ... */
   char *c_after = g_file_get_basename(navigator_get_current(p_nav));
   g_assert_cmpstr(c_before, ==, c_after); /* ... and current did not move */

   /* Control, so the assertion above cannot pass merely because the cursor
    * had nowhere to go: the very same move, allowed, does move current. */
   fg.b_allow = TRUE;
   ggaze_grid_move_cursor(p_grid, +1);
   ggtest_drain_main(50);
   char *c_moved = g_file_get_basename(navigator_get_current(p_nav));
   g_assert_cmpstr(c_before, !=, c_moved);

   g_free(c_before);
   g_free(c_after);
   g_free(c_moved);
   ggaze_grid_detach(p_grid);
   gtk_window_destroy(p_win);
   thumbnail_delete(p_thumb);
   navigator_delete(p_nav);
   ggtest_drain_main(100);
   cleanup_temp_dir(c_dir);
}

/* Find the GtkFlowBox inside the grid widget (grid -> scrolled window ->
 * viewport -> flowbox). ggaze_grid_mark_at_pos() takes coordinates in the
 * flowbox's own space, and the test has to compute them from the real layout
 * rather than guess: cell size and wrapping are GtkFlowBox's business. */
static GtkWidget *
find_flowbox(GtkWidget *p_root) {
   if (GTK_IS_FLOW_BOX(p_root)) {
      return (p_root);
   }
   for (GtkWidget *p_c = gtk_widget_get_first_child(p_root); p_c != NULL;
        p_c            = gtk_widget_get_next_sibling(p_c)) {
      GtkWidget *p_hit = find_flowbox(p_c);
      if (p_hit != NULL) {
         return (p_hit);
      }
   }
   return (NULL);
}

/* Centre of the u_idx'th cell, in flowbox coordinates. */
static void
cell_centre(GtkWidget *p_flow, guint u_idx, gint *pi_x, gint *pi_y) {
   GtkWidget *p_child = gtk_widget_get_first_child(p_flow);
   for (guint u = 0; u < u_idx && p_child != NULL; u++) {
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   g_assert_nonnull(p_child);
   graphene_rect_t t_r;
   g_assert_true(gtk_widget_compute_bounds(p_child, p_flow, &t_r));
   g_assert_cmpfloat(t_r.size.width, >, 0.0f);
   *pi_x = (gint)(t_r.origin.x + t_r.size.width / 2.0f);
   *pi_y = (gint)(t_r.origin.y + t_r.size.height / 2.0f);
}

/* The middle-click mark, the last of the four gated call sites and the one
 * named as untested in every review round so far. Driven through the seam
 * gridview.c exposes for it (ggaze_grid_mark_at_pos), at the real laid-out
 * coordinates of a cell -- so the flowbox hit-test is exercised too, not just
 * the gate call. A refusing gate must block the navigator.current sync exactly
 * as it blocks a double-click; the mark itself is a separate concern (a
 * "win.mark" action this standalone grid has no window to handle). */
static void
test_mark_at_pos_respects_refusing_gate(void) {
   char      *c_dir = make_folder();
   GtkWindow *p_win;
   Navigator *p_nav;
   Thumbnail *p_thumb;
   GgazeGrid *p_grid;
   build_grid(c_dir, &p_win, &p_nav, &p_thumb, &p_grid);
   present_and_lay_out(p_win);

   GtkWidget *p_flow = find_flowbox(GTK_WIDGET(p_grid));
   g_assert_nonnull(p_flow);
   gint i_x, i_y;
   cell_centre(p_flow, 2, &i_x, &i_y); /* last cell: never the current one */

   char    *c_before = g_file_get_basename(navigator_get_current(p_nav));
   FakeGate fg       = {0};
   fg.p_nav          = p_nav;
   fg.b_allow        = FALSE;
   ggaze_grid_set_select_func(p_grid, fake_gate, &fg);

   g_assert_true(ggaze_grid_mark_at_pos(p_grid, i_x, i_y)); /* a cell WAS hit */
   ggtest_drain_main(50);

   g_assert_cmpuint(fg.u_calls, >, 0); /* the gate WAS asked ... */
   g_assert_nonnull(fg.p_last);
   char *c_asked = g_file_get_basename(fg.p_last);
   g_assert_cmpstr(c_asked, ==, "small.png"); /* ... about that very cell */
   char *c_after = g_file_get_basename(navigator_get_current(p_nav));
   g_assert_cmpstr(c_before, ==, c_after); /* and current did not move */

   /* Control: the same position, allowed, does move current -- so the
    * assertion above cannot pass merely because the hit-test missed. */
   fg.b_allow = TRUE;
   g_assert_true(ggaze_grid_mark_at_pos(p_grid, i_x, i_y));
   ggtest_drain_main(50);
   char *c_moved = g_file_get_basename(navigator_get_current(p_nav));
   g_assert_cmpstr(c_moved, ==, "small.png");

   g_free(c_before);
   g_free(c_after);
   g_free(c_asked);
   g_free(c_moved);
   ggaze_grid_detach(p_grid);
   gtk_window_destroy(p_win);
   thumbnail_delete(p_thumb);
   navigator_delete(p_nav);
   ggtest_drain_main(100);
   cleanup_temp_dir(c_dir);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);

   if (!gtk_init_check()) {
      g_test_skip("no display available (run under xvfb)");
      return (g_test_run());
   }

   g_test_add_func("/grid_select_gate/activate_respects_refusing_gate",
                   test_activate_respects_refusing_gate);
   g_test_add_func("/grid_select_gate/activate_allows_when_gate_allows",
                   test_activate_allows_when_gate_allows);
   g_test_add_func("/grid_select_gate/activate_without_gate_moves_current",
                   test_activate_without_gate_moves_current);
   g_test_add_func("/grid_select_gate/clearing_gate_restores_fallback",
                   test_clearing_gate_restores_fallback);
   g_test_add_func("/grid_select_gate/move_cursor_respects_refusing_gate",
                   test_move_cursor_respects_refusing_gate);
   g_test_add_func("/grid_select_gate/mark_at_pos_respects_refusing_gate",
                   test_mark_at_pos_respects_refusing_gate);
   return (g_test_run());
}
