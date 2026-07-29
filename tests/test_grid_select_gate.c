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
 * it skips cleanly when no display is available. No toplevel is ever
 * *presented*, though: selection is driven through the flowbox's
 * "child-activated" signal, which needs no laid-out geometry. That keeps
 * the test off GTK's realize path entirely -- realizing a toplevel here
 * pulled in the AT-SPI bridge and a Wayland surface, both of which leak /
 * dangle under ASan for reasons that have nothing to do with ggaze.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "gridview.h"
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

static void
drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
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
 * not need -- see activate_cell below. */
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
   drain_main(100);
   g_assert_cmpuint(ggaze_grid_get_count(*pp_grid), ==, 3);
}

/* Depth-first search for the GtkFlowBox GgazeGrid keeps inside its
 * GtkScrolledWindow. The grid does not expose it, and these tests need it in
 * order to emit "child-activated" (gridview.c's double-click/Enter selection
 * path -- exactly the "click a different thumbnail" repro from the review)
 * directly. That path needs no laid-out geometry, unlike
 * ggaze_grid_move_cursor, so nothing here has to present a real toplevel. */
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
 * flowbox emits "child-activated", which gridview.c's _on_child_activated
 * handles by routing the cell's file through the installed select gate. */
static void
activate_cell(GgazeGrid *p_grid, gint i_idx) {
   GtkFlowBox *p_flow = find_flow_box(GTK_WIDGET(p_grid));
   g_assert_nonnull(p_flow);
   GtkFlowBoxChild *p_child = gtk_flow_box_get_child_at_index(p_flow, i_idx);
   g_assert_nonnull(p_child);
   g_signal_emit_by_name(p_flow, "child-activated", p_child);
}

/* --- subtests --------------------------------------------------------------
 */

/* A refusing gate must block a cell activation from changing
 * navigator.current, proving gridview.c actually asks the installed gate
 * instead of calling navigator_set_current_file() on its own (the exact bug
 * this fixes: before it, there was no gate to ask). The other three call
 * sites -- middle-click mark, j/k cursor move, toggle-to-large sync -- share
 * the same one-line _grid_select() helper, so covering one covers the
 * mechanism; the geometry-driven ones would need a realized toplevel. */
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

   activate_cell(p_grid, 2); /* last cell: never the current one */
   drain_main(50);

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
   drain_main(100);
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

   activate_cell(p_grid, 2);
   drain_main(50);

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
   drain_main(100);
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
   return (g_test_run());
}
