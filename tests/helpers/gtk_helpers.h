/*:*
 * ggaze — shared GTK test helpers (AGENTS.md: "Shared helpers go in
 * tests/helpers/")
 *
 * Two groups, both needed by more than one suite:
 *
 *   - grid cell activation: reach the GtkFlowBox GgazeGrid keeps private
 *     inside its GtkScrolledWindow and emit "child-activated" on one of its
 *     cells (what a double-click / Enter on a thumbnail does). Needs no
 *     laid-out geometry, so callers never have to present a toplevel.
 *   - alert-dialog driving: find, count and press buttons on the dialogs
 *     gtk_alert_dialog_choose() puts up.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#ifndef GTK_HELPERS_H
#define GTK_HELPERS_H

#include <gtk/gtk.h>

#include "gridview.h"
#include "window.h"

/* --- grid cells ---------------------------------------------------------- */

/* Depth-first search for the GtkFlowBox inside p_w (GgazeGrid does not expose
 * its own). NULL if there is none. */
GtkFlowBox *ggtest_find_flow_box(GtkWidget *p_w);

/* Activate cell i_idx of p_grid exactly as a double-click / Enter on it
 * would: emits the flowbox's "child-activated", which gridview.c handles by
 * routing the cell's file through the installed select gate. Asserts the
 * flowbox and the cell exist. */
void ggtest_activate_cell(GgazeGrid *p_grid, gint i_idx);

/* --- window focus ---------------------------------------------------------
 *
 * Grab keyboard focus into p_win's GgazeViewer (its stack's "large" child),
 * so the toplevel has a focus widget. Asserts the grab succeeded.
 *
 * Why every suite that pops up one of the window's popovers needs this (5w0):
 *
 * A test builds its GgazeWindow with g_object_new() and never presents it, so
 * the toplevel is never mapped and never gains keyboard focus -- and GTK only
 * picks an initial focus widget when a window actually receives focus, so
 * gtk_root_get_focus() on such a window stays NULL. The `e`/`!`/`m` popovers
 * hold a single GtkLabel when nothing is configured, and a plain GtkLabel
 * cannot take focus (gtk_label_grab_focus() returns FALSE unless the label is
 * selectable or carries links), so that popover has no focusable descendant
 * either.
 *
 * Those two facts together walk gtk-4.22.4 into an unguarded NULL:
 * gtk_popover_show() ends with gtk_widget_child_focus() for an autohide
 * popover, and gtk_popover_focus() (gtk/gtkpopover.c) then does this. Note
 * the branch: it is NOT the "Empty popover" one -- that is :1106-1110, which
 * sees a popover with no first child and returns FALSE without ever touching
 * focus. Our popover HAS a child (the GtkLabel), so control goes to the
 * *else* branch at :1111 onwards: gtk_widget_focus_move() fails because the
 * label cannot take focus, and the autohide arm at :1119-1127 then calls
 *
 *    p = gtk_root_get_focus (gtk_widget_get_root (widget));   // NULL
 *    if (!gtk_widget_is_ancestor (p, widget) && p != widget)
 *
 * -- gtk_widget_is_ancestor() with a NULL first argument, a Gtk-CRITICAL,
 * which the suites' g_log_set_always_fatal() turns into an abort.
 *
 * It only aborts on the X11 backend, which is what CI's xvfb-run gives it
 * (.woodpecker/ci.yml). On Wayland gdk_popup_present() refuses a popup whose
 * parent surface is unmapped, so gtk_popover_show() returns before the focus
 * code runs -- which also means those popovers never actually map there and
 * the suites were only ever asserting on an unmapped widget tree.
 *
 * This helper fixes the ABORT, and only the abort. It does NOT itself close
 * the Wayland gap: measured with gtk_widget_get_mapped() from one binary
 * across three backends, the popover is mapped=1 on live X11 and on xvfb but
 * still mapped=0 on Wayland, with and without this grab -- because the
 * blocker there is the unmapped parent surface, not the missing focus widget.
 * So for a window a test never presents, a green Wayland run still says
 * nothing about presentation: mapping, positioning, autohide/grab behaviour,
 * dismissal.
 *
 * WHAT HAS SINCE BEEN CLOSED, AND WHAT HAS NOT (cw0, commit 9aeb902 -- whose
 * subject says "bw0", the deleted duplicate filing of the same task).
 *
 * Exactly ONE subtest presents: /open_external/popup_really_maps in
 * tests/test_open_external.c calls gtk_window_present(), waits for the
 * toplevel to map, fires win.open-external and asserts the popover is mapped.
 * With a mapped parent the popover maps on Wayland and on X11 alike, so it
 * needs no backend-conditional assertion. The other ~40 popover subtests were
 * deliberately left as they are -- they assert construction-time facts, which
 * are honest on both backends, and converting five suites to present would pop
 * real windows on a developer's desktop for coverage one subtest already buys.
 *
 * The residual, stated here so nobody has to re-derive it:
 *
 *   - MAPPING is the only one of the four presentation aspects listed above
 *     that anything asserts. Positioning is unasserted on both backends, and
 *     so is real dismissal: the popover's "closed" -> _open_ext_closed_cb ->
 *     _open_ext_destroy path is never driven, because popup_structure's
 *     toggle-close fires win.open-external a second time, which is the ACTION
 *     path, not the dismissal path.
 *   - Only ONE of the four popover builders in src/window.c is covered, the
 *     `e` one (_action_open_external). `a` (_enhance_build_rows /
 *     _enhance_build_box), `!` (_action_run_script) and `m` (_move_build_box)
 *     are not. All four share the same set_parent-onto-the-stack +
 *     gtk_popover_popup() shape, so the single guard does catch a GTK-version
 *     or shared-pattern regression -- but a regression confined to one of the
 *     other three (a dropped gtk_widget_set_parent, say) would leave that
 *     popover unmapped with nothing failing on EITHER backend.
 *
 * One consequence to know before touching the grabs in those builders:
 * gtk_popover_show() (gtk/gtkpopover.c:1151-1169 in gtk-4.22.4) only reaches
 * its gtk_widget_child_focus() arm after present_popup() has succeeded, so
 * presenting makes that arm run on Wayland for the first time -- in that one
 * subtest. It does NOT revive the builders' own gtk_widget_grab_focus() calls:
 * those run BEFORE gtk_widget_set_parent(), so the button has no GtkRoot
 * whether or not the toplevel was presented, and they stay no-ops either way.
 *
 * Verified on live Wayland, live Xwayland with GDK_BACKEND=x11, and xvfb; the
 * DRI3 warnings xvfb prints are unrelated (GSK_RENDERER=cairo, gl, ngl and
 * vulkan all fail identically without this). */
void ggtest_focus_viewer(GgazeWindow *p_win);

/* --- window teardown ------------------------------------------------------
 *
 * THE rule for every test in this suite (1w0), stated once here because the
 * suites are otherwise independent binaries: a GtkWindow a test built is torn
 * down with gtk_window_destroy(), never with a plain g_object_unref().
 *
 * gtk_window_constructed() appends the window to GTK's internal toplevel list,
 * which takes a reference of its own, and then drops the caller's initial one
 * -- so the list holds the window's only counted reference and a freshly built
 * window sits at refcount 1, properly referenced. Only gtk_window_destroy()
 * takes the entry back out of that list -- and it drops that reference while
 * doing so, so a window a test alone owns still finalizes exactly as the old
 * unref made it finalize. Unreffing *instead* steals the list's reference: the
 * window finalizes, the entry stays behind, and only then is the list left
 * pointing at freed memory.
 *
 * That stays invisible for as long as nothing walks the list. The list is a
 * GListModel, though, and every walk of it goes through
 * g_list_model_get_item(), which takes a TRANSIENT reference on each entry --
 * and that is the reference a freed window cannot survive. So after one
 * leaked window the next walk logs g_object_ref/g_object_unref G_IS_OBJECT
 * assertion failures (fatal under g_test) and yields a NULL entry, and a real
 * modal GtkAlertDialog, an adw_dialog_present() or a grab -- each of which
 * walks that same model -- touches the freed window and aborts under ASan.
 *
 * The GList that gtk_window_list_toplevels() returns is only transfer-
 * container ("The widgets in the list are not individually referenced" --
 * GTK4 docs): it drops each transient reference again before returning, so
 * callers free the list and must NOT unref the entries. Measured on
 * gtk4-4.22.4.
 *
 * Where a test needs the destroy to settle (pending idles, in-flight loads),
 * it iterates the main context afterwards -- ggtest_drain_main() below, or
 * the suite's own local drain.
 */

/* --- alert dialogs -------------------------------------------------------- */

/* Iterate the main context for roughly u_ms milliseconds. */
void ggtest_drain_main(guint u_ms);

/* Depth-first search for a GtkButton labelled c_label below p_root. */
GtkWidget *ggtest_find_button(GtkWidget *p_root, const char *c_label);

/* The first toplevel other than p_skip that carries a button labelled
 * c_label, i.e. the dialog gtk_alert_dialog_choose() opened. NULL if none is
 * up. p_skip is the caller's own window (its popovers are part of its widget
 * tree and could otherwise match). */
GtkWindow *ggtest_find_dialog(GtkWindow *p_skip, const char *c_label);

/* How many such dialogs are live at once -- the check for "a second trigger
 * must not stack a second dialog". */
guint ggtest_count_dialogs(GtkWindow *p_skip, const char *c_label);

/* Iterate the main context until such a dialog appears. Returns it, or NULL
 * once the wait ceiling expires.
 *
 * There is deliberately no per-call budget: every call site wanted the same
 * "as soon as it shows up, and give up eventually", and how long "eventually"
 * has to be is a property of the machine and the build (sanitizers, parallel
 * lane load), not of the call site. The ceiling and its scaling live in one
 * documented place in gtk_helpers.c; GGAZE_TEST_TIMEOUT_SCALE widens it. */
GtkWindow *ggtest_wait_for_dialog(GtkWindow *p_skip, const char *c_label);

/* Click p_btn by emitting "clicked" -- what a real pointer click ends in.
 * NOT gtk_widget_activate(): GtkButton turns that into a keyboard-activation
 * animation that only runs on a REALIZED widget and only emits "clicked" when
 * its timeout expires, so it silently does nothing for a button in a popover
 * of a window the test never presented. */
void ggtest_click_button(GtkWidget *p_btn);

/* TRUE while p_win is still a live toplevel. GTK4's gtk_window_destroy()
 * (which is also what an unhandled "close-request" ends in) hides and
 * unrealizes the window and takes it out of gtk_window_list_toplevels()
 * WITHOUT emitting anything the caller can watch for -- and it does not
 * finalize a window the test still holds a reference to -- so "did the
 * window actually close?" is asked this way. */
gboolean ggtest_is_open_toplevel(GtkWindow *p_win);

/* Press the button labelled c_label on the first such dialog, driving
 * gtk_alert_dialog_choose()'s async callback to completion. No response-
 * injection API is needed: the dialog is an ordinary GtkWindow, so finding its
 * button and driving it through ggtest_click_button above (which emits
 * "clicked" -- see there for why gtk_widget_activate() silently does nothing)
 * is enough. Waits for the dialog on the same ceiling as
 * ggtest_wait_for_dialog() above, so it does not race a prompt that is still
 * being raised; returns FALSE only if none turns up before then. Iterates the
 * main context while waiting, but not after the click -- the caller drains. */
gboolean ggtest_click_dialog_button(GtkWindow *p_skip, const char *c_label);

/* Wait for a dialog carrying a c_label button and return it; if none turns up,
 * g_error() out naming the toplevels that DO exist. Never returns NULL. Call
 * it through GGTEST_ASSERT_DIALOG_UP() below, which supplies c_loc.
 *
 * This exists because the bare g_assert_nonnull(ggtest_wait_for_dialog(...))
 * it replaces reported "should not be NULL" and nothing else, which cannot
 * tell the two halves of task 8w0's flake apart: a dialog that never appeared,
 * and one that appeared and then left the toplevel list again. Naming the
 * surviving toplevels separates them at the moment of failure -- a live
 * GtkMessageDialog (which is what GtkAlertDialog actually puts up) means the
 * button moved; none at all means the dialog is gone. 8w0 measured the second,
 * and only on a display with a window manager: the prompt was answered without
 * the test clicking anything, which GTK reported as an ordinary button index
 * and nothing else. See "A LIVE COMPOSITOR IS NOT A NEUTRAL DISPLAY EITHER" in
 * tests/meson.build.
 *
 * c_loc is the CALL SITE, which g_error() alone cannot give: it prints the
 * file and line of the g_error() itself, i.e. this helper. Most call sites
 * wait for the same "Cancel" button, so in a subtest that waits twice GLib's
 * TAP line narrows the failure to the subtest but no further.
 *
 * Shared home since dw0. test_enhance_flow.c and test_delete_safety.c each
 * carried a byte-identical private copy; the exposure differs between them
 * (delete_safety's fixture calls gtk_window_present(), so on a live session
 * its confirm has real keyboard focus and a real WM able to close it), but
 * the code did not, so it lives here once. */
GtkWindow *ggtest_assert_dialog_up_at(const char *c_loc, GtkWindow *p_own,
                                      const char *c_button);

#define GGTEST_ASSERT_DIALOG_UP(p_own, c_button)                               \
   ggtest_assert_dialog_up_at(G_STRLOC, (p_own), (c_button))

#endif /* GTK_HELPERS_H */
