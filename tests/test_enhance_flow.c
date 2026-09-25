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
 *     active, FALSE again once every preset is toggled back off) AND the
 *     saved flag: a preview `s` already exported is not dirty until the
 *     mask changes again, so moving on does not prompt for it.
 *   - `a` opens the enhance side panel beside the viewer (inside the window,
 *     no second toplevel), which survives navigation and re-previews the
 *     new image; Esc closes it and keeps the edit, x reverts (6i2: `0`
 *     is zoom only, the digits are the open panel's keys).
 *   - ggaze_window_set_hold_original() swaps the displayed texture to the
 *     (cached) original and back without touching u_enhance_mask.
 *   - the `i` card's histogram follows the picture (0c2, second review):
 *     it plots the preview, the original under hold-Space, the preview
 *     again on release, and the next preset's preview when that lands --
 *     bin-for-bin equal to histogram_new_from_texture() of the texture the
 *     viewer shows at each step (test_info_plots_preview).
 *   - win.enhance-save exports a NEW file next to the original
 *     (<stem>-enhanced[-<n>].<ext>, collision-suffixed like mover.c), never
 *     overwriting the original or a pre-existing same-named export.
 *   - ai2: the user's own presets are rows of the panel after the eight
 *     built-ins (j / k reach them, the list scrolls, digits stay 1-8), and
 *     a Preferences change of them carries the edit to their new rows
 *     (the "ai2" section).
 *   - wb2: the crop (c) / straighten (r, R before 6i2) / rotate-90 ([ ])
 *     tools on the same preview graph -- see the "wb2" section before the
 *     registrations; 6i2: the edit panel's modal keys and the key-hint bar
 *     (the "6i2" section).
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

#include "croprect.h"
#include "enhancer-gegl.h"
#include "enhance-ui.h"
#include "file_stamp.h"
#include "ggaze-config.h"
#include "gridview.h"
#include "gtk_helpers.h"
#include "histogram-view.h"
#include "histogram.h"
#include "icc.h"
#include "settings.h"
#include "temp_dir.h"
#include "transform.h"
#include "viewer.h"
#include "window.h"

#include <gdk/gdk.h>
#include <gegl.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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

/* Temp folders come down through ggtest_cleanup_temp_dir (tests/helpers/
 * temp_dir.h): recursive, because the trash subtests leave a "./.Trash"
 * folder (trash.c's lazy bin) inside the temp dir, and asserting the final
 * rmdir, so a subtest that leaves a file behind -- an export still landing,
 * a permission not restored -- fails here instead of littering $TMPDIR. */

static GdkTexture *
viewer_texture(GgazeWindow *p_win) {
   GtkStack  *p_stack = ggaze_window_get_stack(p_win);
   GtkWidget *p_large = gtk_stack_get_child_by_name(p_stack, "large");
   return (ggaze_viewer_get_texture(GGAZE_VIEWER(p_large)));
}

/* Take an OWNED reference on whatever the viewer is showing right now.
 *
 * Every texture pointer a subtest keeps across main-loop iterations goes
 * through here, because the identity comparisons those pointers exist for
 * ("still the same texture", "a different one now") only mean something
 * between two LIVE objects. Nothing the test controls keeps a displayed
 * texture alive: the holders are the viewer itself (ggaze_viewer_set_texture
 * g_set_object's it), the window's bounded LRU texture cache (cap 4), and --
 * for a preview -- the window's own p_enhance_tex. Once the viewer moves on
 * and the cache drops or evicts it, the object is finalized and comparing a
 * borrowed pointer is a bet on the allocator not reusing the address; task
 * 3w0 measured that reuse happening in 9 runs out of 15.
 *
 * The g_assert_nonnull is deliberate rather than defensive. viewer_texture()
 * returns NULL before the first load lands and whenever the navigator has no
 * current file (window.c _load_current) -- a cache-missing navigation is NOT
 * one of those cases, it leaves the previous texture on screen until the new
 * one arrives, which is why no subtest here has ever produced NULL. Should
 * one ever do so, g_object_ref(NULL) would abort all the same (main() makes
 * criticals fatal) but blame G_IS_OBJECT inside GLib; failing here names the
 * step that had no texture to take. */
static GdkTexture *
ref_viewer_texture(GgazeWindow *p_win) {
   GdkTexture *p_tex = viewer_texture(p_win);
   g_assert_nonnull(p_tex);
   return (g_object_ref(p_tex));
}

/* tests/fixtures/plain.jpg's decoded (and upright: orientation 1) size, which
 * every open of it below waits for. */
#define PLAIN_JPG_W 6
#define PLAIN_JPG_H 3

/* Wait until the viewer holds the i_w x i_h full decode of the file just
 * opened (gtk_helpers.h "large-view readiness"), then settle.
 *
 * The SIZE is the point (2d2). This used to wait for any non-NULL texture and
 * then drain a fixed 200 ms, but the JPEG backend shows a 1/8-scale preview
 * first -- 1x1 for plain.jpg -- so on a loaded lane the preview could still
 * be on screen when the drain ran out and be taken for the original (a
 * "p_orig" ref, a render base). The 200 ms drain stays as a SETTLE for the
 * idles that follow a load (info card, panel previews), not as the wait: the
 * assertions after it are the check. A timeout names this line, not the
 * caller's; GLib's TAP line names the subtest. */
static void
wait_for_load(GgazeWindow *p_win, gint i_w, gint i_h) {
   GGTEST_WAIT_FOR_TEXTURE(p_win, i_w, i_h);
   ggtest_drain_main(200);
}

/* wait_for_load() for a PRESENTED window: also waits for the viewer's
 * allocation to settle (GGTEST_WAIT_FOR_VIEW), which the drag subtests' widget
 * -> image pixel mapping and the panel's layout lean on. */
static void
wait_for_view(GgazeWindow *p_win, gint i_w, gint i_h) {
   GGTEST_WAIT_FOR_VIEW(p_win, i_w, i_h);
   ggtest_drain_main(200);
}

/* Pump until c_path exists (the enhance export runs in a worker now, so a
 * save's file lands a little after the key press), up to 10 s. */
static void
wait_for_file(const char *c_path) {
   for (guint u = 0; u < 10000 && !g_file_test(c_path, G_FILE_TEST_EXISTS);
        u++) {
      g_main_context_iteration(NULL, FALSE);
      g_usleep(1000);
   }
   g_assert_true(g_file_test(c_path, G_FILE_TEST_EXISTS));
}

/* Pump until the status line starts with c_prefix (up to 10 s) and return
 * the line as it is then (borrowed, valid until the next main-loop
 * iteration) WITHOUT asserting: a subtest that must undo something before
 * its first assertion can abort the process (a chmod, see
 * test_failed_render_ends_the_tool) polls here and asserts afterwards. */
static const char *
poll_for_status_prefix(GgazeWindow *p_win, const char *c_prefix) {
   GtkLabel *p_lbl = GTK_LABEL(ggaze_window_get_info_label(p_win));
   for (guint u = 0;
        u < 10000 && !g_str_has_prefix(gtk_label_get_text(p_lbl), c_prefix);
        u++) {
      g_main_context_iteration(NULL, FALSE);
      g_usleep(1000);
   }
   return (gtk_label_get_text(p_lbl));
}

/* Pump until the status line starts with c_prefix (up to 10 s): the export
 * that a Save answer starts runs in a worker, and under load its failure
 * report can land well after the dialog closed. */
static void
wait_for_status_prefix(GgazeWindow *p_win, const char *c_prefix) {
   g_assert_true(
      g_str_has_prefix(poll_for_status_prefix(p_win, c_prefix), c_prefix));
}

/* The status line right now (defined with the tool helpers below; the
 * hold-Space subtest above them reads it too). */
static const char *status_text(GgazeWindow *p_win);

static void
fire(GgazeWindow *p_win, const char *c_action) {
   gtk_widget_activate_action(GTK_WIDGET(p_win), c_action, NULL);
}

static GtkWidget *find_panel(GgazeWindow *p_win);

/* Drop every edit the way a user does since 6i2: x, with the edit panel
 * open (opened first when it is not -- x with the panel closed only says
 * where it works). Esc no longer discards. */
static void
revert_edits(GgazeWindow *p_win) {
   if (find_panel(p_win) == NULL) {
      fire(p_win, "win.enhance");
   }
   fire(p_win, "win.edit-revert");
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

/* Return an owned reference to a window transient for p_parent, if any. The
 * enhance UI used to be one (a gallery window); the panel tests assert that
 * none appears any more. */
static GtkWindow *
find_transient_window(GtkWindow *p_parent) {
   GListModel *p_windows = gtk_window_get_toplevels();
   guint       u_n       = g_list_model_get_n_items(p_windows);
   for (guint i = 0; i < u_n; i++) {
      GtkWindow *p_window = g_list_model_get_item(p_windows, i);
      if (gtk_window_get_transient_for(p_window) == p_parent) {
         return (p_window);
      }
      g_object_unref(p_window);
   }
   return (NULL);
}

static void
collect_pictures(GtkWidget *p_root, GPtrArray *p_pictures) {
   if (GTK_IS_PICTURE(p_root)) {
      g_ptr_array_add(p_pictures, p_root);
   }
   GtkWidget *p_child = gtk_widget_get_first_child(p_root);
   while (p_child != NULL) {
      collect_pictures(p_child, p_pictures);
      p_child = gtk_widget_get_next_sibling(p_child);
   }
}

static char *
load_bytes(const char *c_path, gsize *pu_len) {
   char   *c_data = NULL;
   GError *p_err  = NULL;
   g_assert_true(g_file_get_contents(c_path, &c_data, pu_len, &p_err));
   g_assert_no_error(p_err);
   return (c_data);
}

/* The enhance side panel: the widget carrying GGAZE_ENHANCE_PANEL_CLASS
 * anywhere under p_root (it sits beside the stack, not inside it), or NULL
 * when closed. Found by class rather than by tree shape so the tests do not
 * follow a layout change silently. */
static GtkWidget *
find_panel_in(GtkWidget *p_root) {
   if (gtk_widget_has_css_class(p_root, GGAZE_ENHANCE_PANEL_CLASS)) {
      return (p_root);
   }
   GtkWidget *p_child = gtk_widget_get_first_child(p_root);
   while (p_child != NULL) {
      GtkWidget *p_found = find_panel_in(p_child);
      if (p_found != NULL) {
         return (p_found);
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (NULL);
}

static GtkWidget *
find_panel(GgazeWindow *p_win) {
   return (find_panel_in(GTK_WIDGET(p_win)));
}

/* The card (a GtkButton with GGAZE_ENHANCE_CARD_CLASS, carrying its index
 * in the "idx" datum) for preset i_idx, or the Original card for -1; NULL if
 * absent. The class check matters: preset 0's datum is GINT_TO_POINTER(0),
 * i.e. NULL, which every other button also "has". */
static GtkWidget *
find_card(GtkWidget *p_root, gint i_idx) {
   if (gtk_widget_has_css_class(p_root, GGAZE_ENHANCE_CARD_CLASS) &&
       GPOINTER_TO_INT(g_object_get_data(G_OBJECT(p_root), "idx")) == i_idx) {
      return (p_root);
   }
   GtkWidget *p_child = gtk_widget_get_first_child(p_root);
   while (p_child != NULL) {
      GtkWidget *p_found = find_card(p_child, i_idx);
      if (p_found != NULL) {
         return (p_found);
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (NULL);
}

/* The Original reference (GGAZE_ENHANCE_ORIGINAL_CLASS) under p_root, or
 * NULL. Since 6i2 it is a picture to compare against, not a card: x / the
 * Revert button is the one way to drop every edit. */
static GtkWidget *
find_original(GtkWidget *p_root) {
   if (gtk_widget_has_css_class(p_root, GGAZE_ENHANCE_ORIGINAL_CLASS)) {
      return (p_root);
   }
   GtkWidget *p_child = gtk_widget_get_first_child(p_root);
   while (p_child != NULL) {
      GtkWidget *p_found = find_original(p_child);
      if (p_found != NULL) {
         return (p_found);
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (NULL);
}

/* The first GtkButton under p_root bound to c_action, or NULL. */
static GtkWidget *
find_action_button(GtkWidget *p_root, const char *c_action) {
   if (GTK_IS_BUTTON(p_root) &&
       g_strcmp0(gtk_actionable_get_action_name(GTK_ACTIONABLE(p_root)),
                 c_action) == 0) {
      return (p_root);
   }
   GtkWidget *p_child = gtk_widget_get_first_child(p_root);
   while (p_child != NULL) {
      GtkWidget *p_found = find_action_button(p_child, c_action);
      if (p_found != NULL) {
         return (p_found);
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (NULL);
}

/* The first GtkLabel under p_root whose text starts with c_prefix. */
static GtkWidget *
find_label_prefix(GtkWidget *p_root, const char *c_prefix) {
   if (GTK_IS_LABEL(p_root) &&
       g_str_has_prefix(gtk_label_get_text(GTK_LABEL(p_root)), c_prefix)) {
      return (p_root);
   }
   GtkWidget *p_child = gtk_widget_get_first_child(p_root);
   while (p_child != NULL) {
      GtkWidget *p_found = find_label_prefix(p_child, c_prefix);
      if (p_found != NULL) {
         return (p_found);
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (NULL);
}

/* Pump until every picture in p_pics has a paintable (the thumbnail batch
 * runs in a worker), up to 10 s. */
static void
wait_for_pictures_painted(GPtrArray *p_pics) {
   guint u_painted = 0;
   for (guint u = 0; u < 10000 && u_painted < p_pics->len; u++) {
      u_painted = 0;
      for (guint i = 0; i < p_pics->len; i++) {
         if (gtk_picture_get_paintable(g_ptr_array_index(p_pics, i)) != NULL) {
            u_painted++;
         }
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_cmpuint(u_painted, ==, p_pics->len);
}

/* Open plain.jpg alone in a presented i_w x i_h window with the thumbnail
 * preference set as asked, and return the window. The caller frees c_dir /
 * c_path via the out parameters. */
static GgazeWindow *
open_presented_sized(gboolean b_thumbnails, const char *c_tmpl, gint i_w,
                     gint i_h, char **c_dir_out, char **c_path_out) {
   Settings *p_cfg = settings_new();
   settings_set_enhance_preview_thumbnails(p_cfg, b_thumbnails);
   settings_delete(p_cfg);
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp(c_tmpl, &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   char        *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile       *p_file = g_file_new_for_path(c_path);
   GgazeWindow *p_win  = new_window();
   ggaze_window_open(p_win, p_file);
   g_object_unref(p_file);
   gtk_window_set_default_size(GTK_WINDOW(p_win), i_w, i_h);
   gtk_window_present(GTK_WINDOW(p_win));
   wait_for_view(p_win, PLAIN_JPG_W, PLAIN_JPG_H);
   *c_dir_out  = c_dir;
   *c_path_out = c_path;
   return (p_win);
}

/* open_presented_sized at 900x700. */
static GgazeWindow *
open_presented(gboolean b_thumbnails, const char *c_tmpl, char **c_dir_out,
               char **c_path_out) {
   return (open_presented_sized(b_thumbnails, c_tmpl, 900, 700, c_dir_out,
                                c_path_out));
}

/* Undo open_presented: reset the preference, close the window, drop the
 * folder. */
static void
close_presented(GgazeWindow *p_win, char *c_dir, char *c_path) {
   Settings *p_cfg = settings_new();
   g_settings_reset(settings_get_gsettings(p_cfg),
                    "enhance-preview-thumbnails");
   settings_delete(p_cfg);
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_free(c_path);
   ggtest_drain_main(300);
   ggtest_cleanup_temp_dir(c_dir);
}

/* 6i2: the panel is the one home of every edit -- the Transform and
 * Actions buttons are there, each on its action and showing its key (from
 * the table), none focusable (Space compares), Save names the file it will
 * write, and the title says what is edited. */
static void
assert_panel_buttons(GtkWidget *p_panel) {
   static const struct {
      const char *c_action;
      const char *c_key;
   } BUTTONS[] = {
      {"win.crop", "c"},         {"win.straighten", "r"},
      {"win.rotate-ccw", "["},   {"win.rotate-cw", "]"},
      {"win.enhance-save", "s"}, {"win.edit-revert", "x"},
      {"win.edit-undo", "u"},    {"win.edit-redo", "Shift+u"},
      {"win.enhance", "a/Esc"},
   };
   for (gsize u = 0; u < G_N_ELEMENTS(BUTTONS); u++) {
      GtkWidget *p_btn = find_action_button(p_panel, BUTTONS[u].c_action);
      g_assert_nonnull(p_btn);
      g_assert_nonnull(find_label_prefix(p_btn, BUTTONS[u].c_key));
      g_assert_false(gtk_widget_get_can_focus(p_btn)); /* Space compares */
   }
   g_assert_nonnull(find_label_prefix(p_panel, "as plain-enhanced.jpg"));
   /* The title is just "Edit": the window title already names the file. */
   GtkWidget *p_title = find_label_prefix(p_panel, "Edit");
   g_assert_nonnull(p_title);
   g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(p_title)), ==, "Edit");
   /* Close is the title row's flat button, named by its tooltip. */
   GtkWidget *p_close = find_action_button(p_panel, "win.enhance");
   g_assert_cmpstr(gtk_widget_get_tooltip_text(p_close), ==, "Close (a/Esc)");
}

/* `a` opens the panel INSIDE the window, beside the viewer -- no second
 * toplevel -- with one thumbnail card per preset plus the Original card, and
 * the image keeps the rest of the width rather than being covered. Asserted
 * on allocation: the panel starts where the viewer ends. */
static void
test_panel_opens_beside_viewer_with_thumbnails(void) {
   char        *c_dir  = NULL;
   char        *c_path = NULL;
   GgazeWindow *p_win =
      open_presented(TRUE, "ggaze-enhance-panel-XXXXXX", &c_dir, &c_path);
   g_assert_null(find_panel(p_win));

   fire(p_win, "win.enhance");
   GtkWidget *p_panel = find_panel(p_win);
   g_assert_nonnull(p_panel);
   g_assert_null(find_transient_window(GTK_WINDOW(p_win)));
   for (guint u = 0; u < 3000 && gtk_widget_get_width(p_panel) == 0; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_cmpint(gtk_widget_get_width(p_panel), >=,
                   GGAZE_ENHANCE_PANEL_WIDTH);
   GtkWidget *p_viewer =
      gtk_stack_get_child_by_name(ggaze_window_get_stack(p_win), "large");
   graphene_rect_t r_panel, r_viewer;
   g_assert_true(
      gtk_widget_compute_bounds(p_panel, GTK_WIDGET(p_win), &r_panel));
   g_assert_true(
      gtk_widget_compute_bounds(p_viewer, GTK_WIDGET(p_win), &r_viewer));
   g_assert_cmpfloat(r_panel.origin.x, >=,
                     r_viewer.origin.x + r_viewer.size.width - 1.0f);
   g_assert_cmpfloat(r_viewer.size.width, >, 400.0f);

   /* Original + 8 presets, every one a picture card that gets painted. */
   GPtrArray *p_pics = g_ptr_array_new();
   collect_pictures(p_panel, p_pics);
   g_assert_cmpuint(p_pics->len, ==, 9);
   g_assert_nonnull(find_original(p_panel));
   g_assert_null(find_card(p_panel, -1)); /* a reference, no card */
   g_assert_nonnull(find_card(p_panel, 7));
   g_assert_null(find_card(p_panel, 8));
   wait_for_pictures_painted(p_pics);
   g_ptr_array_unref(p_pics);
   /* The save state is spelled out even before anything is on. */
   g_assert_nonnull(find_label_prefix(p_panel, "No edits yet"));
   assert_panel_buttons(p_panel);

   fire(p_win, "win.enhance"); /* close */
   g_assert_null(find_panel(p_win));
   fire(p_win, "win.enhance"); /* reopen, start another preview batch */
   fire(p_win, "win.enhance"); /* immediately close and cancel it */
   ggtest_drain_main(500);
   close_presented(p_win, c_dir, c_path);
}

/* The GtkScale strength slider of card i_idx under p_root, or NULL. */
static GtkWidget *
find_scale(GtkWidget *p_root, gint i_idx) {
   if (GTK_IS_SCALE(p_root) &&
       gtk_widget_has_css_class(p_root, GGAZE_ENHANCE_SCALE_CLASS) &&
       GPOINTER_TO_INT(g_object_get_data(G_OBJECT(p_root), "idx")) == i_idx) {
      return (p_root);
   }
   GtkWidget *p_child = gtk_widget_get_first_child(p_root);
   while (p_child != NULL) {
      GtkWidget *p_found = find_scale(p_child, i_idx);
      if (p_found != NULL) {
         return (p_found);
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (NULL);
}

/* Card i_idx's strength slider is on screen: visible, mapped, laid out,
 * and not focusable (Space compares). */
static void
assert_scale_shown(GtkWidget *p_panel, gint i_idx) {
   GtkWidget *p_scale = find_scale(p_panel, i_idx);
   g_assert_nonnull(p_scale);
   g_assert_true(gtk_widget_get_mapped(p_scale));
   g_assert_cmpint(gtk_widget_get_height(p_scale), >, 0);
   g_assert_false(gtk_widget_get_focusable(p_scale));
   graphene_rect_t r_scale;
   g_assert_true(gtk_widget_compute_bounds(p_scale, p_panel, &r_scale));
   g_assert_cmpfloat(r_scale.origin.y, >=, 0.0f);
}

/* The panel is compact (6i2 review): in a 1280x800 window every one of the
 * eight built-in presets is on screen without scrolling -- the card
 * scroller's content fits its page -- and the Transform and Save rows sit
 * inside the panel below them. The old full-width cards showed one and a
 * half presets there. 8i2: with a tunable card selected (j), its strength
 * slider shows under it and all of that still fits. */
static void
test_panel_fits_eight_presets_at_1280x800(void) {
   char        *c_dir  = NULL;
   char        *c_path = NULL;
   GgazeWindow *p_win  = open_presented_sized(TRUE, "ggaze-enhance-fit-XXXXXX",
                                              1280, 800, &c_dir, &c_path);
   fire(p_win, "win.enhance");
   g_assert_true(ggaze_window_edit_key(p_win, GDK_KEY_j, 0)); /* Brightness */
   GtkWidget *p_panel = find_panel(p_win);
   GtkWidget *p_last  = find_card(p_panel, 7);
   GtkWidget *p_sw = gtk_widget_get_ancestor(p_last, GTK_TYPE_SCROLLED_WINDOW);
   GtkAdjustment *p_adj =
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(p_sw));
   for (guint u = 0; u < 3000 && (gtk_widget_get_height(p_last) == 0 ||
                                  gtk_adjustment_get_page_size(p_adj) == 0.0);
        u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   ggtest_drain_main(200);
   g_assert_cmpint(gtk_widget_get_height(GTK_WIDGET(p_win)), <=, 800);
   g_assert_cmpfloat(gtk_adjustment_get_upper(p_adj), <=,
                     gtk_adjustment_get_page_size(p_adj) + 0.5);
   graphene_rect_t r_save, r_panel, r_redo;
   g_assert_true(gtk_widget_compute_bounds(
      find_action_button(p_panel, "win.enhance-save"), p_panel, &r_save));
   g_assert_true(gtk_widget_compute_bounds(p_panel, p_panel, &r_panel));
   g_assert_cmpfloat(r_save.origin.y + r_save.size.height, <=,
                     r_panel.size.height);
   /* 7i2: the Undo / Redo row under it fits too, all presets still on. */
   g_assert_true(gtk_widget_compute_bounds(
      find_action_button(p_panel, "win.edit-redo"), p_panel, &r_redo));
   g_assert_cmpfloat(r_redo.origin.y, >=, r_save.origin.y + r_save.size.height);
   g_assert_cmpfloat(r_redo.origin.y + r_redo.size.height, <=,
                     r_panel.size.height);
   assert_scale_shown(p_panel, 1);
   close_presented(p_win, c_dir, c_path);
}

/* Preferences can turn the thumbnails off: the same panel, label-only cards,
 * no GtkPicture anywhere (so no preview batch is ever started). */
static void
test_panel_label_only_mode_has_no_pictures(void) {
   char        *c_dir  = NULL;
   char        *c_path = NULL;
   GgazeWindow *p_win =
      open_presented(FALSE, "ggaze-enhance-labels-XXXXXX", &c_dir, &c_path);
   fire(p_win, "win.enhance");
   GtkWidget *p_panel = find_panel(p_win);
   g_assert_nonnull(p_panel);
   GPtrArray *p_pics = g_ptr_array_new();
   collect_pictures(p_panel, p_pics);
   g_assert_cmpuint(p_pics->len, ==, 0);
   g_ptr_array_unref(p_pics);
   g_assert_nonnull(find_original(p_panel));
   g_assert_null(find_card(p_panel, -1)); /* a reference, no card */
   g_assert_nonnull(find_card(p_panel, 7));
   fire(p_win, "win.enhance");
   g_assert_null(find_panel(p_win));
   close_presented(p_win, c_dir, c_path);
}

/* The cards report the mask (highlight on the enabled ones) and their
 * thumbnails are per-preset previews that do NOT change when presets are
 * layered: the large view is the one place that shows the combination
 * (hold Space compares it with the original), so the cards must stay the
 * stable reference they were painted as. Asserted on paintable identity. */
static void
test_panel_cards_track_mask_and_thumbnails_stay(void) {
   char        *c_dir  = NULL;
   char        *c_path = NULL;
   GgazeWindow *p_win =
      open_presented(TRUE, "ggaze-enhance-cards-XXXXXX", &c_dir, &c_path);
   fire(p_win, "win.enhance");
   GtkWidget *p_panel = find_panel(p_win);
   g_assert_nonnull(p_panel);
   GPtrArray *p_pics = g_ptr_array_new();
   collect_pictures(p_panel, p_pics);
   wait_for_pictures_painted(p_pics);
   GtkPicture   *p_pic0   = g_ptr_array_index(p_pics, 0);
   GtkPicture   *p_pic1   = g_ptr_array_index(p_pics, 1);
   GdkPaintable *p_thumb0 = gtk_picture_get_paintable(p_pic0);
   GdkPaintable *p_thumb1 = gtk_picture_get_paintable(p_pic1);
   g_ptr_array_unref(p_pics);

   GdkTexture *p_orig = ref_viewer_texture(p_win);
   fire(p_win, "win.enhance-1");
   fire(p_win, "win.enhance-3");
   wait_for_texture_change(p_win, p_orig);
   ggtest_drain_main(300); /* let the second apply land too */
   g_assert_true(viewer_texture(p_win) != p_orig);
   g_assert_true(
      gtk_widget_has_css_class(find_card(p_panel, 0), "ggaze-enhance-on"));
   g_assert_true(
      gtk_widget_has_css_class(find_card(p_panel, 2), "ggaze-enhance-on"));
   g_assert_false(
      gtk_widget_has_css_class(find_card(p_panel, 1), "ggaze-enhance-on"));
   g_assert_true(gtk_picture_get_paintable(p_pic0) == p_thumb0);
   g_assert_true(gtk_picture_get_paintable(p_pic1) == p_thumb1);
   g_assert_nonnull(find_label_prefix(p_panel, "Unsaved edits"));

   fire(p_win, "win.enhance-1");
   fire(p_win, "win.enhance-3");
   ggtest_drain_main(300);
   g_assert_false(ggaze_window_enhance_is_dirty(p_win));
   g_assert_false(
      gtk_widget_has_css_class(find_card(p_panel, 0), "ggaze-enhance-on"));
   g_assert_nonnull(find_label_prefix(p_panel, "No edits yet"));
   g_object_unref(p_orig);
   close_presented(p_win, c_dir, c_path);
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
   GgazeWindow *p_win; /* an EXTRA ref is held (see fixture_open) */
   /* Both textures are OWNED (reffed in fixture_make_dirty, released in
    * fixture_teardown) and are NULL until the fixture is made dirty. The refs
    * are not optional tidiness: subtests compare later textures against these
    * pointers for identity, and neither is otherwise kept alive by anything
    * the test controls. p_orig in particular is held only by the window's
    * texture cache once the preview replaces it in the viewer, so
    * ggaze_window_clear_texture_cache() finalizes it -- after which
    * `p_reloaded != fx.p_orig` compares a live pointer against a freed
    * address, so it says nothing about identity and holds only while the
    * allocator keeps that address out of circulation. Measured before this
    * ref existed: the original was dead after the clear in 15/15 runs, and a
    * GdkMemoryTexture allocated at the first opportunity afterwards landed on
    * exactly that address in 9/15. Owning a ref keeps both pointers valid and
    * makes the identity comparisons mean what they say.
    *
    * Because fixture_teardown releases them unconditionally, a fixture whose
    * owned fields hold stack garbage unrefs that garbage. Two things keep that
    * impossible rather than merely unlikely: every declaration site writes
    * `DirtyFixture fx = {0};`, and fixture_open_clean zeroes the whole struct
    * on entry. Neither is decoration -- with both removed the three subtests
    * that stay clean crash (9w0 re-measured 3/3: one "g_object_unref:
    * assertion 'G_IS_OBJECT (object)' failed", two SIGSEGV). */
   GdkTexture *p_orig; /* texture before any preset was applied */
   GdkTexture *p_mod;  /* the enhance preview texture */
   guint       u_ref;  /* p_win's refcount once dirty and settled */
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
   /* Zeroed as a whole rather than field by field: several subtests stay clean
    * and never call fixture_make_dirty, yet fixture_teardown releases p_orig
    * and p_mod unconditionally (see DirtyFixture). Naming the owned fields one
    * by one worked only for as long as everyone remembered to extend the list;
    * a memset cannot be forgotten when a field is added. */
   memset(p_fx, 0, sizeof(*p_fx));
   p_fx->c_dir = g_dir_make_tmp(c_tmpl, &p_err);
   g_assert_no_error(p_err);
   for (guint u = 0; c_names[u] != NULL; u++) {
      copy_fixture(p_fx->c_dir, c_names[u]);
   }
   /* The load wait below matches plain.jpg's decoded size, so the start file
    * has to be plain.jpg; every caller's list starts with it. */
   g_assert_cmpstr(c_names[0], ==, "plain.jpg");
   p_fx->c_path = g_build_filename(p_fx->c_dir, c_names[0], NULL);
   p_fx->p_file = g_file_new_for_path(p_fx->c_path);
   p_fx->p_win  = new_window();
   g_object_ref(p_fx->p_win);
   ggaze_window_open(p_fx->p_win, p_fx->p_file);
   wait_for_load(p_fx->p_win, PLAIN_JPG_W, PLAIN_JPG_H);
   g_assert_false(ggaze_window_enhance_is_dirty(p_fx->p_win));
}

/* Apply preset 1, so the window ends up dirty with a live preview, take a ref
 * on both the original and the preview (see DirtyFixture), and record the
 * reference count every assert_ref_settled() call compares against. */
static void
fixture_make_dirty(DirtyFixture *p_fx) {
   p_fx->p_orig = ref_viewer_texture(p_fx->p_win);
   fire(p_fx->p_win, "win.enhance-1");
   wait_for_texture_change(p_fx->p_win, p_fx->p_orig);
   p_fx->p_mod = ref_viewer_texture(p_fx->p_win);
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
   /* After the drain, so the fixture's textures stay valid for the whole of
    * teardown; no-ops for a fixture that never went dirty. */
   g_clear_object(&p_fx->p_orig);
   g_clear_object(&p_fx->p_mod);
   ggtest_cleanup_temp_dir(p_fx->c_dir);
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
   GGTEST_ASSERT_DIALOG_UP(p_own, c_button);
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
 *
 * The subtests below build their windows by hand instead of using
 * DirtyFixture, but they keep texture pointers across main-loop iterations for
 * exactly the same reasons, so they take the same owned refs through
 * ref_viewer_texture(). A texture a subtest still compares against once its
 * teardown has started is released in that teardown block AFTER the final
 * drain, like fixture_teardown, so nothing the drain runs can invalidate a
 * pointer while it is still comparable.
 *
 * That is the rule for the subtests' own locals, not a blanket claim about
 * every owned ref below: assert_hold_cycle_works() releases its two textures
 * inside the helper, before its caller's final drain, because the cycle it
 * owns them for has finished by then and nothing compares them afterwards.
 *
 * Borrowing happened to be safe here only because none of these subtests
 * clears the texture cache; that is a property of the tests, not of the code
 * they exercise, and 3w0 is what borrowing costs once it stops holding.
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
   wait_for_load(p_win, PLAIN_JPG_W, PLAIN_JPG_H);
   g_assert_false(ggaze_window_enhance_is_dirty(p_win));

   GdkTexture *p_orig_tex = ref_viewer_texture(p_win);
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
   g_object_unref(p_orig_tex);
   ggtest_cleanup_temp_dir(c_dir);
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
   wait_for_load(p_win, PLAIN_JPG_W, PLAIN_JPG_H);

   GdkTexture *p_orig_tex = ref_viewer_texture(p_win);
   fire(p_win, "win.enhance-1");
   wait_for_texture_change(p_win, p_orig_tex);
   g_assert_true(ggaze_window_enhance_is_dirty(p_win));

   GdkTexture *p_enhanced_tex = ref_viewer_texture(p_win);
   fire(p_win, "win.enhance-1"); /* toggle the same preset back off */
   wait_for_texture_change(p_win, p_enhanced_tex);
   g_assert_false(ggaze_window_enhance_is_dirty(p_win));
   /* Back to the (cache-hit, synchronous) original texture. */
   g_assert_true(viewer_texture(p_win) == p_orig_tex);

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_free(c_path);
   ggtest_drain_main(300);
   g_object_unref(p_orig_tex);
   g_object_unref(p_enhanced_tex);
   ggtest_cleanup_temp_dir(c_dir);
}

/* The crop tool under a held Space, on a preview p_enhanced whose original
 * is p_orig: its Enter is refused with "Release Space first" -- the screen
 * shows the original, not the base the rectangle is laid out on -- and not
 * with "Preview still rendering", which used to be the only message and
 * was wrong advice (no render was pending; waiting would not have helped).
 * Released, the same Enter commits (the untouched rectangle: no crop). The
 * base needs no re-render here, so the preview texture stays put. Split
 * out of the subtest below to keep it under the 50-line convention. */
static void
assert_crop_enter_refused_under_hold(GgazeWindow *p_win, GdkTexture *p_orig,
                                     GdkTexture *p_enhanced) {
   fire(p_win, "win.crop");
   g_assert_cmpint(ggaze_window_get_tool(p_win), ==, GGAZE_TOOL_CROP);
   g_assert_true(viewer_texture(p_win) == p_enhanced);
   ggaze_window_set_hold_original(p_win, TRUE);
   g_assert_true(viewer_texture(p_win) == p_orig);
   g_assert_true(ggaze_window_tool_key(p_win, GDK_KEY_Return, 0));
   g_assert_cmpint(ggaze_window_get_tool(p_win), ==, GGAZE_TOOL_CROP);
   g_assert_true(g_str_has_prefix(status_text(p_win), "Release Space"));
   ggaze_window_set_hold_original(p_win, FALSE);
   g_assert_true(viewer_texture(p_win) == p_enhanced);
   g_assert_true(ggaze_window_tool_key(p_win, GDK_KEY_Return, 0));
   g_assert_cmpint(ggaze_window_get_tool(p_win), ==, GGAZE_TOOL_NONE);
   g_assert_true(g_str_has_prefix(status_text(p_win), "Crop removed"));
   g_assert_true(ggaze_window_enhance_is_dirty(p_win)); /* the preset */
}

/* Requirement 4: hold-Space shows the original while "held", then restores
 * the modified preview on "release", without touching the dirty mask; the
 * crop tool's Enter under the hold says "Release Space first". */
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
   wait_for_load(p_win, PLAIN_JPG_W, PLAIN_JPG_H);

   GdkTexture *p_orig_tex = ref_viewer_texture(p_win);
   fire(p_win, "win.enhance-1");
   wait_for_texture_change(p_win, p_orig_tex);
   GdkTexture *p_enhanced_tex = ref_viewer_texture(p_win);
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

   assert_crop_enter_refused_under_hold(p_win, p_orig_tex, p_enhanced_tex);

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_free(c_path);
   ggtest_drain_main(300);
   g_object_unref(p_orig_tex);
   g_object_unref(p_enhanced_tex);
   ggtest_cleanup_temp_dir(c_dir);
}

/* Drain until the `i` card's plot holds a histogram whose bins are those of
 * p_tex, binned here through the same histogram_new_from_texture() the
 * overlay's worker uses, and assert it is visible. The refresh is
 * asynchronous (the old plot is cleared at once, the new one lands from a
 * worker), hence the poll; 3 s is generous for a 6x3 fixture. */
static void
wait_for_plot_of(GgazeWindow *p_win, GdkTexture *p_tex) {
   GgazeHistogramView *p_plot =
      GGAZE_HISTOGRAM_VIEW(ggaze_window_get_info_histogram(p_win));
   Histogram *p_want = histogram_new_from_texture(p_tex);
   g_assert_nonnull(p_want);
   const Histogram *p_got = NULL;
   for (guint u = 0; u < 3000; u++) {
      p_got = ggaze_histogram_view_get_histogram(p_plot);
      if (p_got != NULL &&
          memcmp(p_got->u_bins, p_want->u_bins, sizeof(p_want->u_bins)) == 0) {
         break;
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(p_got);
   g_assert_cmpmem(p_got->u_bins, sizeof(p_got->u_bins), p_want->u_bins,
                   sizeof(p_want->u_bins));
   g_assert_true(gtk_widget_get_visible(GTK_WIDGET(p_plot)));
   histogram_delete(p_want);
}

/* The two textures must bin differently, or the follow-the-picture
 * assertions below could pass on a plot that never moved. */
static void
assert_bins_differ(GdkTexture *p_a, GdkTexture *p_b) {
   Histogram *p_ha = histogram_new_from_texture(p_a);
   Histogram *p_hb = histogram_new_from_texture(p_b);
   g_assert_nonnull(p_ha);
   g_assert_nonnull(p_hb);
   g_assert_cmpint(memcmp(p_ha->u_bins, p_hb->u_bins, sizeof(p_ha->u_bins)), !=,
                   0);
   histogram_delete(p_ha);
   histogram_delete(p_hb);
}

/* 0c2 second review: the `i` card plots the image on screen for as long as
 * it is up, not just the texture `i` was pressed over. Preset lands -> `i`
 * plots the preview; hold-Space (original on screen) -> the original's
 * bins; release -> the preview's again; a second preset landing -> the new
 * preview's. Every step compares the plot bin-for-bin with the viewer's
 * texture binned independently. */
static void
test_info_plots_preview(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-enhance-info-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   char        *c_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile       *p_file = g_file_new_for_path(c_path);
   GgazeWindow *p_win  = new_window();
   ggaze_window_open(p_win, p_file);
   wait_for_load(p_win, PLAIN_JPG_W, PLAIN_JPG_H);

   GdkTexture *p_orig = ref_viewer_texture(p_win);
   fire(p_win, "win.enhance-1");
   wait_for_texture_change(p_win, p_orig);
   GdkTexture *p_mod = ref_viewer_texture(p_win);
   g_assert_true(p_mod != p_orig);
   assert_bins_differ(p_orig, p_mod);

   fire(p_win, "win.info");
   wait_for_plot_of(p_win, p_mod); /* the preview, not the file's pixels */

   ggaze_window_set_hold_original(p_win, TRUE);
   g_assert_true(viewer_texture(p_win) == p_orig);
   wait_for_plot_of(p_win, p_orig); /* follows hold-Space ... */
   ggaze_window_set_hold_original(p_win, FALSE);
   wait_for_plot_of(p_win, p_mod); /* ... and the release */

   fire(p_win, "win.enhance-2"); /* a second preset lands on top */
   wait_for_texture_change(p_win, p_mod);
   GdkTexture *p_mod2 = ref_viewer_texture(p_win);
   g_assert_true(p_mod2 != p_mod);
   assert_bins_differ(p_mod, p_mod2);
   wait_for_plot_of(p_win, p_mod2); /* follows the new preview */

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_free(c_path);
   ggtest_drain_main(300);
   g_object_unref(p_orig);
   g_object_unref(p_mod);
   g_object_unref(p_mod2);
   ggtest_cleanup_temp_dir(c_dir);
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

/* One complete hold-compare cycle on a fresh preview: apply a preset, then
 * Space down must show the original and Space up the preview again. Split out
 * of the subtest below so it stays inside the 50-line convention, and because
 * the two owned textures are of no interest outside the cycle. */
static void
assert_hold_cycle_works(GgazeWindow *p_win) {
   GdkTexture *p_orig = ref_viewer_texture(p_win);
   fire(p_win, "win.enhance-1");
   wait_for_texture_change(p_win, p_orig);
   GdkTexture *p_mod = ref_viewer_texture(p_win);
   g_assert_true(p_mod != p_orig);

   ggaze_window_set_hold_original(p_win, TRUE);
   g_assert_true(viewer_texture(p_win) == p_orig);
   ggaze_window_set_hold_original(p_win, FALSE);
   g_assert_true(viewer_texture(p_win) == p_mod);

   g_object_unref(p_orig);
   g_object_unref(p_mod);
}

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
   wait_for_load(p_win, PLAIN_JPG_W, PLAIN_JPG_H);

   GdkTexture *p_orig_tex = ref_viewer_texture(p_win);
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
   assert_hold_cycle_works(p_win);

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_free(c_path);
   ggtest_drain_main(300);
   g_object_unref(p_orig_tex);
   ggtest_cleanup_temp_dir(c_dir);
}

/* A second `s` while the window is still dirty must not clobber the first
 * export: mover.c's collision convention gives the new sibling a "-1" suffix.
 * Split out of the subtest below to keep it inside the 50-line convention,
 * which taking an owned texture ref (see the section comment) pushed it past.
 */
static void
assert_second_save_is_suffixed(GgazeWindow *p_win, const char *c_dir) {
   fire(p_win, "win.enhance-save");
   char *c_out2 = g_build_filename(c_dir, "plain-enhanced-1.jpg", NULL);
   wait_for_file(c_out2);
   ggtest_drain_main(100);
   g_free(c_out2);
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
   wait_for_load(p_win, PLAIN_JPG_W, PLAIN_JPG_H);

   gsize u_before_len;
   char *c_before = load_bytes(c_path, &u_before_len);

   GdkTexture *p_orig_tex = ref_viewer_texture(p_win);
   fire(p_win, "win.enhance-1");
   wait_for_texture_change(p_win, p_orig_tex);

   char  *c_out1 = g_build_filename(c_dir, "plain-enhanced.jpg", NULL);
   GFile *p_out1 = g_file_new_for_path(c_out1);
   g_assert_false(g_file_query_exists(p_out1, NULL));
   fire(p_win, "win.enhance-save");
   wait_for_file(c_out1);
   ggtest_drain_main(100);
   g_assert_true(g_file_query_exists(p_out1, NULL));

   /* Original is still exactly what it was. */
   gsize u_after_len;
   char *c_after = load_bytes(c_path, &u_after_len);
   g_assert_cmpuint(u_after_len, ==, u_before_len);
   g_assert_cmpint(memcmp(c_before, c_after, u_before_len), ==, 0);

   /* Still dirty (toggling didn't clear the mask), so `s` again. */
   assert_second_save_is_suffixed(p_win, c_dir);
   /* The suffixed sibling is what says the first export was not written over;
    * this only adds that it was not removed either. */
   g_assert_true(g_file_query_exists(p_out1, NULL));

   g_free(c_before);
   g_free(c_after);
   g_object_unref(p_out1);
   g_free(c_out1);
   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_free(c_path);
   ggtest_drain_main(300);
   g_object_unref(p_orig_tex);
   ggtest_cleanup_temp_dir(c_dir);
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
   wait_for_load(p_win, PLAIN_JPG_W, PLAIN_JPG_H);

   g_assert_false(ggaze_window_enhance_is_dirty(p_win));
   fire(p_win, "win.next"); /* not dirty -> _maybe_save_then proceeds inline */
   const gchar *c_title = gtk_window_get_title(GTK_WINDOW(p_win));
   g_assert_nonnull(g_strstr_len(c_title, -1, "rot6.jpg"));

   g_object_unref(p_f0);
   g_free(c_p0);
   gtk_window_destroy(GTK_WINDOW(p_win));
   ggtest_drain_main(300);
   ggtest_cleanup_temp_dir(c_dir);
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
   DirtyFixture fx = {0};
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
      wait_for_load(p_clean, PLAIN_JPG_W, PLAIN_JPG_H);
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
   wait_for_load(p_win, PLAIN_JPG_W, PLAIN_JPG_H);
   GdkTexture *p_orig_tex = ref_viewer_texture(p_win);
   fire(p_win, "win.enhance-1");
   wait_for_texture_change(p_win, p_orig_tex);
   g_assert_true(ggaze_window_enhance_is_dirty(p_win));

   gboolean b_stop = FALSE;
   g_signal_emit_by_name(p_win, "close-request", &b_stop);
   g_assert_true(b_stop);                               /* close blocked */
   g_assert_true(ggaze_window_enhance_is_dirty(p_win)); /* still dirty */
   GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(p_win), "Cancel");

   /* Answer it, so nothing is left pending across teardown. */
   g_assert_true(ggtest_click_dialog_button(GTK_WINDOW(p_win), "Cancel"));
   ggtest_drain_main(300);

   g_object_unref(p_file);
   gtk_window_destroy(GTK_WINDOW(p_win));
   g_free(c_path);
   ggtest_drain_main(300);
   g_object_unref(p_orig_tex);
   ggtest_cleanup_temp_dir(c_dir);
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
   DirtyFixture fx = {0};
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

/* Task 8w0: getting rid of the question is not an answer either. The Save
 * prompt's half of test_delete_safety.c's
 * test_dismissing_the_confirm_deletes_nothing, and the assertion 8w0's
 * diagnosis was missing -- until now, "a close-request on the Save prompt
 * resolves it like Cancel, silently" was a comment nobody had executed.
 *
 * gtk_window_close() on the dialog's OWN toplevel is what Escape does
 * (gtk/deprecated/gtkdialog.c binds Escape to ::close, which calls
 * gtk_window_close()) and what a window manager's close button does
 * (gtkmain.c turns GDK_DELETE into gtk_window_emit_close_request). Neither a
 * window manager nor a real key is needed to drive it from here, so this runs
 * on every lane -- including the Xvfb one, where the flake it pins cannot
 * happen by itself.
 *
 * Both halves of the outcome are asserted, and each pins a decision made two
 * functions apart in window.c:
 *
 *   - the STATE: preview kept, deferred select not run, ctx released. That is
 *     _save_prompt_outcome's classification plus _save_dialog_cb's terminal
 *     _save_ctx_finish(ctx, FALSE), and it is what the flake did silently.
 *   - the REPORT: one g_message. It exists only because _save_prompt_show
 *     configures no cancel button (8w0); put that call back and the close
 *     returns button index 0 instead, _report_unanswered_prompt never runs,
 *     and g_test_assert_expected_messages() fails while every state assertion
 *     below still passes -- which is exactly the invisibility 8w0 was filed
 *     for. */
static void
test_closing_the_prompt_keeps_the_preview(void) {
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-closeprompt-XXXXXX");

   activate_other_cell(&fx); /* the deferred change the prompt now gates */
   GtkWindow *p_dlg = GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(fx.p_win), "Cancel");

   /* The tree's only g_test_expect_message(). What it costs, measured by
    * instrumenting THIS binary -- not a probe replicating it (dw0). That
    * distinction is the whole history of this note: main() is not just
    * g_test_init(). It is g_test_init() at :2303 and then
    * g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL) at
    * :2304, which STRIPS the warning bit g_test_init() had just set. A probe
    * that replicates only the first line measures a different program, and
    * twice now has been transcribed in here as if it did not.
    *
    * So warnings are survivable in this suite, and ARMING is what makes one
    * fatal -- not the other way round. While an expectation is armed GLib
    * compares every non-debug message against the head of the queue, and on a
    * mismatch logs "Did not see expected message ..." as a CRITICAL and marks
    * the mismatching message G_LOG_FLAG_FATAL.
    *
    * Measured by injecting g_log("Gtk", G_LOG_LEVEL_WARNING, "Unknown key
    * gtk-modules") into this subtest and running it on the X11 lane, gtk
    * 4.22.4 / glib 2.88.2:
    *
    *   before the arm : logged, the subtest CONTINUES, "ok 1", exit 0.
    *   after the arm  : "GLib-CRITICAL **: Did not see expected message ..."
    *                    then "not ok - Gtk-FATAL-: ...", "Bail out!", abort.
    *
    * Note the armed line reads "Gtk-FATAL-:" with no "WARNING" in it; the
    * mask override above is why. Quoting a "Gtk-FATAL-WARNING" here would be
    * a tell that the text came from somewhere else.
    *
    * KEPT AT 400 ms, re-derived rather than restated. Narrowing is possible:
    * g_test_assert_expected_messages() disarms, so a short armed drain
    * followed by the rest of the settle would shrink the window without
    * changing the total. And there is room -- time from gtk_window_close() to
    * the g_message, 5 runs each: 2.35-2.49 ms plain, 2.77-4.14 ms ASan. The
    * armed window is ~100x what the GTask idle actually needs, so the race
    * against that idle is not what blocks narrowing.
    *
    * What blocks it is that the exposure narrowing removes is not the one
    * that exists. The only foreign warning this box emits is the startup
    * "Unknown key gtk-modules" from ~/.config/gtk-4.0/settings.ini, and it
    * fires ~770 ms BEFORE this window opens (same instrumented run) -- i.e.
    * outside it either way. Narrowing would buy a hypothetical, and pay for
    * it with a constant tuned on one box, in a suite whose recorded failure
    * mode is exactly that. It would also swap a diagnosability problem (an
    * abort whose first line names the wrong thing) for a correctness one (a
    * lane that fails when a loaded box stretches the idle past the constant).
    *
    * If a Gtk warning ever does start landing IN this window, the numbers
    * above are what you need to narrow it safely -- but do not expect this
    * suite to have failed without the arming. */
   g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE,
                         "*prompt dismissed*");
   gtk_window_close(p_dlg); /* what Escape and the WM close button do */
   ggtest_drain_main(400);
   g_test_assert_expected_messages();

   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    0);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win)); /* preview kept */
   assert_showing(fx.p_win, "plain.jpg");                  /* select not run */
   g_assert_true(viewer_texture(fx.p_win) == fx.p_mod);    /* still shown */
   assert_ref_settled(&fx); /* the ctx was released, not orphaned */
   g_assert_true(ggtest_is_open_toplevel(GTK_WINDOW(fx.p_win)));

   fixture_teardown(&fx);
}

/* Discard: the deferred change is applied once the user answers. This is the
 * first test in the suite that runs _proceed_grid_select at all -- until now
 * every assertion stopped at "the change was deferred". */
static void
test_discard_applies_deferred_grid_select(void) {
   DirtyFixture fx = {0};
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
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-savego-XXXXXX");

   gsize u_len_before;
   char *c_before = load_bytes(fx.c_path, &u_len_before);

   activate_other_cell(&fx);
   answer_prompt(&fx, "Save");

   char *c_out = g_build_filename(fx.c_dir, "plain-enhanced.jpg", NULL);
   wait_for_file(c_out);
   ggtest_drain_main(300); /* the gate's continuation runs after the write */
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
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-failsave-XXXXXX");

   g_assert_cmpint(g_chmod(fx.c_dir, 0500), ==, 0); /* r-x: no new files */
   activate_other_cell(&fx);
   answer_prompt(&fx, "Save");
   wait_for_status_prefix(fx.p_win, "Enhance-save failed"); /* worker done */
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
   DirtyFixture fx = {0};
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
   DirtyFixture fx = {0};
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
   DirtyFixture fx = {0};
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
   DirtyFixture fx = {0};
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
   DirtyFixture fx = {0};
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
   ggtest_cleanup_temp_dir(c_other);
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

   DirtyFixture fx = {0}; /* the window reads destinations at construction */
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
   ggtest_cleanup_temp_dir(c_dest);
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
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-captured-XXXXXX");
   char *c_other = g_build_filename(fx.c_dir, "rot6.jpg", NULL);

   fire(fx.p_win, "win.trash"); /* `d`, pressed on plain.jpg */
   ggtest_drain_main(150);
   GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(fx.p_win), "Cancel");

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
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-stale-XXXXXX");
   GError *p_err   = NULL;
   char   *c_other = g_dir_make_tmp("ggaze-enhance-stale2-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_other, "small.png");
   char  *c_target = g_build_filename(c_other, "small.png", NULL);
   GFile *p_target = g_file_new_for_path(c_target);

   fire(fx.p_win, "win.trash"); /* prompt, raised for plain.jpg */
   ggtest_drain_main(150);
   GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(fx.p_win), "Cancel");
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
   ggtest_cleanup_temp_dir(c_other);
}

/* Round 3, finding (k): answering Save on a prompt whose preview has since
 * vanished must not be a silent no-op that ALSO cancels the user's action.
 * _enhance_do_save returns FALSE both for "nothing to save" and "the export
 * failed", and _save_dialog_cb used to treat both as a failure -- so with the
 * mask cleared under the dialog, clicking Save wrote nothing, showed nothing,
 * and aborted the trash the user had asked for. */
static void
test_save_with_no_preview_left_reports_and_proceeds(void) {
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-nosave-XXXXXX");

   fire(fx.p_win, "win.trash"); /* `d`, pressed on plain.jpg */
   ggtest_drain_main(150);
   GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(fx.p_win), "Cancel");
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
   /* The gate said "Nothing to save" and proceeded; the trash then reported
    * its own outcome, which is the status the user is left looking at. */
   g_assert_cmpstr(
      gtk_label_get_text(GTK_LABEL(ggaze_window_get_info_label(fx.p_win))), ==,
      "Trashed plain.jpg \u2014 u to undo");

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
   DirtyFixture fx = {0};
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
   ggtest_cleanup_temp_dir(c_other);
}

/* Round 3, finding (i), the drop half. Cancel means "stay on this image, keep
 * the preview", so running the parked request anyway would do exactly what the
 * user just refused: it is dropped instead -- but visibly, via the status
 * label, not silently. This is also the only test that exercises the drop path
 * with fn_free_data != NULL: the parked _OpenCtx holds an owned window ref, so
 * assert_ref_settled is what proves it was actually released. */
static void
test_queued_request_is_dropped_on_cancel(void) {
   DirtyFixture fx = {0};
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
   ggtest_cleanup_temp_dir(c_other);
}

/* Save on close-request, the success half: the export lands AND the window
 * really closes. Only the Discard half of this was covered (round 3's
 * "still untested" list). See test_close_request_discard_closes_window for
 * why the window has to be presented and why "closed" is asked as "no longer
 * a live toplevel". */
static void
test_close_request_save_closes_window(void) {
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-closesave-XXXXXX");
   gtk_window_present(GTK_WINDOW(fx.p_win));
   ggtest_drain_main(300);

   gboolean b_stop = FALSE;
   g_signal_emit_by_name(fx.p_win, "close-request", &b_stop);
   g_assert_true(b_stop);
   answer_prompt(&fx, "Save");

   char *c_out = g_build_filename(fx.c_dir, "plain-enhanced.jpg", NULL);
   wait_for_file(c_out);
   ggtest_drain_main(300); /* the gate's quit continuation runs after it */
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
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-closefail-XXXXXX");
   gtk_window_present(GTK_WINDOW(fx.p_win));
   ggtest_drain_main(300);
   g_assert_cmpint(g_chmod(fx.c_dir, 0500), ==, 0); /* r-x: no new files */

   gboolean b_stop = FALSE;
   g_signal_emit_by_name(fx.p_win, "close-request", &b_stop);
   g_assert_true(b_stop);
   answer_prompt(&fx, "Save");
   wait_for_status_prefix(fx.p_win, "Enhance-save failed"); /* worker done */
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
   DirtyFixture fx = {0};
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
    * miss path.
    *
    * The `!= fx.p_orig` below is why DirtyFixture owns a ref on p_orig: the
    * clear above drops the cache's last reference, and without the fixture's
    * ref this would compare a live pointer with a dangling one -- true only
    * for as long as the reload's texture misses the freed address. */
   ggaze_window_set_hold_original(fx.p_win, TRUE);
   /* p_reloaded is the one BORROWED texture local left in this file, and it is
    * correct only because nothing between this line and the last comparison
    * three lines down iterates the main loop -- so nothing can drop the
    * viewer's reference underneath it. Putting a drain in that gap would
    * restore exactly the dangling comparison 3w0 removed; take an owned ref
    * through ref_viewer_texture() instead, as the rest of the file does. */
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
   DirtyFixture             fx      = {0};
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
   DirtyFixture             fx      = {0};
   static const char *const c_two[] = {"plain.jpg", "rot6.jpg", NULL};
   fixture_open_clean(&fx, "ggaze-enhance-delprune-XXXXXX", c_two);
   mark_file_and_return(&fx, "rot6.jpg");
   fixture_make_dirty(&fx);

   fire(fx.p_win, "win.delete");
   ggtest_drain_main(150);
   GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(fx.p_win), "Cancel");

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
   DirtyFixture             fx       = {0};
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
   GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(fx.p_win), "Cancel");
   remove_behind_the_prompt(&fx, "rgba.png");
   remove_behind_the_prompt(&fx, "rot6.jpg");
   assert_marked_count(fx.p_win, 1); /* pruned 3 -> 1 */

   /* Answered by hand rather than through answer_prompt(): the confirm
    * dialog this must raise has a "Cancel" button of its own, so the
    * "no dialog remains" assertion there would fire on the very thing this
    * subtest is looking for. The GGTEST_ASSERT_DIALOG_UP() ahead of each
    * click is what answer_prompt() does for its own click, and it is not
    * decoration here: the 3 s of draining the two remove_behind_the_prompt()
    * calls above spend with the prompt on screen is where 8w0's flake was
    * reproduced, and ggtest_click_dialog_button()'s bare FALSE says nothing
    * about why. */
   GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(fx.p_win), "Discard");
   g_assert_true(ggtest_click_dialog_button(GTK_WINDOW(fx.p_win), "Discard"));
   ggtest_drain_main(400);

   /* The >1-mark confirm must still be raised, for the captured three. */
   GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(fx.p_win), "Delete");
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
   DirtyFixture             fx        = {0};
   static const char *const c_three[] = {"plain.jpg", "rgba.png", "rot6.jpg",
                                         NULL};
   fixture_open_clean(&fx, "ggaze-enhance-delnav-XXXXXX", c_three);
   fixture_make_dirty(&fx);

   fire(fx.p_win, "win.delete"); /* `D` on plain.jpg, nothing marked */
   ggtest_drain_main(150);
   GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(fx.p_win), "Cancel");
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
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-gonetrash-XXXXXX");

   fire(fx.p_win, "win.trash"); /* `d`, captured on plain.jpg */
   ggtest_drain_main(150);
   GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(fx.p_win), "Cancel");
   remove_behind_the_prompt(&fx, "plain.jpg");

   answer_prompt(&fx, "Discard");
   ggtest_drain_main(200);

   g_assert_cmpstr(
      gtk_label_get_text(GTK_LABEL(ggaze_window_get_info_label(fx.p_win))), ==,
      "Nothing trashed — the file is gone");
   /* Nothing was binned, so the lazy bin was never created. The name is
    * ".Trash" (trash.c _trash_dir), not "Trash" -- as written before (4w0)
    * this assertion could never have failed whatever the code did. */
   g_assert_false(file_exists_in(fx.c_dir, ".Trash"));
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
   DirtyFixture fx = {0};
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
   ggtest_cleanup_temp_dir(c_a);
   ggtest_cleanup_temp_dir(c_b);
}

/* Round 4, finding (r): when the answered prompt's OWN continuation is the
 * quit, the flushed request used to run against a window gtk_window_close()
 * had already taken down -- _open_now building a fresh Navigator,
 * GFileMonitor and texture load inside it. A closing window can honour no
 * request, so the queue is dropped instead. */
static void
test_quit_continuation_drops_the_queued_request(void) {
   DirtyFixture fx = {0};
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
   ggtest_cleanup_temp_dir(c_other);
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

   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-movecapsrc-XXXXXX");
   click_move_row(&fx); /* captures plain.jpg, raises the prompt */
   GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(fx.p_win), "Cancel");
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
   ggtest_cleanup_temp_dir(c_dst);
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

   DirtyFixture             fx        = {0};
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
   ggtest_cleanup_temp_dir(c_dst);
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

   DirtyFixture             fx        = {0};
   static const char *const c_three[] = {"plain.jpg", "rgba.png", "rot6.jpg",
                                         NULL};
   fixture_open_clean(&fx, "ggaze-enhance-movecursrc-XXXXXX", c_three);
   assert_showing(fx.p_win, "plain.jpg");

   g_assert_true(ggaze_window_move_index(fx.p_win, 0));
   /* Checked BEFORE the main loop gets a turn: once the folder monitor's
    * relist runs it clamps the cursor onto rgba.png by index anyway (the
    * successor slides into the vacated slot), so only the state the
    * synchronous move leaves behind can tell a real advance apart from a
    * cursor left parked on the file that just left. */
   assert_showing(fx.p_win, "rgba.png"); /* advanced past it at once */
   ggtest_drain_main(300);

   g_assert_true(file_exists_in(c_dst, "plain.jpg")); /* the current one */
   assert_showing(fx.p_win, "rgba.png");              /* and stayed there */

   fixture_teardown(&fx);
   reset_destinations();
   ggtest_cleanup_temp_dir(c_dst);
}

/* --- 4w0: `d`'s cursor advance, both directions --------------------------
 *
 * The last leg of the destructive trio with no cursor coverage at all: the
 * round-3/4 trash subtests assert WHICH file was binned, never where the
 * cursor ended up, so _do_trash_now's advance could be deleted outright (or
 * its `if (b_was_current)` gate dropped) and the whole suite stayed green --
 * the same gap that let (q)'s and (x)'s twins survive several review rounds.
 */

/* Direction one: the binned file IS the current one, so the cursor must move
 * off it. Nothing is dirty, so `d` runs its continuation synchronously
 * (_maybe_save_then's "no preview" fast path calls fn straight away), which
 * is what makes the pre-loop assertion below possible. */
static void
test_trash_advances_when_the_current_image_is_binned(void) {
   DirtyFixture             fx        = {0};
   static const char *const c_three[] = {"plain.jpg", "rgba.png", "rot6.jpg",
                                         NULL};
   fixture_open_clean(&fx, "ggaze-enhance-trashcur-XXXXXX", c_three);
   assert_showing(fx.p_win, "plain.jpg");

   fire(fx.p_win, "win.trash"); /* `d` on plain.jpg; clean, so no prompt */
   /* Checked BEFORE the main loop gets a turn, exactly as the move twin
    * above. Once the folder monitor's relist runs, _relist keeps current by
    * path and -- plain.jpg having left the folder -- clamps the OLD index
    * into the new listing, so rgba.png slides into the vacated slot and the
    * monitor reproduces this very title on its own. Only the state the
    * synchronous trash leaves behind tells a real advance apart from a
    * cursor left parked on the file that just went to the bin. */
   assert_showing(fx.p_win, "rgba.png"); /* advanced past it at once */
   ggtest_drain_main(300);

   g_assert_false(file_exists_in(fx.c_dir, "plain.jpg")); /* binned ... */
   char *c_bin = g_build_filename(fx.c_dir, ".Trash", NULL);
   g_assert_true(file_exists_in(c_bin, "plain.jpg")); /* ... into ./.Trash */
   g_free(c_bin);
   assert_showing(fx.p_win, "rgba.png"); /* and the cursor stayed */

   fixture_teardown(&fx);
}

/* Direction two: the `if (b_was_current)` gate itself. `d` captures
 * plain.jpg, the slideshow tick moves current on to rgba.png behind the
 * prompt, Discard bins plain.jpg -- and the cursor must STAY on rgba.png,
 * which the user has only just been shown.
 *
 * No pre-loop assertion is possible here (the trash runs inside the alert
 * dialog's async callback, which only answer_prompt's own drain gets to), and
 * none is needed: an unconditional navigator_next() takes the cursor to
 * rot6.jpg, and the relist does NOT put it back -- rot6.jpg is still listed,
 * so _relist keeps current by path and leaves it there. The trash twin of
 * test_delete_does_not_advance_past_an_unseen_image. */
static void
test_trash_does_not_advance_past_an_unseen_image(void) {
   DirtyFixture             fx        = {0};
   static const char *const c_three[] = {"plain.jpg", "rgba.png", "rot6.jpg",
                                         NULL};
   fixture_open_clean(&fx, "ggaze-enhance-trashnav-XXXXXX", c_three);
   fixture_make_dirty(&fx);

   fire(fx.p_win, "win.trash"); /* `d` on plain.jpg, nothing marked */
   ggtest_drain_main(150);
   GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(fx.p_win), "Cancel");
   ggaze_window_next(fx.p_win); /* what the slideshow tick does */
   ggtest_drain_main(300);
   assert_showing(fx.p_win, "rgba.png");

   answer_prompt(&fx, "Discard");
   ggtest_drain_main(300);

   g_assert_false(file_exists_in(fx.c_dir, "plain.jpg")); /* the captured one */
   assert_showing(fx.p_win, "rgba.png"); /* and rot6.jpg was NOT skipped */

   fixture_teardown(&fx);
}

/* --- 2w0: disposing the window out from under a live prompt --------------
 *
 * The branch round 4 had to leave uncovered. Driving _enhance_dispose's "a
 * request is still parked" leg needs a dispose while the GtkAlertDialog is up,
 * and until 2w0 that abandoned the dialog's GTask: the task never completed,
 * so _save_dialog_cb never ran and the _SaveCtx (plus the _FileCtx/_OpenCtx it
 * carries) leaked -- 479 bytes in 11 allocations, measured by running exactly
 * this subtest against the pre-fix window.c (tu0 measured 474/11 with its own
 * slightly different version of it).
 *
 * g_object_run_dispose() is not a shortcut here, it is the only way in.
 * gtk_window_destroy() alone cannot dispose the window while a prompt is up:
 * the _SaveCtx holds an owned window ref, so the refcount never reaches zero
 * (re-measured for THIS subtest's fixture: 4 -> 3 on the destroy, then holding
 * at 3 across 5 s of draining with the dialog still listed and still
 * answerable), and GTK's destroy-with-parent is wired to the parent's
 * ::destroy, which is itself emitted from dispose -- so the dialog survives
 * too.
 *
 * Those absolutes are this fixture's, not a property of the destroy: they
 * count whatever contexts happen to be outstanding (a grid-select prompt
 * parks a _FileCtx too and gives 4 -> 3; a win.next prompt gives 3 -> 2).
 * What generalises is the ONE ref gtk_window_destroy() drops -- the toplevel
 * list's -- which is why any context still holding one keeps the window off
 * zero.
 *
 * That forced dispose is therefore the only thing this subtest shares with
 * production, and it is a narrow thing to share: nothing in src/ calls
 * g_object_run_dispose(), so what is covered here is the safety of a forced
 * dispose, not a shutdown path ggaze itself takes. The reachable variant --
 * a native close arriving with the prompt up -- is
 * test_close_request_blocked_while_prompt_is_up below.
 *
 * The parked request is deliberately an ggaze_window_open: its _OpenCtx drops
 * an owned window ref from inside the window's own dispose, which is the
 * ordering _enhance_dispose clears the slot before releasing for. */
static void
test_dispose_under_a_live_prompt_releases_it(void) {
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-disposeprompt-XXXXXX");
   GError *p_err = NULL;
   char   *c_other =
      g_dir_make_tmp("ggaze-enhance-disposeprompt2-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_other, "small.png");
   char  *c_target = g_build_filename(c_other, "small.png", NULL);
   GFile *p_target = g_file_new_for_path(c_target);

   activate_other_cell(&fx);              /* raises the prompt (a _FileCtx) */
   ggaze_window_open(fx.p_win, p_target); /* parked behind it (an _OpenCtx) */
   ggtest_drain_main(200);
   GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(fx.p_win), "Cancel");
   /* Three contexts now hold a window ref each, on top of the count the
    * fixture settled at while merely dirty: the prompt's own _SaveCtx, the
    * _FileCtx carrying the deferred grid select it gates, and the parked
    * _OpenCtx. */
   g_assert_cmpuint(((GObject *)fx.p_win)->ref_count, ==, fx.u_ref + 3);

   g_object_run_dispose(G_OBJECT(fx.p_win));
   ggtest_drain_main(400);

   /* Nothing is left on screen. This one does NOT discriminate: GTK takes the
    * dialog window down either way, because dispose emits the ::destroy that
    * its destroy-with-parent is wired to. It is here to pin down that the
    * ref taken in _save_prompt_show does not keep a dead dialog visible. */
   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    0);
   /* THIS is the assertion the fix is answerable to: the dialog's GTask
    * completed, so _save_dialog_cb ran and released the _SaveCtx and the
    * _FileCtx it gated. Without the cancel both were simply abandoned and the
    * count stopped at u_ref + 2 (measured 4 against a wanted 2), taking 479
    * bytes in 11 allocations down with them under ASan. Only the parked
    * _OpenCtx came back on its own -- _enhance_dispose drops that one. */
   g_assert_cmpuint(((GObject *)fx.p_win)->ref_count, ==, fx.u_ref);
   /* The queued open never ran: a disposed window honours no request. */
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "small.png"));

   g_object_unref(p_target);
   g_free(c_target);
   fixture_teardown(&fx);
   ggtest_cleanup_temp_dir(c_other);
}

/* 2w0 review finding (A): "close-request is prompt-gated" was FALSE.
 * _on_close_request used to consult only the dirty mask, so once something
 * cleared that mask BEHIND the modal dialog -- the slideshow tick and the
 * folder's GFileMonitor both keep running behind an input-only modal grab,
 * and either can move navigator.current -- the very next Alt+F4 or WM close
 * button (not input events, so the grab does not swallow them) propagated
 * straight through to gtk_window_destroy() with the prompt still up. That
 * destroy cannot dispose the window (it drops the toplevel list's ref, and the
 * _SaveCtx's own window ref keeps the count off zero -- see
 * test_dispose_under_a_live_prompt_releases_it above for the measurement and
 * why the absolute numbers are a fixture property), so the cancel in
 * _prompt_dispose never runs, the dialog is orphaned on screen and every ctx
 * it carries is abandoned.
 *
 * ggaze_window_next() stands in for the timer/monitor: it is the same
 * navigation choke point they use (nav_changed_cb -> _enhance_nav_changed
 * clears the mask), and it is what test_trash_does_not_advance_past_an_-
 * unseen_image already uses for the slideshow tick.
 *
 * Both close entry points are exercised deliberately: the raw signal emission
 * pins the handler's own verdict, and gtk_window_close() -- which needs the
 * presented toplevel, being a no-op on an unrealized window -- is the path
 * Alt+F4 actually takes and the one that would destroy the window. */
static void
test_close_request_blocked_while_prompt_is_up(void) {
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-closeundeprompt-XXXXXX");
   gtk_window_present(GTK_WINDOW(fx.p_win));
   ggtest_drain_main(300);

   fire(fx.p_win, "win.next"); /* raises the prompt */
   ggtest_drain_main(150);
   GGTEST_ASSERT_DIALOG_UP(GTK_WINDOW(fx.p_win), "Cancel");

   ggaze_window_next(fx.p_win); /* what the slideshow tick does */
   ggtest_drain_main(300);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win)); /* mask gone... */
   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    1); /* ...but the prompt is still up */

   gboolean b_stop = FALSE;
   g_signal_emit_by_name(fx.p_win, "close-request", &b_stop);
   g_assert_true(b_stop); /* the handler must still block the close */

   gtk_window_close(GTK_WINDOW(fx.p_win)); /* the real Alt+F4 path */
   ggtest_drain_main(300);
   g_assert_true(ggtest_is_open_toplevel(GTK_WINDOW(fx.p_win)));
   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    1); /* not orphaned: still parented, still answerable */

   /* Blocking is only half of it -- the user must keep a way out. The close
    * was queued behind the prompt, so answering it in favour of proceeding
    * flushes that queued quit through the (now clean) gate and the window
    * really closes, rather than the close being silently swallowed. */
   answer_prompt(&fx, "Discard");
   ggtest_drain_main(300);
   g_assert_false(ggtest_is_open_toplevel(GTK_WINDOW(fx.p_win)));

   fixture_teardown(&fx); /* the fixture ref is what keeps this valid */
}

/* --- side panel + saved state (the redesign) ------------------------------ */

/* `s` on a dirty preview exports the copy AND clears dirty: the preview stays
 * on screen (mask untouched, the enhanced texture still displayed), but
 * navigating away no longer prompts -- the work is on disk. Touching the
 * mask makes it dirty; coming back to exactly the saved combination makes
 * it saved again, because the file on disk IS that combination whichever
 * way it was reached (the wb2 review reversed the earlier "one exact
 * export, not a memo" rule: a tool cancelled back to the saved state must
 * not prompt for work that is already written). */
static void
test_manual_save_clears_dirty_until_next_change(void) {
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-saved-XXXXXX");
   fire(fx.p_win, "win.enhance-save");
   char *c_out = g_build_filename(fx.c_dir, "plain-enhanced.jpg", NULL);
   wait_for_file(c_out);
   wait_for_status_prefix(fx.p_win, "Saved");
   g_free(c_out);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_mod); /* still on screen */

   /* Another preset on top: dirty again ... */
   fire(fx.p_win, "win.enhance-2");
   wait_for_texture_change(fx.p_win, fx.p_mod);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   /* ... and back to exactly the saved combination is saved again. */
   GdkTexture *p_two = ref_viewer_texture(fx.p_win);
   fire(fx.p_win, "win.enhance-2");
   wait_for_texture_change(fx.p_win, p_two);
   g_object_unref(p_two);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));

   /* Save once more, then move on: no prompt, the other image simply shows.
    * win.last rather than win.next, because the exported copies land in the
    * same folder and sort right after plain.jpg once the monitor's rescan
    * has picked them up -- whether it has by now is a race this subtest is
    * not about. */
   fire(fx.p_win, "win.enhance-save");
   char *c_out2 = g_build_filename(fx.c_dir, "plain-enhanced-1.jpg", NULL);
   wait_for_file(c_out2);
   wait_for_status_prefix(fx.p_win, "Saved");
   g_free(c_out2);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   fire(fx.p_win, "win.last");
   ggtest_drain_main(400);
   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    0);
   assert_showing(fx.p_win, "rot6.jpg");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   fixture_teardown(&fx);
}

/* 6i2: Esc NEVER discards an edit. With the panel open it closes the panel
 * and keeps the preview (saying so); the next Esc goes on to the grid --
 * still keeping it -- where it used to drop the preview without a prompt.
 * Back in the large view the edit is on screen and dirty as before. */
static void
test_esc_never_discards_the_edit(void) {
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-esc-XXXXXX");
   fire(fx.p_win, "win.enhance");
   g_assert_nonnull(find_panel(fx.p_win));
   fire(fx.p_win, "win.back");
   g_assert_null(find_panel(fx.p_win));
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_mod);
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Edit panel closed"));
   fire(fx.p_win, "win.back"); /* large -> grid, nothing discarded */
   ggtest_drain_main(200);
   g_assert_cmpstr(
      gtk_stack_get_visible_child_name(ggaze_window_get_stack(fx.p_win)), ==,
      "grid");
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   fire(fx.p_win, "win.toggle-view"); /* back: the edit is still there */
   ggtest_drain_main(200);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_mod);
   fixture_teardown(&fx);
}

/* 6i2: `0` is zoom and nothing else -- with the panel open it toggles fit /
 * 100% like anywhere, the edit and the panel stay. x is the revert: with
 * the panel closed it only says where it works; with it open it drops
 * every edit (status line), the panel staying up for the next attempt. */
static void
test_zero_zooms_and_x_reverts(void) {
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-zero-XXXXXX");
   fire(fx.p_win, "win.edit-revert"); /* panel closed: explains, keeps */
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "x reverts"));
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   fire(fx.p_win, "win.enhance");
   GtkWidget *p_panel = find_panel(fx.p_win);
   g_assert_nonnull(p_panel);
   fire(fx.p_win, "win.zoom-reset"); /* a zoom toggle, nothing else */
   fire(fx.p_win, "win.zoom-reset");
   ggtest_drain_main(200);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win)); /* kept */
   g_assert_true(viewer_texture(fx.p_win) == fx.p_mod);
   fire(fx.p_win, "win.edit-revert");
   ggtest_drain_main(200);
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Reverted"));
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_true(find_panel(fx.p_win) == p_panel);
   g_assert_nonnull(find_label_prefix(p_panel, "No edits yet"));
   fire(fx.p_win, "win.edit-revert"); /* nothing left */
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Nothing to revert"));
   fixture_teardown(&fx);
}

/* The panel outlives a navigation: after Discard it is the SAME widget,
 * re-pointed at the new file (its Save names the new target), with no card
 * highlighted and a fresh batch of thumbnails for the new image. It is hidden
 * (not closed) with the grid and back beside the viewer after `t` twice. */
static void
test_panel_persists_across_navigation(void) {
   Settings *p_cfg = settings_new();
   settings_set_enhance_preview_thumbnails(p_cfg, TRUE);
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-persist-XXXXXX");
   fire(fx.p_win, "win.enhance");
   GtkWidget *p_panel = find_panel(fx.p_win);
   g_assert_nonnull(p_panel);
   g_assert_nonnull(find_label_prefix(p_panel, "as plain-enhanced.jpg"));
   g_assert_true(
      gtk_widget_has_css_class(find_card(p_panel, 0), "ggaze-enhance-on"));

   fire(fx.p_win, "win.next");
   answer_prompt(&fx, "Discard");
   assert_showing(fx.p_win, "rot6.jpg");
   g_assert_true(find_panel(fx.p_win) == p_panel);
   g_assert_nonnull(find_label_prefix(p_panel, "as rot6-enhanced.jpg"));
   g_assert_false(
      gtk_widget_has_css_class(find_card(p_panel, 0), "ggaze-enhance-on"));
   GPtrArray *p_pics = g_ptr_array_new();
   collect_pictures(p_panel, p_pics);
   g_assert_cmpuint(p_pics->len, ==, 9);
   wait_for_pictures_painted(p_pics);
   g_ptr_array_unref(p_pics);

   /* The window is never presented here, so ask the panel's slot for its
    * own visible flag rather than gtk_widget_is_visible (which also needs
    * every ancestor, the toplevel included, to be shown). */
   GtkWidget *p_slot = gtk_widget_get_parent(p_panel);
   g_assert_true(gtk_widget_get_visible(p_slot));
   fire(fx.p_win, "win.toggle-view"); /* grid: the panel is hidden ... */
   ggtest_drain_main(100);
   g_assert_true(find_panel(fx.p_win) == p_panel);
   g_assert_false(gtk_widget_get_visible(p_slot));
   fire(fx.p_win, "win.toggle-view"); /* ... and back with the large view */
   ggtest_drain_main(100);
   g_assert_true(gtk_widget_get_visible(p_slot));

   g_settings_reset(settings_get_gsettings(p_cfg),
                    "enhance-preview-thumbnails");
   settings_delete(p_cfg);
   fixture_teardown(&fx);
}

/* --- wb2: the crop / straighten / rotate-90 tools ------------------------
 *
 * All drive the real window actions (win.crop / win.straighten /
 * win.rotate-cw / win.rotate-ccw), the tools' modal keys through the
 * window's public ggaze_window_tool_key hook, and pointer drags through
 * ggaze_window_tool_drag, and assert on what the viewer actually shows: the
 * texture's size after the async apply lands, the title's transform suffix,
 * the dirty flag, and the exported file. The shared fixtures are a few
 * pixels across, so these use a generated 400x300 PNG: a quarter turn of a
 * 6x3 image is visible, but a crop's 1%-step keyboard nudge is not. */

#define TOOL_W 400
#define TOOL_H 300

/* A fresh folder holding one TOOL_W x TOOL_H PNG ("tool.png"). */
static char *
make_tool_dir(char **c_path_out) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-tool-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   GdkPixbuf *p_pix =
      gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, TOOL_W, TOOL_H);
   gdk_pixbuf_fill(p_pix, 0x336699ffu);
   char *c_path = g_build_filename(c_dir, "tool.png", NULL);
   g_assert_true(gdk_pixbuf_save(p_pix, c_path, "png", &p_err, NULL));
   g_assert_no_error(p_err);
   g_object_unref(p_pix);
   *c_path_out = c_path;
   return (c_dir);
}

/* Window on tool.png, optionally presented at 900x700 with a real
 * allocation (the drag subtests map widget pixels to image pixels). */
typedef struct {
   char        *c_dir;
   char        *c_path;
   GgazeWindow *p_win;
   GdkTexture  *p_orig; /* owned ref on the original texture */
} ToolFx;

static void
tool_fx_open(ToolFx *p_fx, gboolean b_present) {
   memset(p_fx, 0, sizeof(*p_fx));
   p_fx->c_dir   = make_tool_dir(&p_fx->c_path);
   GFile *p_file = g_file_new_for_path(p_fx->c_path);
   p_fx->p_win   = new_window();
   if (b_present) {
      gtk_window_set_default_size(GTK_WINDOW(p_fx->p_win), 900, 700);
      gtk_window_present(GTK_WINDOW(p_fx->p_win));
   }
   ggaze_window_open(p_fx->p_win, p_file);
   g_object_unref(p_file);
   if (b_present) {
      wait_for_view(p_fx->p_win, TOOL_W, TOOL_H);
   } else {
      wait_for_load(p_fx->p_win, TOOL_W, TOOL_H);
   }
   p_fx->p_orig = ref_viewer_texture(p_fx->p_win);
   g_assert_false(ggaze_window_enhance_is_dirty(p_fx->p_win));
}

/* Like tool_fx_open(FALSE), plus u_siblings copies of plain.jpg (6x3) named
 * "a.jpg", "b.jpg", ... that sort BEFORE tool.png, so win.prev / win.first
 * have somewhere to go: the subtests about navigating away from a tool need
 * a target (a crop needs the 400x300 image -- the 6x3 fixture is below the
 * rectangle's minimum size), and one two positions away is what no
 * prefetch has cached. */
static void
tool_fx_open_siblings(ToolFx *p_fx, guint u_siblings) {
   memset(p_fx, 0, sizeof(*p_fx));
   p_fx->c_dir = make_tool_dir(&p_fx->c_path);
   for (guint u = 0; u < u_siblings; u++) {
      copy_fixture(p_fx->c_dir, "plain.jpg");
      char c_name[] = "a.jpg";
      c_name[0]     = (char)('a' + u);
      char *c_a     = g_build_filename(p_fx->c_dir, c_name, NULL);
      char *c_p     = g_build_filename(p_fx->c_dir, "plain.jpg", NULL);
      g_assert_cmpint(g_rename(c_p, c_a), ==, 0);
      g_free(c_a);
      g_free(c_p);
   }
   GFile *p_file = g_file_new_for_path(p_fx->c_path);
   p_fx->p_win   = new_window();
   ggaze_window_open(p_fx->p_win, p_file);
   g_object_unref(p_file);
   wait_for_load(p_fx->p_win, TOOL_W, TOOL_H); /* opened on tool.png */
   p_fx->p_orig = ref_viewer_texture(p_fx->p_win);
   g_assert_false(ggaze_window_enhance_is_dirty(p_fx->p_win));
}

/* The one-sibling form ("a.jpg" before tool.png). */
static void
tool_fx_open_with_sibling(ToolFx *p_fx) {
   tool_fx_open_siblings(p_fx, 1);
}

/* Wait until the viewer shows an i_w x i_h texture (a render or a load
 * landed with that size) on the shared scaled deadline (GGTEST_WAIT_FOR_
 * TEXTURE, which g_error()s out if it never does), then settle 50 ms and
 * assert the texture is STILL that size: a render that lands during the
 * settle and replaces it is a failure here, as it was before the shared
 * helper replaced this suite's own iteration-counted loop. */
static void
wait_for_texture_size(GgazeWindow *p_win, gint i_w, gint i_h) {
   GGTEST_WAIT_FOR_TEXTURE(p_win, i_w, i_h);
   ggtest_drain_main(50);
   GdkTexture *p_tex = viewer_texture(p_win);
   g_assert_nonnull(p_tex);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, i_w);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, i_h);
}

static void
tool_fx_close(ToolFx *p_fx) {
   gtk_window_destroy(GTK_WINDOW(p_fx->p_win));
   ggtest_drain_main(300);
   g_clear_object(&p_fx->p_orig);
   g_free(p_fx->c_path);
   ggtest_cleanup_temp_dir(p_fx->c_dir);
}

static void
assert_texture_size(GgazeWindow *p_win, gint i_w, gint i_h) {
   GdkTexture *p_tex = viewer_texture(p_win);
   g_assert_nonnull(p_tex);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, i_w);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, i_h);
}

/* Bump c_path's mtime (5 s into the future, so it differs whatever the
 * clock did): the texture cache's stamp is stale afterwards -- the next
 * load decodes a NEW texture object -- while the folder monitor, which
 * ignores attribute changes, schedules no rescan. */
static void
touch_file(const char *c_path) {
   GFile *p_f = g_file_new_for_path(c_path);
   g_assert_true(g_file_set_attribute_uint64(
      p_f, G_FILE_ATTRIBUTE_TIME_MODIFIED, (guint64)time(NULL) + 5,
      G_FILE_QUERY_INFO_NONE, NULL, NULL));
   g_object_unref(p_f);
}

/* Pump until the viewer shows a texture that is neither p_a nor p_b (a
 * reload's fresh decode of the same file, told from the texture it
 * replaced AND the original it re-decodes), up to 5 s; asserts it did. */
static void
wait_for_fresh_texture(GgazeWindow *p_win, GdkTexture *p_a, GdkTexture *p_b) {
   for (guint u = 0; u < 5000; u++) {
      GdkTexture *p_tex = viewer_texture(p_win);
      if (p_tex != NULL && p_tex != p_a && p_tex != p_b) {
         break;
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   ggtest_drain_main(50);
   GdkTexture *p_tex = viewer_texture(p_win);
   g_assert_nonnull(p_tex);
   g_assert_true(p_tex != p_a && p_tex != p_b);
}

/* Rewrite c_path in place as a 200x150 PNG of another colour: another size
 * AND another byte count, so the cache's stamp misses on the byte count
 * alone, whatever the clock did, and the folder monitor's rescan follows
 * (a content change, unlike touch_file's). */
static void
rewrite_as_200x150(const char *c_path) {
   GdkPixbuf *p_pix = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 200, 150);
   gdk_pixbuf_fill(p_pix, 0x99cc33ffu);
   GError *p_err = NULL;
   g_assert_true(gdk_pixbuf_save(p_pix, c_path, "png", &p_err, NULL));
   g_assert_no_error(p_err);
   g_object_unref(p_pix);
}

/* Fire c_action and wait for the async preview to replace the texture. */
static void
fire_and_wait(GgazeWindow *p_win, const char *c_action) {
   GdkTexture *p_before = ref_viewer_texture(p_win);
   fire(p_win, c_action);
   wait_for_texture_change(p_win, p_before);
   g_object_unref(p_before);
}

/* Press a modal tool key (no modifiers) and assert the tool consumed it. */
static void
tool_key(GgazeWindow *p_win, guint u_keyval) {
   g_assert_true(ggaze_window_tool_key(p_win, u_keyval, 0));
}

/* Crop tool, 6i2 keys: Ctrl+l moves the right side in, Ctrl+j the bottom
 * side up, by one step (1% of the base's shorter side) -- what H / K did
 * before the four sides became symmetric. */
static void
crop_shrink_right(GgazeWindow *p_win) {
   g_assert_true(ggaze_window_tool_key(p_win, GDK_KEY_l, GDK_CONTROL_MASK));
}

static void
crop_shrink_bottom(GgazeWindow *p_win) {
   g_assert_true(ggaze_window_tool_key(p_win, GDK_KEY_j, GDK_CONTROL_MASK));
}

/* Crop tool: lock 1:1 (`a` once from a fresh tool: free -> 1:1), what
 * the `1` key did before 6i2. */
static void
crop_square(GgazeWindow *p_win) {
   tool_key(p_win, GDK_KEY_a);
}

/* Press a modal key that re-renders the preview, and wait for it. */
static void
tool_key_and_wait(GgazeWindow *p_win, guint u_keyval) {
   GdkTexture *p_before = ref_viewer_texture(p_win);
   tool_key(p_win, u_keyval);
   wait_for_texture_change(p_win, p_before);
   g_object_unref(p_before);
}

static const char *
status_text(GgazeWindow *p_win) {
   return (gtk_label_get_text(GTK_LABEL(ggaze_window_get_info_label(p_win))));
}

/* `]` turns the preview a quarter clockwise each press, the title says so,
 * and the fourth press is the original again (not dirty, cache hit). */
static void
test_rotate_cw_repeats_to_the_original(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire_and_wait(fx.p_win, "win.rotate-cw");
   assert_texture_size(fx.p_win, TOOL_H, TOOL_W);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "90° CW"));
   fire_and_wait(fx.p_win, "win.rotate-cw");
   assert_texture_size(fx.p_win, TOOL_W, TOOL_H);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "180°"));
   fire_and_wait(fx.p_win, "win.rotate-cw");
   assert_texture_size(fx.p_win, TOOL_H, TOOL_W);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "90° CCW"));
   fire_and_wait(fx.p_win, "win.rotate-cw");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "°"));
   tool_fx_close(&fx);
}

/* `[` then `s`: the exported copy is the turned image, the original file is
 * untouched, and the saved preview is no longer dirty. */
static void
test_rotate_ccw_save_exports_the_turned_copy(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   gsize u_len;
   char *c_before = load_bytes(fx.c_path, &u_len);
   fire_and_wait(fx.p_win, "win.rotate-ccw");
   assert_texture_size(fx.p_win, TOOL_H, TOOL_W);
   fire(fx.p_win, "win.enhance-save");
   char *c_out = g_build_filename(fx.c_dir, "tool-enhanced.png", NULL);
   wait_for_file(c_out);
   wait_for_status_prefix(fx.p_win, "Saved ");
   GError     *p_err = NULL;
   GdkTexture *p_tex = gdk_texture_new_from_filename(c_out, &p_err);
   g_assert_no_error(p_err);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, TOOL_H);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, TOOL_W);
   g_object_unref(p_tex);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win)); /* saved */
   gsize u_after;
   char *c_after = load_bytes(fx.c_path, &u_after);
   g_assert_cmpuint(u_after, ==, u_len);
   g_assert_cmpint(memcmp(c_before, c_after, u_len), ==, 0);
   g_free(c_before);
   g_free(c_after);
   g_free(c_out);
   tool_fx_close(&fx);
}

/* A preset and a turn compose in one preview and one export. */
static void
test_rotate_composes_with_a_preset(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire_and_wait(fx.p_win, "win.enhance-1");
   fire_and_wait(fx.p_win, "win.rotate-cw");
   assert_texture_size(fx.p_win, TOOL_H, TOOL_W);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "Auto-fix"));
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "90° CW"));
   fire(fx.p_win, "win.enhance-save");
   char *c_out = g_build_filename(fx.c_dir, "tool-enhanced.png", NULL);
   wait_for_file(c_out);
   wait_for_status_prefix(fx.p_win, "Saved ");
   GError     *p_err = NULL;
   GdkTexture *p_tex = gdk_texture_new_from_filename(c_out, &p_err);
   g_assert_no_error(p_err);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, TOOL_H);
   g_object_unref(p_tex);
   g_free(c_out);
   tool_fx_close(&fx);
}

/* A dirty turn is gated like a dirty preset: navigating away prompts,
 * Cancel keeps the turned preview, Discard moves on with the original. */
static void
test_dirty_rotate_gates_navigation(void) {
   DirtyFixture             fx      = {0};
   static const char *const c_two[] = {"plain.jpg", "rot6.jpg", NULL};
   fixture_open_clean(&fx, "ggaze-tool-gate-XXXXXX", c_two);
   fx.p_orig = ref_viewer_texture(fx.p_win);
   fire(fx.p_win, "win.rotate-cw");
   wait_for_texture_change(fx.p_win, fx.p_orig);
   fx.p_mod = ref_viewer_texture(fx.p_win);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   fx.u_ref = ((GObject *)fx.p_win)->ref_count;
   fire(fx.p_win, "win.next");
   answer_prompt(&fx, "Cancel");
   assert_showing(fx.p_win, "plain.jpg");
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_mod);
   assert_ref_settled(&fx);
   fire(fx.p_win, "win.next");
   answer_prompt(&fx, "Discard");
   assert_showing(fx.p_win, "rot6.jpg");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "°"));
   fixture_teardown(&fx);
}

/* `c` opens the crop tool over the unchanged image (the identity transform
 * needs no re-render), H/K shrink the rectangle by 1% steps, Enter commits
 * the crop and leaves the tool, and the preview is the cropped size. */
static void
test_crop_keys_then_enter_commits(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.crop");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Crop"));
   for (guint u = 0; u < 10; u++) {
      crop_shrink_right(fx.p_win);  /* right edge in: 3 px per press */
      crop_shrink_bottom(fx.p_win); /* bottom edge up */
   }
   /* h/l/j/k move the rectangle, so they are consumed: none of these may
    * reach win.prev / win.next / the pan actions. */
   tool_key(fx.p_win, GDK_KEY_l);
   tool_key(fx.p_win, GDK_KEY_h);
   tool_key(fx.p_win, GDK_KEY_j);
   tool_key(fx.p_win, GDK_KEY_k);
   g_assert_false(ggaze_window_tool_key(fx.p_win, GDK_KEY_x, 0)); /* not ours */
   g_assert_false(ggaze_window_tool_key(fx.p_win, GDK_KEY_q, GDK_CONTROL_MASK));
   GdkTexture *p_before = ref_viewer_texture(fx.p_win);
   tool_key(fx.p_win, GDK_KEY_Return);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   /* Said at once; the landed preview's own hint replaces it later. */
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Cropped"));
   wait_for_texture_change(fx.p_win, p_before);
   g_object_unref(p_before);
   assert_texture_size(fx.p_win, TOOL_W - 30, TOOL_H - 30);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "crop"));
   tool_fx_close(&fx);
}

/* Esc in the crop tool restores what the tool started from; `c` again
 * while it is active is the same cancel; the untouched rectangle commits
 * as "no crop". */
static void
test_crop_esc_and_toggle_cancel(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.crop");
   crop_shrink_right(fx.p_win);
   tool_key(fx.p_win, GDK_KEY_Escape);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_cmpstr(status_text(fx.p_win), ==, "Crop cancelled");
   fire(fx.p_win, "win.crop");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   fire(fx.p_win, "win.crop"); /* the toggle */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   fire(fx.p_win, "win.crop");
   tool_key(fx.p_win, GDK_KEY_Return); /* the whole image: no crop */
   ggtest_drain_main(100);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Crop removed"));
   tool_fx_close(&fx);
}

/* yb2 review finding 8: the crop tool over an animated GIF holds its FIRST
 * frame -- the texture the rectangle is laid out on and Enter applies to
 * -- for as long as the tool is up, and Esc lets the animation play on.
 * The identity transform needs no render, so the original (the animated
 * texture) stays on screen under the tool, which is exactly the case the
 * hold exists for. Leaving by APPLYING releases the hold too (second
 * review, finding 5): Enter on the untouched rectangle commits "no crop",
 * which renders nothing, so the animated original stays up and must play
 * on rather than stay frozen on frame 1. Presented: an unmapped viewer
 * plays nothing anyway. */
static void
test_crop_tool_holds_animation_first_frame(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-tool-anim-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "anim.gif");
   char        *c_path = g_build_filename(c_dir, "anim.gif", NULL);
   GFile       *p_file = g_file_new_for_path(c_path);
   GgazeWindow *p_win  = new_window();
   gtk_window_set_default_size(GTK_WINDOW(p_win), 600, 400);
   gtk_window_present(GTK_WINDOW(p_win));
   ggaze_window_open(p_win, p_file);
   g_object_unref(p_file);
   g_free(c_path);
   GgazeViewer *p_v    = GGTEST_WAIT_FOR_VIEW(p_win, 8, 6);
   GdkTexture  *p_orig = viewer_texture(p_win);
   g_assert_true(ggaze_viewer_is_animating(p_v));

   fire(p_win, "win.crop");
   g_assert_cmpint(ggaze_window_get_tool(p_win), ==, GGAZE_TOOL_CROP);
   g_assert_true(viewer_texture(p_win) == p_orig);
   g_assert_false(ggaze_viewer_is_animating(p_v));
   g_assert_true(ggaze_viewer_get_frame(p_v) == p_orig);
   ggtest_drain_main(250); /* more than two of the fixture's 100 ms frames */
   g_assert_false(ggaze_viewer_is_animating(p_v));
   g_assert_true(ggaze_viewer_get_frame(p_v) == p_orig);

   tool_key(p_win, GDK_KEY_Escape);
   g_assert_cmpint(ggaze_window_get_tool(p_win), ==, GGAZE_TOOL_NONE);
   g_assert_true(ggaze_viewer_is_animating(p_v));

   fire(p_win, "win.crop");
   g_assert_false(ggaze_viewer_is_animating(p_v));
   tool_key(p_win, GDK_KEY_Return); /* the whole image: no crop */
   g_assert_cmpint(ggaze_window_get_tool(p_win), ==, GGAZE_TOOL_NONE);
   ggtest_drain_main(100);
   g_assert_true(viewer_texture(p_win) == p_orig);
   g_assert_true(ggaze_viewer_is_animating(p_v));
   gtk_window_destroy(GTK_WINDOW(p_win));
   ggtest_drain_main(300);
   ggtest_cleanup_temp_dir(c_dir);
}

/* The crop rectangle as the overlay has it now (asserts it is laid out). */
static CropRect
crop_rect_now(GgazeWindow *p_win) {
   CropRect r;
   gint     i_bw, i_bh;
   g_assert_true(ggaze_window_tool_crop_rect(p_win, &r, &i_bw, &i_bh));
   return (r);
}

/* 6i2: `a` cycles the aspect lock -- free -> 1:1 -> 3:2 -> 4:3 -> 16:9 ->
 * original -> free -- each the largest such rectangle, centred, inside
 * the rectangle the user left before the run of presses (not inside the
 * previous lock's result, which shrank it on every press), and back at
 * "free" that rectangle again; the status line names the lock and the
 * next one. The FIRST press visibly changes the whole-image rectangle the
 * tool starts on (it used to pick "original", which on the whole image
 * changed nothing and looked dead). The digits are no aspect keys any
 * more (`0` is zoom, 1-8 the panel's presets). */
static void
test_crop_aspect_presets(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.crop");
   g_assert_false(ggaze_window_tool_key(fx.p_win, GDK_KEY_1, 0));
   g_assert_false(ggaze_window_tool_key(fx.p_win, GDK_KEY_0, 0));
   g_assert_cmpfloat(crop_rect_now(fx.p_win).d_w, ==, TOOL_W);
   tool_key(fx.p_win, GDK_KEY_a); /* 1:1: a 400x300 image's width shrinks */
   g_assert_cmpstr(status_text(fx.p_win), ==,
                   "Crop aspect: 1:1 (a: next is 3:2)");
   g_assert_cmpfloat(crop_rect_now(fx.p_win).d_w, ==, TOOL_H);
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, TOOL_H, TOOL_H); /* 300x300 square */
   fire(fx.p_win, "win.crop"); /* re-opens on the committed rectangle ... */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   for (guint u = 0; u < 4; u++) {
      tool_key(fx.p_win, GDK_KEY_a); /* 1:1, 3:2, 4:3, 16:9 */
   }
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Crop aspect: 16:9"));
   CropRect t_wide = crop_rect_now(fx.p_win); /* inside the square */
   g_assert_cmpfloat(t_wide.d_w, ==, TOOL_H);
   g_assert_cmpfloat_with_epsilon(t_wide.d_h, TOOL_H * 9.0 / 16.0, 1e-6);
   tool_key(fx.p_win, GDK_KEY_a); /* original: the 4:3 image's own shape */
   g_assert_cmpstr(status_text(fx.p_win), ==,
                   "Crop aspect: original (a: next is free)");
   g_assert_cmpfloat(crop_rect_now(fx.p_win).d_w, ==, TOOL_H);
   g_assert_cmpfloat(crop_rect_now(fx.p_win).d_h, ==, TOOL_H * 3.0 / 4.0);
   tool_key(fx.p_win, GDK_KEY_a); /* free: the square it started from */
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Crop aspect: free"));
   g_assert_cmpfloat(crop_rect_now(fx.p_win).d_h, ==, TOOL_H);
   for (guint u = 0; u < 4; u++) {
      tool_key(fx.p_win, GDK_KEY_a); /* round again to 16:9 */
   }
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, TOOL_H, (gint)(TOOL_H * 9.0 / 16.0));
   tool_fx_close(&fx);
}

/* A pointer drag on the presented window: the bottom-right corner dragged
 * inward resizes, a drag inside moves; both in image pixels through the
 * viewer's geometry. */
static void
test_crop_drag_resizes_and_moves(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.crop");
   GgazeViewer *p_v = GGAZE_VIEWER(
      gtk_stack_get_child_by_name(ggaze_window_get_stack(fx.p_win), "large"));
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   g_assert_cmpint(g.i_img_w, ==, TOOL_W);
   gdouble d_s = g.d_scale;
   /* Corner (400,300) -> (300,200): the rectangle becomes 300x200. */
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN,
                          g.d_x + TOOL_W * d_s, g.d_y + TOOL_H * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_UPDATE, g.d_x + 300 * d_s,
                          g.d_y + 200 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 300 * d_s,
                          g.d_y + 200 * d_s);
   /* Inside (150,100) -> (200,140): moved by (50,40); the size stays. */
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN, g.d_x + 150 * d_s,
                          g.d_y + 100 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 200 * d_s,
                          g.d_y + 140 * d_s);
   /* A drag that starts outside the rectangle does nothing. */
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN, g.d_x + 390 * d_s,
                          g.d_y + 290 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 100 * d_s,
                          g.d_y + 100 * d_s);
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, 300, 200);
   /* The pan gesture is back once the tool is gone: a drag now is a pan. */
   gdouble d_px, d_py;
   ggaze_viewer_get_pan(p_v, &d_px, &d_py);
   g_assert_cmpfloat(d_px, ==, 0.0);
   tool_fx_close(&fx);
}

/* `r`: h/l nudge by half a degree and render live with the auto-crop
 * (smaller than the image), `a` turns auto-crop off (the padded bounding
 * box is larger; `A` did before 6i2 and is no tool key now), Enter keeps
 * it, and the title names the angle. Esc outside the tool keeps the edit
 * (6i2): only x reverts it. */
static void
test_straighten_nudge_and_autocrop(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.straighten");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_STRAIGHTEN);
   /* A zero angle has no direction (it used to read "0.0° CW"). */
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Straighten 0.0° —"));
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   GdkTexture *p_tex = viewer_texture(fx.p_win);
   g_assert_cmpint(gdk_texture_get_width(p_tex), <, TOOL_W);
   g_assert_cmpint(gdk_texture_get_height(p_tex), <, TOOL_H);
   g_assert_nonnull(
      g_strstr_len(window_title(fx.p_win), -1, "straighten 1.0° CW"));
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Straighten 1.0"));
   g_assert_false(ggaze_window_tool_key(fx.p_win, GDK_KEY_A, GDK_SHIFT_MASK));
   tool_key_and_wait(fx.p_win, GDK_KEY_a); /* auto-crop off */
   p_tex = viewer_texture(fx.p_win);
   g_assert_cmpint(gdk_texture_get_width(p_tex), >, TOOL_W);
   g_assert_cmpint(gdk_texture_get_height(p_tex), >, TOOL_H);
   tool_key_and_wait(fx.p_win, GDK_KEY_minus); /* back to 0.5 CW */
   tool_key_and_wait(fx.p_win, GDK_KEY_minus);
   tool_key_and_wait(fx.p_win, GDK_KEY_h); /* 0.5 CCW */
   g_assert_nonnull(
      g_strstr_len(window_title(fx.p_win), -1, "straighten 0.5° CCW"));
   tool_key(fx.p_win, GDK_KEY_Return);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Straightened 0.5"));
   fire(fx.p_win, "win.back"); /* Esc outside the tool: closes the panel */
   ggtest_drain_main(100);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win)); /* kept */
   g_assert_null(find_panel(fx.p_win));
   fire(fx.p_win, "win.enhance");
   fire(fx.p_win, "win.edit-revert"); /* x: the one key that reverts */
   ggtest_drain_main(100);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   tool_fx_close(&fx);
}

/* Dragging along a horizon that slopes 10 degrees down to the right levels
 * it: the image turns 10 degrees counter-clockwise. */
static void
test_straighten_horizon_drag_levels(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.straighten");
   GgazeViewer *p_v = GGAZE_VIEWER(
      gtk_stack_get_child_by_name(ggaze_window_get_stack(fx.p_win), "large"));
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   gdouble     d_s      = g.d_scale;
   GdkTexture *p_before = ref_viewer_texture(fx.p_win);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN, g.d_x + 50 * d_s,
                          g.d_y + 100 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_UPDATE, g.d_x + 100 * d_s,
                          g.d_y + 110 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 150 * d_s,
                          g.d_y + 117.63 * d_s);
   wait_for_texture_change(fx.p_win, p_before);
   g_object_unref(p_before);
   g_assert_nonnull(
      g_strstr_len(window_title(fx.p_win), -1, "straighten 10.0° CCW"));
   /* A level line changes nothing (a zero angle is not a re-render). */
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN, g.d_x + 50 * d_s,
                          g.d_y + 100 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 150 * d_s,
                          g.d_y + 100 * d_s);
   ggtest_drain_main(200);
   g_assert_nonnull(
      g_strstr_len(window_title(fx.p_win), -1, "straighten 10.0° CCW"));
   tool_key(fx.p_win, GDK_KEY_Return);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   tool_fx_close(&fx);
}

/* Esc in the straighten tool restores the transform it started from -- here
 * a quarter turn, which stays (and stays dirty). */
static void
test_straighten_esc_restores_the_turn(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire_and_wait(fx.p_win, "win.rotate-cw");
   GdkTexture *p_turned = ref_viewer_texture(fx.p_win);
   fire(fx.p_win, "win.straighten");
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   GdkTexture *p_tilted = ref_viewer_texture(fx.p_win);
   tool_key(fx.p_win, GDK_KEY_Escape);
   wait_for_texture_change(fx.p_win, p_tilted);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   assert_texture_size(fx.p_win, TOOL_H, TOOL_W);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "90° CW"));
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_object_unref(p_turned);
   g_object_unref(p_tilted);
   tool_fx_close(&fx);
}

/* While one tool is active the other tool's key, its action, and the
 * quarter turns are refused (a turn under a laid-out rectangle would
 * silently move it); the tool stays and the preview is untouched. */
static void
test_tool_refuses_switch_and_turn_while_active(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.crop");
   fire(fx.p_win, "win.straighten");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Finish the"));
   tool_key(fx.p_win, GDK_KEY_r);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   fire(fx.p_win, "win.rotate-cw");
   tool_key(fx.p_win, GDK_KEY_bracketleft);
   ggtest_drain_main(200);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   tool_key(fx.p_win, GDK_KEY_Escape);
   fire(fx.p_win, "win.straighten");
   fire(fx.p_win, "win.crop");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_STRAIGHTEN);
   tool_key(fx.p_win, GDK_KEY_c);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_STRAIGHTEN);
   tool_key(fx.p_win, GDK_KEY_r); /* the same tool's key cancels it */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   tool_fx_close(&fx);
}

/* Enter in the crop tool is refused while the base preview is still
 * rendering (a rectangle laid out over a stale image must not commit), and
 * accepted once it has landed. */
static void
test_crop_enter_refused_while_rendering(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.rotate-cw"); /* async: in flight until the loop runs */
   fire(fx.p_win, "win.crop");
   tool_key(fx.p_win, GDK_KEY_Return);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Preview still"));
   wait_for_texture_change(fx.p_win, fx.p_orig);
   assert_texture_size(fx.p_win, TOOL_H, TOOL_W);
   crop_shrink_right(fx.p_win); /* now laid out on the turned base */
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   assert_texture_size(fx.p_win, TOOL_H - 3, TOOL_W);
   tool_fx_close(&fx);
}

/* The crop turns with the image: a committed crop, then `]`, keeps
 * covering the same pixels (its size swaps with the turn). */
static void
test_crop_turns_with_the_image(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.crop");
   for (guint u = 0; u < 10; u++) {
      crop_shrink_right(fx.p_win);
   }
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, TOOL_W - 30, TOOL_H);
   fire_and_wait(fx.p_win, "win.rotate-cw");
   assert_texture_size(fx.p_win, TOOL_H, TOOL_W - 30);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "90° CW, crop"));
   tool_fx_close(&fx);
}

/* A tool ends -- without committing -- when the image under it changes
 * hands: navigation (clean: no prompt) and leaving the large view. From the
 * grid, `c` first opens the highlighted image large. */
static void
test_tool_abandoned_on_navigation_and_view_change(void) {
   DirtyFixture             fx      = {0};
   static const char *const c_two[] = {"plain.jpg", "rot6.jpg", NULL};
   fixture_open_clean(&fx, "ggaze-tool-abandon-XXXXXX", c_two);
   fire(fx.p_win, "win.crop");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   fire(fx.p_win, "win.next"); /* not dirty: no prompt, straight through */
   ggtest_drain_main(300);
   assert_showing(fx.p_win, "rot6.jpg");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   fire(fx.p_win, "win.straighten");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_STRAIGHTEN);
   tool_key_and_wait(fx.p_win, GDK_KEY_l); /* a nudge, previewed live */
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   fire(fx.p_win, "win.toggle-view"); /* the grid: no canvas, no tool ... */
   ggtest_drain_main(300);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   /* ... and, like Esc, the nudge is undone: the angle the tool started
    * from (none) is back, so nothing is dirty and the title names no angle
    * -- the restore's commit did not pull the large view back either. */
   g_assert_cmpstr(
      gtk_stack_get_visible_child_name(ggaze_window_get_stack(fx.p_win)), ==,
      "grid");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   fire(fx.p_win, "win.crop"); /* from the grid: large first, then the tool */
   ggtest_drain_main(100);
   g_assert_cmpstr(
      gtk_stack_get_visible_child_name(ggaze_window_get_stack(fx.p_win)), ==,
      "large");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   fixture_teardown(&fx);
}

/* Re-opening the crop tool on an applied crop must not lose the crop: the
 * tool shows the base (a preview override), but the committed crop keeps
 * counting as work -- the window stays dirty, `s` inside the tool exports
 * the CROPPED file, and Esc afterwards brings the crop back. It used to
 * commit "no crop" to show the base, so `s` said "Nothing to save" and
 * navigation skipped the prompt and abandoned the tool: crop gone. */
static void
test_reopened_crop_tool_keeps_the_crop_as_work(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.crop");
   for (guint u = 0; u < 10; u++) {
      crop_shrink_right(fx.p_win);
   }
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, TOOL_W - 30, TOOL_H);
   GdkTexture *p_cropped = ref_viewer_texture(fx.p_win);
   fire(fx.p_win, "win.crop"); /* again: the base is shown ... */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   wait_for_texture_change(fx.p_win, p_cropped);
   assert_texture_size(fx.p_win, TOOL_W, TOOL_H);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win)); /* ... but */
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "crop"));
   fire(fx.p_win, "win.enhance-save"); /* `s` in the tool: the crop */
   char *c_out = g_build_filename(fx.c_dir, "tool-enhanced.png", NULL);
   wait_for_file(c_out);
   wait_for_status_prefix(fx.p_win, "Saved ");
   GError     *p_err = NULL;
   GdkTexture *p_tex = gdk_texture_new_from_filename(c_out, &p_err);
   g_assert_no_error(p_err);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, TOOL_W - 30);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, TOOL_H);
   g_object_unref(p_tex);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win)); /* saved */
   tool_key(fx.p_win, GDK_KEY_Escape); /* back to the (saved) crop */
   wait_for_texture_size(fx.p_win, TOOL_W - 30, TOOL_H);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_object_unref(p_cropped);
   g_free(c_out);
   tool_fx_close(&fx);
}

/* Navigating away with the crop tool open over an applied crop prompts
 * like any dirty preview: Cancel keeps the crop AND the tool, Discard moves
 * on with nothing applied. */
static void
test_navigation_inside_crop_tool_prompts(void) {
   ToolFx fx;
   tool_fx_open_with_sibling(&fx);
   fire(fx.p_win, "win.crop");
   for (guint u = 0; u < 10; u++) {
      crop_shrink_bottom(fx.p_win);
   }
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, TOOL_W, TOOL_H - 30);
   fire(fx.p_win, "win.crop");
   ggtest_drain_main(300);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   GtkWindow *p_own = GTK_WINDOW(fx.p_win);
   fire(fx.p_win, "win.prev");
   GGTEST_ASSERT_DIALOG_UP(p_own, "Cancel");
   g_assert_true(ggtest_click_dialog_button(p_own, "Cancel"));
   ggtest_drain_main(400);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "tool.png"));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   fire(fx.p_win, "win.prev");
   GGTEST_ASSERT_DIALOG_UP(p_own, "Discard");
   g_assert_true(ggtest_click_dialog_button(p_own, "Discard"));
   ggtest_drain_main(400);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "a.jpg"));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "crop"));
   tool_fx_close(&fx);
}

/* Saved means "the file on disk is this state": a tool cancelled back to
 * exactly the exported (mask, transform) is still saved, for the crop tool
 * (its override never touched the commit) and for the straighten tool (Esc
 * restores the saved angle); committing something else makes it dirty. */
static void
test_saved_state_survives_a_tool_cancel(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.crop");
   crop_square(fx.p_win);
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   fire(fx.p_win, "win.enhance-save");
   char *c_out = g_build_filename(fx.c_dir, "tool-enhanced.png", NULL);
   wait_for_file(c_out);
   wait_for_status_prefix(fx.p_win, "Saved ");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   fire(fx.p_win, "win.crop");
   wait_for_texture_size(fx.p_win, TOOL_W, TOOL_H);
   crop_shrink_right(fx.p_win);
   tool_key(fx.p_win, GDK_KEY_Escape);
   wait_for_texture_size(fx.p_win, TOOL_H, TOOL_H);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win)); /* still saved */
   /* Straighten on top, saved, then a nudge undone by Esc. */
   fire(fx.p_win, "win.straighten");
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   tool_key(fx.p_win, GDK_KEY_Return);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_remove(c_out);
   fire(fx.p_win, "win.enhance-save");
   wait_for_file(c_out);
   wait_for_status_prefix(fx.p_win, "Saved ");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   GdkTexture *p_saved = ref_viewer_texture(fx.p_win);
   fire(fx.p_win, "win.straighten");
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   GdkTexture *p_tilted = ref_viewer_texture(fx.p_win);
   tool_key(fx.p_win, GDK_KEY_Escape);
   wait_for_texture_change(fx.p_win, p_tilted);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win)); /* back: saved */
   /* A different commit is new work. */
   fire(fx.p_win, "win.crop");
   ggtest_drain_main(300);
   crop_shrink_right(fx.p_win);
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_object_unref(p_saved);
   g_object_unref(p_tilted);
   g_free(c_out);
   tool_fx_close(&fx);
}

/* A drag END (or UPDATE) that never had a BEGIN in the straighten tool --
 * the press happened before `r`, or the end is stale -- levels nothing: it
 * used to apply an angle computed from whatever the start coordinates last
 * held (a lone END at (300, 200) turned the image 35 degrees). */
static void
test_straighten_drag_end_without_begin_is_ignored(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.straighten");
   GgazeViewer *p_v = GGAZE_VIEWER(
      gtk_stack_get_child_by_name(ggaze_window_get_stack(fx.p_win), "large"));
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   gdouble d_s = g.d_scale;
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_UPDATE, g.d_x + 300 * d_s,
                          g.d_y + 200 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 300 * d_s,
                          g.d_y + 200 * d_s);
   ggtest_drain_main(300);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   /* A zero angle has no direction (it used to read "0.0° CW"). */
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Straighten 0.0° —"));
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   /* A proper line right after still works (the guard is per gesture). */
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN, g.d_x + 50 * d_s,
                          g.d_y + 100 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 150 * d_s,
                          g.d_y + 117.63 * d_s);
   wait_for_texture_change(fx.p_win, fx.p_orig);
   g_assert_nonnull(
      g_strstr_len(window_title(fx.p_win), -1, "straighten 10.0° CCW"));
   tool_key(fx.p_win, GDK_KEY_Escape);
   tool_fx_close(&fx);
}

/* Renders are coalesced: twenty nudges without a main-loop turn in between
 * launch ONE render for the first and ONE more (for the latest state) when
 * it lands -- not twenty full decodes -- and what ends up on screen is the
 * last state (10 degrees), never an intermediate one. */
static void
test_rapid_nudges_coalesce_into_two_renders(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.straighten");
   guint u_before = ggaze_window_enhance_render_count(fx.p_win);
   for (guint u = 0; u < 20; u++) {
      tool_key(fx.p_win, GDK_KEY_l); /* 0.5 degrees each, no loop turn */
   }
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win) - u_before, ==,
                    1);
   Transform t;
   transform_init(&t);
   t.d_degrees = 10.0;
   gdouble d_w, d_h;
   transform_base_size(&t, TOOL_W, TOOL_H, &d_w, &d_h);
   wait_for_texture_size(fx.p_win, (gint)d_w, (gint)d_h);
   ggtest_drain_main(300); /* nothing else may land after the last state */
   assert_texture_size(fx.p_win, (gint)d_w, (gint)d_h);
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win) - u_before, ==,
                    2);
   g_assert_nonnull(
      g_strstr_len(window_title(fx.p_win), -1, "straighten 10.0° CW"));
   /* Enter is refused only while something is still in flight: not now. */
   tool_key(fx.p_win, GDK_KEY_Return);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   tool_fx_close(&fx);
}

/* A committed crop follows a straighten: anchored on the centre it keeps
 * the same content (a centred square stays centred, cut to the new base).
 * Before, the crop was silently skipped by the chain while the title still
 * said "crop" and `s` exported un-cropped. */
static void
test_crop_follows_straighten(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.crop");
   crop_square(fx.p_win); /* 300x300 centred */
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   fire(fx.p_win, "win.straighten");
   tool_key(fx.p_win, GDK_KEY_l);
   tool_key(fx.p_win, GDK_KEY_l); /* 1 degree: the base is 392x291 */
   Transform t;
   transform_init(&t);
   t.d_degrees = 1.0;
   gdouble d_bw, d_bh;
   transform_base_size(&t, TOOL_W, TOOL_H, &d_bw, &d_bh);
   /* The 300-wide square, centred, cut to the 291-tall base. */
   wait_for_texture_size(fx.p_win, 300, (gint)d_bh);
   tool_key(fx.p_win, GDK_KEY_Return);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "crop"));
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "outside"));
   g_assert_null(g_strstr_len(status_text(fx.p_win), -1, "outside"));
   tool_fx_close(&fx);
}

/* A crop the straightened base no longer covers at all -- the rightmost
 * 8 px column at 10 degrees -- is KEPT: nothing is cropped meanwhile, the
 * status line and the title ("crop (outside view)") say so, and nudging
 * back applies it again unconditionally, whether the way back is the keys
 * or Esc. It used to be dropped with a status line, which lost the
 * rectangle for good one nudge too far. */
static void
test_crop_outside_the_view_comes_back(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.crop");
   for (guint u = 0; u < 131; u++) {
      crop_shrink_right(fx.p_win); /* right edge in to the minimum ... */
   }
   for (guint u = 0; u < 131; u++) {
      tool_key(fx.p_win, GDK_KEY_l); /* ... then slid to the right border */
   }
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, 8, TOOL_H);
   fire(fx.p_win, "win.straighten");
   for (guint u = 0; u < 20; u++) {
      tool_key(fx.p_win, GDK_KEY_l); /* 10 degrees: the base is 361x238 */
   }
   Transform t;
   transform_init(&t);
   t.d_degrees = 10.0;
   gdouble d_bw, d_bh;
   transform_base_size(&t, TOOL_W, TOOL_H, &d_bw, &d_bh);
   wait_for_texture_size(fx.p_win, (gint)d_bw, (gint)d_bh); /* no crop */
   g_assert_nonnull(g_strstr_len(status_text(fx.p_win), -1, "crop outside"));
   g_assert_nonnull(
      g_strstr_len(window_title(fx.p_win), -1, "crop (outside view)"));
   for (guint u = 0; u < 20; u++) {
      tool_key(fx.p_win, GDK_KEY_h); /* back to 0: the crop applies again */
   }
   wait_for_texture_size(fx.p_win, 8, TOOL_H);
   g_assert_null(g_strstr_len(status_text(fx.p_win), -1, "outside"));
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "outside"));
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "crop"));
   for (guint u = 0; u < 20; u++) {
      tool_key(fx.p_win, GDK_KEY_l); /* out again ... */
   }
   wait_for_texture_size(fx.p_win, (gint)d_bw, (gint)d_bh);
   tool_key(fx.p_win, GDK_KEY_Escape); /* ... and Esc brings it back too */
   wait_for_texture_size(fx.p_win, 8, TOOL_H);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "outside"));
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "crop"));
   tool_fx_close(&fx);
}

/* The result of emitting "key-pressed" on every capture-phase key
 * controller of p_win (what GDK does for a real key press before the
 * shortcut table sees it): TRUE iff one of them stopped the event. */
static gboolean
emit_capture_key(GgazeWindow *p_win, guint u_keyval) {
   GListModel *p_ctrls  = gtk_widget_observe_controllers(GTK_WIDGET(p_win));
   gboolean    b_stop   = FALSE;
   guint       u_looked = 0;
   for (guint i = 0; i < g_list_model_get_n_items(p_ctrls); i++) {
      GtkEventController *p_c = g_list_model_get_item(p_ctrls, i);
      if (GTK_IS_EVENT_CONTROLLER_KEY(p_c) &&
          gtk_event_controller_get_propagation_phase(p_c) ==
             GTK_PHASE_CAPTURE) {
         gboolean b_ret = FALSE;
         g_signal_emit_by_name(p_c, "key-pressed", u_keyval, 0u, 0, &b_ret);
         b_stop = b_stop || b_ret;
         u_looked++;
      }
      g_object_unref(p_c);
   }
   g_object_unref(p_ctrls);
   g_assert_cmpuint(u_looked, >, 0); /* the tools' controller exists */
   return (b_stop);
}

/* Activate the GLOBAL shortcut bound to u_keyval (no modifiers) exactly as
 * the shortcut controller would once a key press propagates that far. */
static void
activate_shortcut(GgazeWindow *p_win, guint u_keyval) {
   GListModel *p_ctrls = gtk_widget_observe_controllers(GTK_WIDGET(p_win));
   gboolean    b_found = FALSE;
   for (guint i = 0; i < g_list_model_get_n_items(p_ctrls) && !b_found; i++) {
      GtkEventController *p_c = g_list_model_get_item(p_ctrls, i);
      if (GTK_IS_SHORTCUT_CONTROLLER(p_c)) {
         GListModel *p_sc = G_LIST_MODEL(p_c);
         for (guint j = 0; j < g_list_model_get_n_items(p_sc) && !b_found;
              j++) {
            GtkShortcut        *p_s = g_list_model_get_item(p_sc, j);
            GtkShortcutTrigger *p_t = gtk_shortcut_get_trigger(p_s);
            if (GTK_IS_KEYVAL_TRIGGER(p_t) &&
                gtk_keyval_trigger_get_keyval(GTK_KEYVAL_TRIGGER(p_t)) ==
                   u_keyval &&
                gtk_keyval_trigger_get_modifiers(GTK_KEYVAL_TRIGGER(p_t)) ==
                   0) {
               b_found = gtk_shortcut_action_activate(
                  gtk_shortcut_get_action(p_s), 0, GTK_WIDGET(p_win), NULL);
            }
            g_object_unref(p_s);
         }
      }
      g_object_unref(p_c);
   }
   g_object_unref(p_ctrls);
   g_assert_true(b_found);
}

/* The window's capture-phase key controller (window.c _tool_key_cb) is what
 * keeps `h` from reaching win.prev while a tool is active: with the crop
 * tool up it STOPS the key (the rectangle moves, the file stays); with no
 * tool -- and, since 8i2, no panel -- it PROPAGATES and the same key's
 * shortcut navigates as usual. */
static void
test_tool_key_controller_claims_keys_only_while_active(void) {
   ToolFx fx;
   tool_fx_open_with_sibling(&fx);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "tool.png"));
   fire(fx.p_win, "win.crop");
   g_assert_true(emit_capture_key(fx.p_win, GDK_KEY_h));
   ggtest_drain_main(200);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "tool.png"));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   /* A key the tool does not own propagates even while it is active. */
   g_assert_false(emit_capture_key(fx.p_win, GDK_KEY_x));
   g_assert_true(emit_capture_key(fx.p_win, GDK_KEY_Escape));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   /* The panel the tool opened is still up, and h is its strength key
    * (8i2); closed, h is win.prev's again. */
   g_assert_true(emit_capture_key(fx.p_win, GDK_KEY_h));
   fire(fx.p_win, "win.enhance");
   g_assert_false(
      emit_capture_key(fx.p_win, GDK_KEY_h)); /* no tool, no panel */
   activate_shortcut(fx.p_win, GDK_KEY_h);    /* -> win.prev */
   ggtest_drain_main(300);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "a.jpg"));
   tool_fx_close(&fx);
}

/* --- 6i2: edit keys are modal to the panel --------------------------------
 *
 * The edit panel is the one home of every edit: the digits toggle presets
 * only while it is open, c / r / [ / ] open it first, and a key-hint bar
 * under the image lists the live keys of the mode on screen -- generated
 * from shortcuts.c's table. */

/* With the panel closed a digit is nobody's (the capture-phase router
 * passes it on, the global table has no binding for it): nothing is
 * toggled and nothing becomes dirty. With the panel open the same key
 * press toggles preset 1 -- and a popover's own digits are not taken. */
static void
test_digits_are_inert_with_the_panel_closed(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   g_assert_null(find_panel(fx.p_win));
   g_assert_false(emit_capture_key(fx.p_win, GDK_KEY_1));
   g_assert_false(ggaze_window_edit_key(fx.p_win, GDK_KEY_1, 0));
   ggtest_drain_main(200);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win), ==, 0);
   fire(fx.p_win, "win.enhance");
   g_assert_true(emit_capture_key(fx.p_win, GDK_KEY_1)); /* the panel's */
   wait_for_texture_change(fx.p_win, fx.p_orig);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "Auto-fix"));
   /* 8i2: h / j / k are the panel's now (strength, selection); Page
    * Down, which the panel leaves alone, still navigates. */
   g_assert_true(ggaze_window_edit_key(fx.p_win, GDK_KEY_h, 0));
   g_assert_true(ggaze_window_edit_key(fx.p_win, GDK_KEY_j, 0));
   g_assert_false(ggaze_window_edit_key(fx.p_win, GDK_KEY_Page_Down, 0));
   tool_fx_close(&fx);
}

/* c / r / [ / ] open the panel first when it is closed -- from the grid
 * too, where they first open the highlighted image large -- then act. */
static void
test_tool_keys_open_the_panel(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire_and_wait(fx.p_win, "win.rotate-cw");
   g_assert_nonnull(find_panel(fx.p_win));
   fire(fx.p_win, "win.back"); /* closes the panel, keeps the turn */
   g_assert_null(find_panel(fx.p_win));
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   fire(fx.p_win, "win.straighten");
   g_assert_nonnull(find_panel(fx.p_win));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_STRAIGHTEN);
   tool_key(fx.p_win, GDK_KEY_Escape); /* the tool only: the panel stays */
   g_assert_nonnull(find_panel(fx.p_win));
   fire(fx.p_win, "win.back");
   fire(fx.p_win, "win.back"); /* large -> grid */
   g_assert_cmpstr(
      gtk_stack_get_visible_child_name(ggaze_window_get_stack(fx.p_win)), ==,
      "grid");
   fire(fx.p_win, "win.crop");
   g_assert_cmpstr(
      gtk_stack_get_visible_child_name(ggaze_window_get_stack(fx.p_win)), ==,
      "large");
   g_assert_nonnull(find_panel(fx.p_win));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   tool_key(fx.p_win, GDK_KEY_Escape);
   tool_fx_close(&fx);
}

/* All four crop sides are symmetric: Ctrl+<key> moves the side the key
 * points at inward by one step (1% of 300 = 3 px), Shift+<key> moves it
 * outward again; h/j/k/l move the whole rectangle. */
static void
test_crop_shift_and_ctrl_move_every_side(void) {
   static const struct {
      guint   u_key;
      gdouble d_dx, d_dy, d_dw, d_dh; /* after Ctrl+key, from full */
   } SIDES[] = {
      {GDK_KEY_h, 3, 0, -3, 0}, /* left side in */
      {GDK_KEY_l, 0, 0, -3, 0}, /* right side in */
      {GDK_KEY_k, 0, 3, 0, -3}, /* top side down */
      {GDK_KEY_j, 0, 0, 0, -3}, /* bottom side up */
   };
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.crop");
   for (gsize u = 0; u < G_N_ELEMENTS(SIDES); u++) {
      g_assert_true(
         ggaze_window_tool_key(fx.p_win, SIDES[u].u_key, GDK_CONTROL_MASK));
      CropRect r = crop_rect_now(fx.p_win);
      g_assert_cmpfloat(r.d_x, ==, SIDES[u].d_dx);
      g_assert_cmpfloat(r.d_y, ==, SIDES[u].d_dy);
      g_assert_cmpfloat(r.d_w, ==, TOOL_W + SIDES[u].d_dw);
      g_assert_cmpfloat(r.d_h, ==, TOOL_H + SIDES[u].d_dh);
      guint u_upper = gdk_keyval_to_upper(SIDES[u].u_key); /* Shift+key */
      g_assert_true(ggaze_window_tool_key(fx.p_win, u_upper, GDK_SHIFT_MASK));
      r = crop_rect_now(fx.p_win);
      g_assert_cmpfloat(r.d_x, ==, 0.0);
      g_assert_cmpfloat(r.d_y, ==, 0.0);
      g_assert_cmpfloat(r.d_w, ==, TOOL_W);
      g_assert_cmpfloat(r.d_h, ==, TOOL_H);
   }
   crop_shrink_right(fx.p_win);
   tool_key(fx.p_win, GDK_KEY_h); /* no room left: slides along the edge */
   tool_key(fx.p_win, GDK_KEY_l); /* one step right */
   CropRect r = crop_rect_now(fx.p_win);
   g_assert_cmpfloat(r.d_x, ==, 3.0);
   g_assert_cmpfloat(r.d_w, ==, TOOL_W - 3);
   tool_key(fx.p_win, GDK_KEY_Escape);
   tool_fx_close(&fx);
}

/* The hint bar's text with its key/label joins (U+00A0) read as spaces. */
static char *
plain_hint(GgazeWindow *p_win) {
   char *c_raw = ggaze_window_get_hint_text(p_win);
   if (c_raw == NULL) {
      return (NULL);
   }
   char **c_parts = g_strsplit(c_raw, "\u00a0", -1);
   char  *c_out   = g_strjoinv(" ", c_parts);
   g_strfreev(c_parts);
   g_free(c_raw);
   return (c_out);
}

/* The key-hint bar follows the key mode: hidden while browsing, the
 * panel's keys while it is open, a tool's while that is up, back to the
 * panel's when the tool ends, hidden in the grid and when the panel
 * closes. Its text is the table's (shortcuts_hint_for_mode). */
static void
test_hint_bar_follows_the_mode(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   g_assert_null(plain_hint(fx.p_win));
   g_assert_cmpint(ggaze_window_get_key_mode(fx.p_win), ==,
                   GGAZE_KEY_MODE_NONE);
   fire(fx.p_win, "win.enhance");
   char *c_hint = plain_hint(fx.p_win);
   g_assert_true(g_str_has_prefix(c_hint, "Edit: 1–8 presets"));
   g_assert_nonnull(g_strstr_len(c_hint, -1, "x revert"));
   g_assert_nonnull(g_strstr_len(c_hint, -1, "a/Esc close"));
   g_free(c_hint);
   fire(fx.p_win, "win.crop");
   g_assert_cmpint(ggaze_window_get_key_mode(fx.p_win), ==,
                   GGAZE_KEY_MODE_CROP);
   c_hint = plain_hint(fx.p_win);
   g_assert_true(g_str_has_prefix(c_hint, "Crop: h/j/k/l move"));
   g_assert_nonnull(g_strstr_len(c_hint, -1, "Ctrl+h/j/k/l shrink"));
   g_free(c_hint);
   tool_key(fx.p_win, GDK_KEY_Escape);
   c_hint = plain_hint(fx.p_win);
   g_assert_true(g_str_has_prefix(c_hint, "Edit: "));
   g_free(c_hint);
   fire(fx.p_win, "win.straighten");
   c_hint = plain_hint(fx.p_win);
   g_assert_true(g_str_has_prefix(c_hint, "Straighten: h/l/-/+ nudge"));
   g_assert_nonnull(g_strstr_len(c_hint, -1, "a auto-crop"));
   g_free(c_hint);
   tool_key(fx.p_win, GDK_KEY_Return);
   fire(fx.p_win, "win.toggle-view"); /* grid: no bar */
   g_assert_null(plain_hint(fx.p_win));
   fire(fx.p_win, "win.toggle-view"); /* large again: the panel's */
   c_hint = plain_hint(fx.p_win);
   g_assert_true(g_str_has_prefix(c_hint, "Edit: "));
   g_free(c_hint);
   fire(fx.p_win, "win.back"); /* closes the panel */
   g_assert_null(plain_hint(fx.p_win));
   tool_fx_close(&fx);
}

/* Caps Lock is not Shift (review of 6i2): with it on, a plain `l` arrives
 * as `L` + Lock and must MOVE the crop rectangle (it grew it), Shift+h
 * under Caps Lock still grows, and `A` + Lock is the tool's aspect key --
 * it fell through to the global `a`, which closed the panel and left the
 * tool running. All through the router, the path a real key press takes. */
static void
test_caps_lock_keys_in_the_crop_tool(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.crop");
   crop_shrink_right(fx.p_win); /* room to move: x 0, w 397 */
   g_assert_true(ggaze_window_edit_key(fx.p_win, GDK_KEY_L, GDK_LOCK_MASK));
   CropRect r = crop_rect_now(fx.p_win);
   g_assert_cmpfloat(r.d_x, ==, 3.0);        /* moved right ... */
   g_assert_cmpfloat(r.d_w, ==, TOOL_W - 3); /* ... not grown */
   g_assert_true(ggaze_window_edit_key(fx.p_win, GDK_KEY_h,
                                       GDK_SHIFT_MASK | GDK_LOCK_MASK));
   r = crop_rect_now(fx.p_win);
   g_assert_cmpfloat(r.d_x, ==, 0.0); /* Shift+h: the left side out */
   g_assert_cmpfloat(r.d_w, ==, TOOL_W);
   g_assert_true(ggaze_window_edit_key(fx.p_win, GDK_KEY_A, GDK_LOCK_MASK));
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Crop aspect: 1:1"));
   g_assert_nonnull(find_panel(fx.p_win)); /* not the global `a` */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_true(
      ggaze_window_edit_key(fx.p_win, GDK_KEY_Escape, GDK_LOCK_MASK));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   tool_fx_close(&fx);
}

/* `a` then `t` hides the open panel with the grid (not closed: `t` brings
 * it back). While hidden it is no key mode: a digit is nobody's (it used
 * to toggle a preset on the image that is not on screen), x only says
 * where it works (it used to revert that image's edits), and Esc skips the
 * invisible panel and goes on to the grid's chain (it used to "close" it,
 * an Esc that seemed to do nothing). Back in the large view all of it is
 * live again. */
static void
test_hidden_panel_in_the_grid_is_inert(void) {
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-hidden-XXXXXX");
   fire(fx.p_win, "win.enhance");
   GtkWidget *p_panel = find_panel(fx.p_win);
   g_assert_nonnull(p_panel);
   fire(fx.p_win, "win.toggle-view"); /* grid: the panel hides */
   ggtest_drain_main(200);
   /* The window is never presented: the slot's own flag is the one. */
   GtkWidget *p_slot = gtk_widget_get_parent(p_panel);
   g_assert_false(gtk_widget_get_visible(p_slot));
   g_assert_cmpint(ggaze_window_get_key_mode(fx.p_win), ==,
                   GGAZE_KEY_MODE_NONE);
   g_assert_false(ggaze_window_edit_key(fx.p_win, GDK_KEY_2, 0));
   fire(fx.p_win, "win.enhance-2"); /* the action itself, e.g. a script */
   g_assert_true(
      g_str_has_prefix(status_text(fx.p_win), "Edits apply in the large"));
   fire(fx.p_win, "win.edit-revert");
   g_assert_true(g_str_has_prefix(status_text(fx.p_win),
                                  "x reverts edits in the large view"));
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win)); /* kept */
   fire(fx.p_win, "win.back"); /* Esc: the grid's chain, not the panel */
   g_assert_true(find_panel(fx.p_win) == p_panel);
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Press Esc again"));
   fire(fx.p_win, "win.enhance"); /* `a`: shows the panel, never closes it */
   ggtest_drain_main(200);
   g_assert_cmpstr(
      gtk_stack_get_visible_child_name(ggaze_window_get_stack(fx.p_win)), ==,
      "large");
   g_assert_true(find_panel(fx.p_win) == p_panel);
   g_assert_true(gtk_widget_get_visible(p_slot));
   g_assert_cmpint(ggaze_window_get_key_mode(fx.p_win), ==,
                   GGAZE_KEY_MODE_PANEL);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_mod);
   fire(fx.p_win, "win.back"); /* now Esc closes it, keeping the edit */
   g_assert_null(find_panel(fx.p_win));
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   fixture_teardown(&fx);
}

/* No tool outlives the panel: closing it under a tool -- its close button,
 * or win.enhance from the menu (a real `a` is the tool's own key) --
 * cancels the tool first, restoring what it started from, then closes. It
 * used to leave the crop tool running, its h/j/k/l live, with no panel,
 * Save or Revert on screen. */
static void
test_closing_the_panel_cancels_the_tool(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.crop");
   crop_shrink_right(fx.p_win);
   ggtest_click_button(find_action_button(find_panel(fx.p_win), "win.enhance"));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_null(find_panel(fx.p_win));
   g_assert_cmpint(ggaze_window_get_key_mode(fx.p_win), ==,
                   GGAZE_KEY_MODE_NONE);
   g_assert_false(ggaze_window_edit_key(fx.p_win, GDK_KEY_h, 0)); /* global */
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));       /* no crop */
   fire(fx.p_win, "win.straighten");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_STRAIGHTEN);
   fire(fx.p_win, "win.enhance"); /* the menu's "Edit panel" */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_null(find_panel(fx.p_win));
   g_assert_null(ggaze_window_get_hint_text(fx.p_win));
   tool_fx_close(&fx);
}

/* --- 7i2: undo / redo of edit steps ----------------------------------------
 *
 * With the edit panel open, u / Ctrl+z undo the last edit step and U /
 * Ctrl+Shift+Z redo it -- a preset toggled (digit or card), a quarter
 * turn, a crop or straighten applied, x -- through the same render path a
 * toggle takes. The keys go through the window's edit-key router
 * (ggaze_window_edit_key), the path a real key press takes; outside the
 * panel u is the file undo as before. */

/* Press a key through the edit-key router and assert it was claimed. */
static void
edit_key(GgazeWindow *p_win, guint u_keyval, GdkModifierType e_mods) {
   g_assert_true(ggaze_window_edit_key(p_win, u_keyval, e_mods));
}

/* Press a key through the router and wait for the texture it changes. */
static void
edit_key_and_wait(GgazeWindow *p_win, guint u_keyval, GdkModifierType e_mods) {
   GdkTexture *p_before = ref_viewer_texture(p_win);
   edit_key(p_win, u_keyval, e_mods);
   wait_for_texture_change(p_win, p_before);
   g_object_unref(p_before);
}

/* The open panel's button on c_action (asserted present). */
static GtkWidget *
panel_button(GgazeWindow *p_win, const char *c_action) {
   GtkWidget *p_panel = find_panel(p_win);
   g_assert_nonnull(p_panel);
   GtkWidget *p_btn = find_action_button(p_panel, c_action);
   g_assert_nonnull(p_btn);
   return (p_btn);
}

/* The Undo / Redo buttons are sensitive exactly when there is something to
 * undo / redo. */
static void
assert_history_buttons(GgazeWindow *p_win, gboolean b_undo, gboolean b_redo) {
   g_assert_cmpint(
      gtk_widget_get_sensitive(panel_button(p_win, "win.edit-undo")), ==,
      b_undo);
   g_assert_cmpint(
      gtk_widget_get_sensitive(panel_button(p_win, "win.edit-redo")), ==,
      b_redo);
}

static void
assert_status_prefix(GgazeWindow *p_win, const char *c_prefix) {
   if (!g_str_has_prefix(status_text(p_win), c_prefix)) {
      g_error("status \"%s\", wanted \"%s...\"", status_text(p_win), c_prefix);
   }
}

/* A preset toggled by its digit: u takes it back (the original, clean),
 * U does it again (dirty) -- the status line names the step, the Undo /
 * Redo buttons follow, and Caps Lock changes nothing (U + Lock is undo,
 * Shift+u under Lock redo). The hint bar lists both keys. */
static void
test_undo_redo_a_preset(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   assert_history_buttons(fx.p_win, FALSE, FALSE);
   char *c_hint = plain_hint(fx.p_win);
   g_assert_nonnull(g_strstr_len(c_hint, -1, "u/Shift+u undo/redo"));
   g_free(c_hint);
   edit_key_and_wait(fx.p_win, GDK_KEY_1, 0);
   GdkTexture *p_mod = ref_viewer_texture(fx.p_win);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   assert_history_buttons(fx.p_win, TRUE, FALSE);

   edit_key_and_wait(fx.p_win, GDK_KEY_u, 0);
   assert_status_prefix(fx.p_win, "Undid: Auto-fix on");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "Auto-fix"));
   assert_history_buttons(fx.p_win, FALSE, TRUE);
   edit_key(fx.p_win, GDK_KEY_u, 0); /* nothing left */
   assert_status_prefix(fx.p_win, "No edit to undo");

   edit_key_and_wait(fx.p_win, GDK_KEY_U, GDK_SHIFT_MASK);
   assert_status_prefix(fx.p_win, "Redid: Auto-fix on");
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "Auto-fix"));
   assert_history_buttons(fx.p_win, TRUE, FALSE);
   edit_key(fx.p_win, GDK_KEY_U, GDK_SHIFT_MASK);
   assert_status_prefix(fx.p_win, "Nothing to redo");

   /* Caps Lock: `u` arrives as U + Lock and is undo; Shift+u is redo. */
   edit_key_and_wait(fx.p_win, GDK_KEY_U, GDK_LOCK_MASK);
   assert_status_prefix(fx.p_win, "Undid: ");
   edit_key_and_wait(fx.p_win, GDK_KEY_U, GDK_SHIFT_MASK | GDK_LOCK_MASK);
   assert_status_prefix(fx.p_win, "Redid: ");
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_object_unref(p_mod);
   tool_fx_close(&fx);
}

/* Ctrl+z / Ctrl+Shift+Z and the panel's buttons are the same undo / redo,
 * and a card click is a step like its digit. A new step after an undo
 * drops the redo. */
static void
test_undo_redo_chords_buttons_and_cards(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   GtkWidget  *p_card   = find_card(find_panel(fx.p_win), 1);
   GdkTexture *p_before = ref_viewer_texture(fx.p_win);
   ggtest_click_button(p_card);
   wait_for_texture_change(fx.p_win, p_before);
   g_clear_object(&p_before);
   g_assert_true(gtk_widget_has_css_class(p_card, "ggaze-enhance-on"));

   edit_key_and_wait(fx.p_win, GDK_KEY_z, GDK_CONTROL_MASK);
   assert_status_prefix(fx.p_win, "Undid: ");
   g_assert_false(gtk_widget_has_css_class(p_card, "ggaze-enhance-on"));
   edit_key_and_wait(fx.p_win, GDK_KEY_Z, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
   assert_status_prefix(fx.p_win, "Redid: ");
   g_assert_true(gtk_widget_has_css_class(p_card, "ggaze-enhance-on"));

   p_before = ref_viewer_texture(fx.p_win);
   ggtest_click_button(panel_button(fx.p_win, "win.edit-undo"));
   wait_for_texture_change(fx.p_win, p_before);
   g_clear_object(&p_before);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   assert_history_buttons(fx.p_win, FALSE, TRUE);
   p_before = ref_viewer_texture(fx.p_win);
   ggtest_click_button(panel_button(fx.p_win, "win.edit-redo"));
   wait_for_texture_change(fx.p_win, p_before);
   g_clear_object(&p_before);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));

   edit_key_and_wait(fx.p_win, GDK_KEY_u, 0);
   assert_history_buttons(fx.p_win, FALSE, TRUE);
   edit_key_and_wait(fx.p_win, GDK_KEY_1, 0);     /* a new step ... */
   assert_history_buttons(fx.p_win, TRUE, FALSE); /* ... drops the redo */
   tool_fx_close(&fx);
}

/* Each quarter turn is a step: u turns back one, U turns again. */
static void
test_undo_redo_a_quarter_turn(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire_and_wait(fx.p_win, "win.rotate-cw"); /* opens the panel */
   fire_and_wait(fx.p_win, "win.rotate-cw");
   assert_texture_size(fx.p_win, TOOL_W, TOOL_H); /* 180 degrees */
   edit_key(fx.p_win, GDK_KEY_u, 0);
   assert_status_prefix(fx.p_win, "Undid: rotate right");
   wait_for_texture_size(fx.p_win, TOOL_H, TOOL_W); /* 90 */
   edit_key(fx.p_win, GDK_KEY_u, 0);
   wait_for_texture_size(fx.p_win, TOOL_W, TOOL_H); /* 0 */
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key(fx.p_win, GDK_KEY_U, GDK_SHIFT_MASK);
   assert_status_prefix(fx.p_win, "Redid: rotate right");
   wait_for_texture_size(fx.p_win, TOOL_H, TOOL_W);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   fire_and_wait(fx.p_win, "win.rotate-ccw");
   edit_key(fx.p_win, GDK_KEY_u, 0);
   assert_status_prefix(fx.p_win, "Undid: rotate left");
   wait_for_texture_size(fx.p_win, TOOL_H, TOOL_W);
   tool_fx_close(&fx);
}

/* A crop applied with Enter is one step, whatever the nudges before it:
 * u shows the whole image again, U the crop. */
static void
test_undo_redo_a_crop(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.crop");
   crop_shrink_right(fx.p_win);
   crop_shrink_right(fx.p_win);
   tool_key(fx.p_win, GDK_KEY_Return);
   wait_for_texture_size(fx.p_win, TOOL_W - 6, TOOL_H);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key(fx.p_win, GDK_KEY_u, 0);
   assert_status_prefix(fx.p_win, "Undid: crop");
   wait_for_texture_size(fx.p_win, TOOL_W, TOOL_H);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key(fx.p_win, GDK_KEY_U, GDK_SHIFT_MASK);
   assert_status_prefix(fx.p_win, "Redid: crop");
   wait_for_texture_size(fx.p_win, TOOL_W - 6, TOOL_H);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   tool_fx_close(&fx);
}

/* A straighten applied with Enter is one step (its nudges are not): u
 * levels the image back to 0 degrees, U to the applied angle. */
static void
test_undo_redo_a_straighten(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.straighten");
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   tool_key(fx.p_win, GDK_KEY_Return);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "1.0°"));
   edit_key(fx.p_win, GDK_KEY_u, 0);
   assert_status_prefix(fx.p_win, "Undid: straighten 1.0° CW");
   wait_for_texture_size(fx.p_win, TOOL_W, TOOL_H);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "1.0°"));
   edit_key_and_wait(fx.p_win, GDK_KEY_U, GDK_SHIFT_MASK);
   assert_status_prefix(fx.p_win, "Redid: straighten 1.0° CW");
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   /* The title names the state once its render has landed. */
   g_assert_cmpint(gdk_texture_get_width(viewer_texture(fx.p_win)), <, TOOL_W);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "1.0°"));
   tool_fx_close(&fx);
}

/* Under a tool u is refused, not a cancel: the tool stays up with its
 * work, and the status line says how to end it. A preset toggled under
 * the straighten tool is a step of its own that records the angle the
 * tool STARTED from -- undoing it later never brings back a nudge that
 * was not applied. */
static void
test_undo_under_a_tool(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.straighten");
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   edit_key(fx.p_win, GDK_KEY_u, 0);
   assert_status_prefix(fx.p_win, "Finish the current tool first");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_STRAIGHTEN);
   edit_key_and_wait(fx.p_win, GDK_KEY_1, 0); /* Auto-fix under the tool */
   tool_key(fx.p_win, GDK_KEY_Return);        /* straighten 0.5° */
   edit_key(fx.p_win, GDK_KEY_u, 0);          /* the straighten */
   assert_status_prefix(fx.p_win, "Undid: straighten 0.5° CW");
   edit_key(fx.p_win, GDK_KEY_u, 0); /* Auto-fix, at the STARTING angle */
   assert_status_prefix(fx.p_win, "Undid: Auto-fix on");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   wait_for_texture_size(fx.p_win, TOOL_W, TOOL_H);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "0.5°"));
   tool_fx_close(&fx);
}

/* x is a step too: u after x puts every edit back at once (the preset AND
 * the turn), U reverts again. */
static void
test_undo_brings_back_a_revert(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   edit_key_and_wait(fx.p_win, GDK_KEY_1, 0);
   fire_and_wait(fx.p_win, "win.rotate-cw");
   fire(fx.p_win, "win.edit-revert");
   wait_for_texture_size(fx.p_win, TOOL_W, TOOL_H);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key(fx.p_win, GDK_KEY_u, 0);
   assert_status_prefix(fx.p_win, "Undid: revert all");
   wait_for_texture_size(fx.p_win, TOOL_H, TOOL_W);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "Auto-fix"));
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "90° CW"));
   edit_key(fx.p_win, GDK_KEY_U, GDK_SHIFT_MASK);
   assert_status_prefix(fx.p_win, "Redid: revert all");
   wait_for_texture_size(fx.p_win, TOOL_W, TOOL_H);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   tool_fx_close(&fx);
}

/* Undo keeps the saved/dirty rule: coming back to the state `s` wrote is
 * saved again (no prompt on navigation), leaving it is dirty. */
static void
test_undo_to_the_saved_state_is_clean(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   edit_key_and_wait(fx.p_win, GDK_KEY_1, 0);
   fire(fx.p_win, "win.enhance-save");
   char *c_out = g_build_filename(fx.c_dir, "tool-enhanced.png", NULL);
   wait_for_file(c_out);
   wait_for_status_prefix(fx.p_win, "Saved ");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key_and_wait(fx.p_win, GDK_KEY_2, 0);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key_and_wait(fx.p_win, GDK_KEY_u, 0);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win)); /* saved again */
   g_assert_nonnull(find_label_prefix(find_panel(fx.p_win), "Saved as "));
   edit_key_and_wait(fx.p_win, GDK_KEY_u, 0); /* the original: clean */
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key_and_wait(fx.p_win, GDK_KEY_U, GDK_SHIFT_MASK); /* saved state */
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key_and_wait(fx.p_win, GDK_KEY_U, GDK_SHIFT_MASK); /* +preset 2 */
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   g_free(c_out);
   tool_fx_close(&fx);
}

/* The history is the image's: moving to another file clears it (nothing
 * to undo or redo there, the buttons insensitive), and so does the gate's
 * Discard. */
static void
test_navigation_clears_the_history(void) {
   ToolFx fx;
   tool_fx_open_with_sibling(&fx);
   fire(fx.p_win, "win.enhance");
   edit_key_and_wait(fx.p_win, GDK_KEY_1, 0);
   edit_key_and_wait(fx.p_win, GDK_KEY_1, 0);
   edit_key_and_wait(fx.p_win, GDK_KEY_2, 0);
   edit_key_and_wait(fx.p_win, GDK_KEY_u, 0); /* clean, undo + redo left */
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   assert_history_buttons(fx.p_win, TRUE, TRUE);
   fire(fx.p_win, "win.prev"); /* clean: no prompt */
   wait_for_load(fx.p_win, PLAIN_JPG_W, PLAIN_JPG_H);
   assert_showing(fx.p_win, "a.jpg");
   assert_history_buttons(fx.p_win, FALSE, FALSE);
   edit_key(fx.p_win, GDK_KEY_u, 0);
   assert_status_prefix(fx.p_win, "No edit to undo");
   edit_key(fx.p_win, GDK_KEY_U, GDK_SHIFT_MASK);
   assert_status_prefix(fx.p_win, "Nothing to redo");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   tool_fx_close(&fx);
}

/* The gate's Discard throws the edit away with its history: after it
 * (answered on a navigation) nothing is undone back onto the new file. */
static void
test_discard_clears_the_history(void) {
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-undodiscard-XXXXXX");
   fire(fx.p_win, "win.enhance");
   fire(fx.p_win, "win.next");
   answer_prompt(&fx, "Discard");
   assert_showing(fx.p_win, "rot6.jpg");
   assert_history_buttons(fx.p_win, FALSE, FALSE);
   edit_key(fx.p_win, GDK_KEY_u, 0);
   assert_status_prefix(fx.p_win, "No edit to undo");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   fixture_teardown(&fx);
}

/* The slideshow's silent discard throws the history away too. A one-file
 * folder: the tick's navigator_next is a no-op, so nothing but the
 * discard could have cleared it (the gate's Discard above is followed by
 * a navigation that clears it anyway). */
static void
test_slideshow_discard_clears_the_history(void) {
   Settings *p_s = settings_new();
   settings_set_slideshow_delay(p_s, 0.2);
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   edit_key_and_wait(fx.p_win, GDK_KEY_1, 0);
   assert_history_buttons(fx.p_win, TRUE, FALSE);
   fire(fx.p_win, "win.slideshow");
   ggtest_drain_main(600);          /* at least one tick */
   fire(fx.p_win, "win.slideshow"); /* stop it */
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   assert_history_buttons(fx.p_win, FALSE, FALSE);
   edit_key(fx.p_win, GDK_KEY_u, 0);
   assert_status_prefix(fx.p_win, "No edit to undo");
   g_settings_reset(settings_get_gsettings(p_s), "slideshow-delay");
   settings_delete(p_s);
   tool_fx_close(&fx);
}

/* Outside the panel u is the file undo, exactly as before 7i2: the router
 * leaves it to the global table (win.undo) with the panel closed, while
 * with it open the same key is the edit undo and never restores a trashed
 * file. */
static void
test_u_outside_the_panel_undoes_a_trash(void) {
   DirtyFixture             fx        = {0};
   static const char *const c_three[] = {"plain.jpg", "rgba.png", "rot6.jpg",
                                         NULL};
   fixture_open_clean(&fx, "ggaze-enhance-undotrash-XXXXXX", c_three);
   g_assert_false(ggaze_window_edit_key(fx.p_win, GDK_KEY_u, 0));
   g_assert_false(ggaze_window_edit_key(fx.p_win, GDK_KEY_z, GDK_CONTROL_MASK));
   fire(fx.p_win, "win.trash"); /* clean: no prompt */
   ggtest_drain_main(300);
   g_assert_false(g_file_test(fx.c_path, G_FILE_TEST_EXISTS));
   fire(fx.p_win, "win.enhance");
   edit_key(fx.p_win, GDK_KEY_u, 0); /* the panel's: an edit undo */
   assert_status_prefix(fx.p_win, "No edit to undo");
   ggtest_drain_main(200);
   g_assert_false(g_file_test(fx.c_path, G_FILE_TEST_EXISTS));
   fire(fx.p_win, "win.enhance"); /* close the panel */
   g_assert_false(ggaze_window_edit_key(fx.p_win, GDK_KEY_u, 0));
   fire(fx.p_win, "win.undo"); /* what the global u then fires */
   ggtest_drain_main(300);
   g_assert_true(g_file_test(fx.c_path, G_FILE_TEST_EXISTS));
   /* The menu's "Undo edit" with the panel closed only says where it
    * works. */
   fire(fx.p_win, "win.edit-undo");
   assert_status_prefix(fx.p_win, "u undoes edits in the edit panel");
   fixture_teardown(&fx);
}

/* --- touch swipe (zb2) ---------------------------------------------------- */

/* The window's GgazeViewer (its stack's "large" child), borrowed. */
static GgazeViewer *
large_viewer(GgazeWindow *p_win) {
   GtkStack *p_stack = ggaze_window_get_stack(p_win);
   return (GGAZE_VIEWER(gtk_stack_get_child_by_name(p_stack, "large")));
}

/* A leftward swipe (viewer.h ggaze_viewer_swipe, what the GtkGestureSwipe
 * handler calls) is a navigation like `l`: away from a dirty preview it
 * raises the Save/Discard/Cancel prompt instead of moving. Cancel keeps
 * the preview and the file; a second swipe answered Discard moves on. */
static void
test_swipe_away_from_dirty_preview_prompts(void) {
   DirtyFixture fx = {0};
   fixture_open(&fx, "ggaze-enhance-swipe-XXXXXX");
   GgazeViewer *p_v = large_viewer(fx.p_win);
   g_assert_cmpint(ggaze_viewer_swipe(p_v, -200.0, 0.0, -900.0, 0.0), ==, 1);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   assert_showing(fx.p_win, "plain.jpg");
   answer_prompt(&fx, "Cancel");
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   assert_showing(fx.p_win, "plain.jpg");
   assert_ref_settled(&fx);
   g_assert_cmpint(ggaze_viewer_swipe(p_v, -200.0, 0.0, -900.0, 0.0), ==, 1);
   answer_prompt(&fx, "Discard");
   assert_showing(fx.p_win, "rot6.jpg");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   fixture_teardown(&fx);
}

/* While the crop tool is up a swipe navigates nowhere -- the tool owns the
 * drag, and a navigation would abandon it -- and the tool stays; once it is
 * cancelled the same swipe goes to the previous file. */
static void
test_swipe_refused_while_tool_active(void) {
   ToolFx fx;
   tool_fx_open_with_sibling(&fx);
   GgazeViewer *p_v = large_viewer(fx.p_win);
   fire(fx.p_win, "win.crop");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_cmpint(ggaze_viewer_swipe(p_v, 200.0, 0.0, 900.0, 0.0), ==, 0);
   ggtest_drain_main(200);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "tool.png"));
   tool_key(fx.p_win, GDK_KEY_Escape);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_cmpint(ggaze_viewer_swipe(p_v, 200.0, 0.0, 900.0, 0.0), ==, -1);
   ggtest_drain_main(300);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "a.jpg"));
   tool_fx_close(&fx);
}

/* The viewer's drag gesture (borrowed: the widget owns it), for driving
 * the real drag handlers the tool's overlay callback sits behind. */
static GtkEventController *
viewer_drag_gesture(GgazeViewer *p_v) {
   GListModel *p_ctrls = gtk_widget_observe_controllers(GTK_WIDGET(p_v));
   GtkEventController *p_found = NULL;
   for (guint i = 0; i < g_list_model_get_n_items(p_ctrls); i++) {
      GtkEventController *p_c = g_list_model_get_item(p_ctrls, i);
      if (GTK_IS_GESTURE_DRAG(p_c) && p_found == NULL) {
         p_found = p_c;
      }
      g_object_unref(p_c);
   }
   g_object_unref(p_ctrls);
   g_assert_nonnull(p_found);
   return (p_found);
}

/* A second finger landing while the first draws a horizon in the straighten
 * tool (`r`) makes a pinch, and the line is dropped: the viewer sends a
 * CANCEL, not an END (viewer.h), so the first finger's (1, 1) px jitter --
 * a 45 degree line -- levels nothing: no render, no angle in the title,
 * and the tool stays up. The rest of that drag reaches the tool neither.
 * What proves the line was DROPPED (not merely never ended): a stray END
 * with no BEGIN, straight into the tool, still levels nothing -- with the
 * line kept it would level by the jitter (zb2 second review). */
static void
test_pinch_in_straighten_levels_nothing(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.straighten");
   GgazeViewer    *p_v = large_viewer(fx.p_win);
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   GtkEventController *p_drag   = viewer_drag_gesture(p_v);
   guint               u_before = ggaze_window_enhance_render_count(fx.p_win);
   gdouble             d_x0     = g.d_x + 50 * g.d_scale;
   gdouble             d_y0     = g.d_y + 100 * g.d_scale;
   g_signal_emit_by_name(p_drag, "drag-begin", d_x0, d_y0);
   g_signal_emit_by_name(p_drag, "drag-update", 1.0, 1.0);
   ggaze_viewer_pinch_begin(p_v, d_x0 + 60.0, d_y0, FALSE);
   ggaze_viewer_pinch_update(p_v, 1.3, d_x0 + 60.0, d_y0);
   g_signal_emit_by_name(p_drag, "drag-update", 100.0, 40.0);
   g_signal_emit_by_name(p_drag, "drag-end", 100.0, 40.0);
   ggaze_viewer_pinch_end(p_v);
   ggtest_drain_main(300);
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win), ==, u_before);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_STRAIGHTEN);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, d_x0 + 1.0,
                          d_y0 + 1.0);
   ggtest_drain_main(300);
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win), ==, u_before);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   tool_key(fx.p_win, GDK_KEY_Escape);
   tool_fx_close(&fx);
}

/* A two-finger TAP in the crop tool whose first finger landed on the
 * rectangle's corner and jittered before the second landed: the pinch
 * CANCELs that drag (keeping the jittered rectangle, as a real pinch
 * would), and when it ends as a tap the viewer sends a DRAG_REVERT, so
 * the rectangle is what it was before the first finger went down -- a
 * tap edits nothing (zb2 second review). */
static void
test_tap_in_crop_leaves_the_rect(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.crop");
   GgazeViewer    *p_v = large_viewer(fx.p_win);
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   CropRect t_before, t_after;
   gint     i_bw, i_bh;
   g_assert_true(
      ggaze_window_tool_crop_rect(fx.p_win, &t_before, &i_bw, &i_bh));
   GtkEventController *p_drag = viewer_drag_gesture(p_v);
   gdouble             d_cx   = g.d_x + TOOL_W * g.d_scale;
   gdouble             d_cy   = g.d_y + TOOL_H * g.d_scale;
   g_signal_emit_by_name(p_drag, "drag-begin", d_cx, d_cy);
   g_signal_emit_by_name(p_drag, "drag-update", -6.0, -4.0); /* jitter */
   g_assert_true(ggaze_window_tool_crop_rect(fx.p_win, &t_after, &i_bw, &i_bh));
   g_assert_cmpfloat(t_after.d_w, <, t_before.d_w); /* the jitter landed */
   ggaze_viewer_pinch_begin(p_v, d_cx - 40.0, d_cy - 20.0, FALSE);
   ggaze_viewer_pinch_update(p_v, 1.02, d_cx - 39.0, d_cy - 20.0);
   g_assert_true(ggaze_viewer_pinch_end(p_v)); /* a tap */
   g_signal_emit_by_name(p_drag, "drag-end", -6.0, -4.0);
   g_assert_true(ggaze_window_tool_crop_rect(fx.p_win, &t_after, &i_bw, &i_bh));
   g_assert_cmpfloat(t_after.d_x, ==, t_before.d_x);
   g_assert_cmpfloat(t_after.d_y, ==, t_before.d_y);
   g_assert_cmpfloat(t_after.d_w, ==, t_before.d_w);
   g_assert_cmpfloat(t_after.d_h, ==, t_before.d_h);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   tool_key(fx.p_win, GDK_KEY_Escape);
   tool_fx_close(&fx);
}

/* The crop tool on the same CANCEL lets go and keeps the rectangle as the
 * drag left it before the pinch (what the user watched land); what the
 * finger reports after the pinch began moves nothing. Here: the corner
 * dragged from (400,300) to (300,200) before the pinch, further after it;
 * Enter crops 300x200. */
static void
test_pinch_in_crop_keeps_the_dragged_rect(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.crop");
   GgazeViewer    *p_v = large_viewer(fx.p_win);
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   GtkEventController *p_drag = viewer_drag_gesture(p_v);
   gdouble             d_s    = g.d_scale;
   g_signal_emit_by_name(p_drag, "drag-begin", g.d_x + TOOL_W * d_s,
                         g.d_y + TOOL_H * d_s);
   g_signal_emit_by_name(p_drag, "drag-update", -100 * d_s, -100 * d_s);
   ggaze_viewer_pinch_begin(p_v, g.d_x + 200 * d_s, g.d_y + 150 * d_s, FALSE);
   g_signal_emit_by_name(p_drag, "drag-update", -250 * d_s, -150 * d_s);
   g_signal_emit_by_name(p_drag, "drag-end", -250 * d_s, -150 * d_s);
   ggaze_viewer_pinch_end(p_v);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, 300, 200);
   tool_fx_close(&fx);
}

/* Take the viewer's picture away and put the same one back: the viewer
 * CANCELs a drag in progress at the set_texture(NULL), with no geometry
 * to map a point through. */
static void
blank_and_restore_viewer(GgazeViewer *p_v) {
   GdkTexture *p_shown = g_object_ref(ggaze_viewer_get_texture(p_v));
   ggaze_viewer_set_texture(p_v, NULL);
   ggaze_viewer_set_texture(p_v, p_shown);
   g_object_unref(p_shown);
}

/* A CANCEL that reaches the straighten tool while the viewer has no
 * texture still drops the line (zb2 third review: the geometry guard used
 * to swallow it, so a stray END afterwards levelled by the finger's
 * jitter). Here the line is (50,100) -> (51,101) in image px when the
 * picture goes; an END with no BEGIN after it levels nothing. */
static void
test_cancel_without_texture_drops_the_line(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.straighten");
   GgazeViewer    *p_v = large_viewer(fx.p_win);
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   GtkEventController *p_drag   = viewer_drag_gesture(p_v);
   guint               u_before = ggaze_window_enhance_render_count(fx.p_win);
   gdouble             d_x0     = g.d_x + 50 * g.d_scale;
   gdouble             d_y0     = g.d_y + 100 * g.d_scale;
   g_signal_emit_by_name(p_drag, "drag-begin", d_x0, d_y0);
   g_signal_emit_by_name(p_drag, "drag-update", g.d_scale, g.d_scale);
   blank_and_restore_viewer(p_v);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, d_x0 + g.d_scale,
                          d_y0 + g.d_scale);
   g_signal_emit_by_name(p_drag, "drag-end", g.d_scale, g.d_scale);
   ggtest_drain_main(300);
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win), ==, u_before);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_STRAIGHTEN);
   tool_key(fx.p_win, GDK_KEY_Escape);
   tool_fx_close(&fx);
}

/* The crop tool on a CANCEL and a REVERT that arrive while the viewer has
 * no texture: the CANCEL lets go of the corner it grabbed and the REVERT
 * puts the rectangle back -- so neither the jitter nor a stray UPDATE
 * afterwards (which, with the grab kept, resized the rectangle) is left on
 * it (zb2 third review). */
static void
test_cancel_without_texture_lets_go_of_the_rect(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.crop");
   GgazeViewer    *p_v = large_viewer(fx.p_win);
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   CropRect t_before, t_after;
   gint     i_bw, i_bh;
   g_assert_true(
      ggaze_window_tool_crop_rect(fx.p_win, &t_before, &i_bw, &i_bh));
   GtkEventController *p_drag = viewer_drag_gesture(p_v);
   gdouble             d_cx   = g.d_x + TOOL_W * g.d_scale;
   gdouble             d_cy   = g.d_y + TOOL_H * g.d_scale;
   g_signal_emit_by_name(p_drag, "drag-begin", d_cx, d_cy);
   g_signal_emit_by_name(p_drag, "drag-update", -6.0, -4.0); /* jitter */
   GdkTexture *p_shown = g_object_ref(ggaze_viewer_get_texture(p_v));
   ggaze_viewer_set_texture(p_v, NULL); /* the CANCEL */
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_REVERT, d_cx - 6.0,
                          d_cy - 4.0);
   ggaze_viewer_set_texture(p_v, p_shown);
   g_object_unref(p_shown);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_UPDATE, d_cx - 100.0,
                          d_cy - 80.0);
   g_signal_emit_by_name(p_drag, "drag-end", -6.0, -4.0);
   g_assert_true(ggaze_window_tool_crop_rect(fx.p_win, &t_after, &i_bw, &i_bh));
   g_assert_cmpfloat(t_after.d_x, ==, t_before.d_x);
   g_assert_cmpfloat(t_after.d_y, ==, t_before.d_y);
   g_assert_cmpfloat(t_after.d_w, ==, t_before.d_w);
   g_assert_cmpfloat(t_after.d_h, ==, t_before.d_h);
   tool_key(fx.p_win, GDK_KEY_Escape);
   tool_fx_close(&fx);
}

/* A discard while a tool is up ends the tool BEFORE the transform is reset
 * -- the panel's Revert button and x (6i2; the Original card and `0` did
 * before) both go through it -- so the straighten session's working angle
 * cannot come back: afterwards
 * `l` is not a tool key any more (no tool, so no overlay either) and the
 * title stays without an angle. Before this hook tool-ctrl kept its
 * t_work / t_saved across the discard and the next nudge re-applied the
 * "discarded" angle. */
static void
test_discard_ends_the_straighten_tool(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance"); /* the panel: its Revert button and x */
   GtkWidget *p_panel = find_panel(fx.p_win);
   g_assert_nonnull(p_panel);
   fire(fx.p_win, "win.straighten");
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   g_assert_nonnull(
      g_strstr_len(window_title(fx.p_win), -1, "straighten 1.0° CW"));
   ggtest_click_button(find_action_button(p_panel, "win.edit-revert"));
   ggtest_drain_main(300);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_false(ggaze_window_tool_key(fx.p_win, GDK_KEY_l, 0)); /* no tool */
   ggtest_drain_main(300);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   /* x is the same discard; `0` under the tool is only a zoom now. */
   fire(fx.p_win, "win.straighten");
   tool_key_and_wait(fx.p_win, GDK_KEY_h);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   fire(fx.p_win, "win.zoom-reset");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_STRAIGHTEN);
   fire(fx.p_win, "win.edit-revert");
   ggtest_drain_main(300);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_false(ggaze_window_tool_key(fx.p_win, GDK_KEY_l, 0));
   ggtest_drain_main(300);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   tool_fx_close(&fx);
}

/* The same for a quarter turn under the crop tool: `]`, `c`, the Original
 * card, then Enter -- the global Enter, since no tool is left -- must not
 * bring the turn back (the crop tool's t_work carried the turn and Enter
 * used to commit it). */
static void
test_discard_ends_the_crop_tool_and_its_turn(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire_and_wait(fx.p_win, "win.rotate-cw");
   assert_texture_size(fx.p_win, TOOL_H, TOOL_W);
   GtkWidget *p_panel = find_panel(fx.p_win); /* `]` opened it (6i2) */
   g_assert_nonnull(p_panel);
   fire(fx.p_win, "win.crop");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   crop_shrink_right(fx.p_win); /* some work in the tool */
   ggtest_click_button(find_action_button(p_panel, "win.edit-revert"));
   ggtest_drain_main(300);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_false(ggaze_window_tool_key(fx.p_win, GDK_KEY_Return, 0));
   fire(fx.p_win, "win.enter-large"); /* what a real Enter activates */
   ggtest_drain_main(300);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "90°"));
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "crop"));
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   tool_fx_close(&fx);
}

/* While the crop tool's base size is still unknown -- `c` right after a
 * jump to a file two positions away, which no prefetch cached and whose
 * load has not landed -- only the tool's OWN keys are consumed (with the
 * still-rendering status); q, s, ?, t and Space propagate to their usual
 * meaning. Before, every plain key was swallowed in that state and `q`
 * could not quit. Once the load lands the same key edits the rectangle. */
static void
test_crop_tool_passes_foreign_keys_while_base_unknown(void) {
   ToolFx fx;
   tool_fx_open_siblings(&fx, 2); /* a.jpg, b.jpg, tool.png */
   fire(fx.p_win, "win.first");   /* a.jpg: two away, never prefetched */
   fire(fx.p_win, "win.crop");    /* before its load lands */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_false(emit_capture_key(fx.p_win, GDK_KEY_q));
   g_assert_false(emit_capture_key(fx.p_win, GDK_KEY_s));
   g_assert_false(emit_capture_key(fx.p_win, GDK_KEY_question));
   g_assert_false(emit_capture_key(fx.p_win, GDK_KEY_t));
   g_assert_false(ggaze_window_tool_key(fx.p_win, GDK_KEY_space, 0));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_true(emit_capture_key(fx.p_win, GDK_KEY_h)); /* ours: stopped */
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Preview still"));
   g_assert_true(ggaze_window_tool_key(fx.p_win, GDK_KEY_a, 0));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_true(ggaze_window_tool_key(fx.p_win, GDK_KEY_Return, 0));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   wait_for_texture_size(fx.p_win, 6, 3); /* the load landed: a base */
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "a.jpg"));
   g_assert_false(ggaze_window_tool_key(fx.p_win, GDK_KEY_q, 0));
   tool_key(fx.p_win, GDK_KEY_h);      /* laid out now: no "still rendering" */
   tool_key(fx.p_win, GDK_KEY_Return); /* ... and Enter is accepted */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Crop removed"));
   tool_fx_close(&fx);
}

/* A pointer drag in the crop tool is ignored while the texture on screen is
 * not the base the rectangle is laid out on (the base preview is still
 * rendering: here a `]` in flight under `c`), exactly as the overlay is not
 * drawn then -- pointer pixels mapped through the old image's geometry would
 * edit a rectangle the user cannot see. Once the base has landed the same
 * gesture resizes. */
static void
test_crop_drag_ignored_while_sizes_disagree(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   GgazeViewer *p_v = GGAZE_VIEWER(
      gtk_stack_get_child_by_name(ggaze_window_get_stack(fx.p_win), "large"));
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   g_assert_cmpint(g.i_img_w, ==, TOOL_W);
   fire(fx.p_win, "win.rotate-cw"); /* in flight: 400x300 on screen, the
                                     * rectangle's base is 300x400 */
   fire(fx.p_win, "win.crop");
   gdouble d_s = g.d_scale;
   /* The bottom-right corner dragged inward: on a visible rectangle this
    * resizes it to 200x100. */
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN,
                          g.d_x + TOOL_W * d_s, g.d_y + TOOL_H * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_UPDATE, g.d_x + 200 * d_s,
                          g.d_y + 100 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 200 * d_s,
                          g.d_y + 100 * d_s);
   wait_for_texture_change(fx.p_win, fx.p_orig);
   assert_texture_size(fx.p_win, TOOL_H, TOOL_W);
   tool_key(fx.p_win, GDK_KEY_Return); /* still the whole base: no crop */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Crop removed"));
   ggtest_drain_main(300);
   assert_texture_size(fx.p_win, TOOL_H, TOOL_W);
   /* Now the sizes agree: the same gesture on the turned image resizes. */
   fire(fx.p_win, "win.crop");
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   g_assert_cmpint(g.i_img_w, ==, TOOL_H);
   d_s = g.d_scale;
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN,
                          g.d_x + TOOL_H * d_s, g.d_y + TOOL_W * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 200 * d_s,
                          g.d_y + 100 * d_s);
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, 200, 100);
   tool_fx_close(&fx);
}

/* A horizon drag's slope is measured on the texture on screen, which lags
 * the working angle while a render is pending: twenty fast `l` presses
 * (10 degrees, one render in flight) and a 10-degree drag used to level by
 * 20. The END is refused with the still-rendering status and the angle
 * stays; once the render has landed the same drag adds its -10 degrees
 * and the image is back at 0 (the original, no render). */
static void
test_straighten_drag_refused_while_rendering(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.straighten");
   GgazeViewer *p_v = GGAZE_VIEWER(
      gtk_stack_get_child_by_name(ggaze_window_get_stack(fx.p_win), "large"));
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   gdouble d_s = g.d_scale;
   for (guint u = 0; u < 20; u++) {
      tool_key(fx.p_win, GDK_KEY_l); /* 10 degrees CW, render in flight */
   }
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN, g.d_x + 50 * d_s,
                          g.d_y + 100 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 150 * d_s,
                          g.d_y + 117.63 * d_s);
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Preview still"));
   Transform t;
   transform_init(&t);
   t.d_degrees = 10.0;
   gdouble d_w, d_h;
   transform_base_size(&t, TOOL_W, TOOL_H, &d_w, &d_h);
   wait_for_texture_size(fx.p_win, (gint)d_w, (gint)d_h);
   ggtest_drain_main(300);
   g_assert_nonnull(
      g_strstr_len(window_title(fx.p_win), -1, "straighten 10.0° CW"));
   /* The render is on screen now: the same slope levels it, 10 - 10 = 0. */
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   d_s = g.d_scale;
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN, g.d_x + 50 * d_s,
                          g.d_y + 100 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 150 * d_s,
                          g.d_y + 117.63 * d_s);
   ggtest_drain_main(300);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Straighten 0.0° —"));
   tool_key(fx.p_win, GDK_KEY_Escape);
   tool_fx_close(&fx);
}

/* With Space held the viewer shows the original, so a horizon dragged on it
 * would be measured against 0 degrees and added to the current angle. The
 * END is refused ("Release Space first") and the angle stays; after the
 * release the same drag adds its -10 degrees to the 1 degree there. */
static void
test_straighten_drag_refused_while_holding_original(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.straighten");
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   tool_key_and_wait(fx.p_win, GDK_KEY_l); /* 1.0 CW */
   GgazeViewer *p_v = GGAZE_VIEWER(
      gtk_stack_get_child_by_name(ggaze_window_get_stack(fx.p_win), "large"));
   GdkTexture *p_tilted = ref_viewer_texture(fx.p_win);
   ggaze_window_set_hold_original(fx.p_win, TRUE);
   ggtest_drain_main(50);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   gdouble d_s = g.d_scale;
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN, g.d_x + 50 * d_s,
                          g.d_y + 100 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 150 * d_s,
                          g.d_y + 117.63 * d_s);
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Release Space"));
   ggtest_drain_main(300);
   g_assert_nonnull(
      g_strstr_len(window_title(fx.p_win), -1, "straighten 1.0° CW"));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig); /* still held */
   ggaze_window_set_hold_original(fx.p_win, FALSE);
   ggtest_drain_main(50);
   g_assert_true(viewer_texture(fx.p_win) == p_tilted);
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   d_s = g.d_scale;
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN, g.d_x + 50 * d_s,
                          g.d_y + 100 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 150 * d_s,
                          g.d_y + 117.63 * d_s);
   wait_for_texture_change(fx.p_win, p_tilted);
   g_assert_nonnull(
      g_strstr_len(window_title(fx.p_win), -1, "straighten 9.0° CCW"));
   g_object_unref(p_tilted);
   tool_key(fx.p_win, GDK_KEY_Escape);
   tool_fx_close(&fx);
}

/* Two quick `]` then `c`: the rectangle's base is 400x300 (180 degrees) and
 * so is the un-turned original still on screen, so a size comparison let
 * the rectangle be drawn over -- and dragged on -- the wrong picture. The
 * guard is the render's identity now: the corner drag is ignored and Enter
 * refused until the 180-degree render itself is on screen. */
static void
test_crop_tool_waits_for_the_exact_render(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   GgazeViewer *p_v = GGAZE_VIEWER(
      gtk_stack_get_child_by_name(ggaze_window_get_stack(fx.p_win), "large"));
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   gdouble d_s = g.d_scale;
   fire(fx.p_win, "win.rotate-cw"); /* in flight ... */
   fire(fx.p_win, "win.rotate-cw"); /* ... and queued: 180, same size */
   fire(fx.p_win, "win.crop");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN,
                          g.d_x + TOOL_W * d_s, g.d_y + TOOL_H * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 200 * d_s,
                          g.d_y + 100 * d_s);
   tool_key(fx.p_win, GDK_KEY_Return);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Preview still"));
   wait_for_texture_change(fx.p_win, fx.p_orig);    /* 90: 300x400 */
   wait_for_texture_size(fx.p_win, TOOL_W, TOOL_H); /* 180: 400x300 */
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "180°"));
   tool_key(fx.p_win, GDK_KEY_Return); /* the drag never happened */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Crop removed"));
   ggtest_drain_main(300);
   assert_texture_size(fx.p_win, TOOL_W, TOOL_H);
   /* On the landed render the same gesture resizes. */
   fire(fx.p_win, "win.crop");
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   d_s = g.d_scale;
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN,
                          g.d_x + TOOL_W * d_s, g.d_y + TOOL_H * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 200 * d_s,
                          g.d_y + 100 * d_s);
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, 200, 100);
   tool_fx_close(&fx);
}

/* A preset toggled with the crop tool up renders a same-size base with new
 * pixels: the drag on the old ones is ignored until the render lands, then
 * the same gesture resizes. A size comparison could not tell the two. */
static void
test_crop_tool_waits_for_a_same_size_preset_render(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   GgazeViewer *p_v = GGAZE_VIEWER(
      gtk_stack_get_child_by_name(ggaze_window_get_stack(fx.p_win), "large"));
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   gdouble d_s = g.d_scale;
   fire(fx.p_win, "win.crop");
   fire(fx.p_win, "win.enhance-1"); /* Auto-fix under the tool: in flight */
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN,
                          g.d_x + TOOL_W * d_s, g.d_y + TOOL_H * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 200 * d_s,
                          g.d_y + 100 * d_s);
   wait_for_texture_change(fx.p_win, fx.p_orig);
   assert_texture_size(fx.p_win, TOOL_W, TOOL_H);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   tool_key(fx.p_win, GDK_KEY_Return); /* the whole base: no crop */
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Crop removed"));
   fire(fx.p_win, "win.crop");
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   d_s = g.d_scale;
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN,
                          g.d_x + TOOL_W * d_s, g.d_y + TOOL_H * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 200 * d_s,
                          g.d_y + 100 * d_s);
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, 200, 100);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "Auto-fix"));
   tool_fx_close(&fx);
}

/* The gate's Discard ends a straighten tool through the discard hook
 * ALONE: in a one-file folder win.next has nowhere to go (navigator_next
 * returns FALSE without a "changed"), so tool_ctrl_nav_changed never runs
 * and only enhance_ctrl_discard's abandon_tool can have ended the tool. */
static void
test_gate_discard_ends_the_straighten_tool(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.straighten");
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   GtkWindow *p_own = GTK_WINDOW(fx.p_win);
   fire(fx.p_win, "win.next");
   GGTEST_ASSERT_DIALOG_UP(p_own, "Discard");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_STRAIGHTEN);
   g_assert_true(ggtest_click_dialog_button(p_own, "Discard"));
   ggtest_drain_main(400);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "tool.png"));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   g_assert_false(ggaze_window_tool_key(fx.p_win, GDK_KEY_l, 0)); /* no tool */
   ggtest_drain_main(300);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   tool_fx_close(&fx);
}

/* The slideshow timer discards a dirty preview without asking; with a tool
 * up that goes through the same hook, so the tool is gone after the first
 * tick. A one-file folder again: the tick's navigator_next is a no-op and
 * nothing but the discard could have ended it. */
static void
test_slideshow_discard_ends_the_tool(void) {
   Settings *p_s = settings_new();
   settings_set_slideshow_delay(p_s, 0.2);
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.crop");
   for (guint u = 0; u < 10; u++) {
      crop_shrink_right(fx.p_win);
   }
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   fire(fx.p_win, "win.straighten");
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   fire(fx.p_win, "win.slideshow");
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Slideshow started"));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_STRAIGHTEN);
   ggtest_drain_main(600); /* at least one tick */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "crop"));
   g_assert_false(ggaze_window_tool_key(fx.p_win, GDK_KEY_l, 0)); /* no tool */
   fire(fx.p_win, "win.slideshow");
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Slideshow stopped"));
   g_settings_reset(settings_get_gsettings(p_s), "slideshow-delay");
   settings_delete(p_s);
   tool_fx_close(&fx);
}

/* A render that fails -- the file made unreadable under the tool -- discards
 * the preview from _apply_done_cb, and that discard ends the tool too:
 * after "Enhance failed" no straighten session is left to re-apply its
 * angle. The folder monitor ignores an attribute change, so nothing else
 * moves. Skipped as root, which reads a mode-000 file regardless. */
static void
test_failed_render_ends_the_tool(void) {
   if (geteuid() == 0) {
      g_test_skip("an unreadable file is readable as root");
      return;
   }
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.straighten");
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   g_assert_cmpint(g_chmod(fx.c_path, 0), ==, 0);
   tool_key(fx.p_win, GDK_KEY_l); /* this render decodes the file: fails */
   /* The mode is restored BEFORE the first assertion: a failing assertion
    * aborts the process, and the mode-000 file used to outlive it in the
    * temp folder (the poll asserts nothing). */
   const char *c_status = poll_for_status_prefix(fx.p_win, "Enhance failed");
   g_assert_cmpint(g_chmod(fx.c_path, 0644), ==, 0);
   g_assert_true(g_str_has_prefix(c_status, "Enhance failed"));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_false(ggaze_window_tool_key(fx.p_win, GDK_KEY_l, 0)); /* no tool */
   ggtest_drain_main(300);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   tool_fx_close(&fx);
}

/* --- wb2 fourth review round ------------------------------------------- */

/* A refused BEGIN grabs nothing. Gesture 1 grabs the bottom-right corner
 * and is left as a 300x200 rectangle without an END (GTK cancels a gesture
 * without one; an END refused while a render was pending used to leave
 * the grab too). A preset toggled next puts a render in flight, so gesture
 * 2's BEGIN -- inside the rectangle, a move -- is refused ("Preview still
 * rendering"); once that render has landed its UPDATE and END are
 * accepted. They used to continue gesture 1: the corner grab from
 * (400,300) re-derived at (150,150) shrank the rectangle to 150x150. Now
 * they belong to a gesture that grabbed nothing and the rectangle is still
 * the 300x200 gesture 1 left. A BEGIN under a held Space is refused with
 * the other reason. */
static void
test_refused_drag_begin_grabs_nothing(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.crop");
   GgazeViewer *p_v = GGAZE_VIEWER(
      gtk_stack_get_child_by_name(ggaze_window_get_stack(fx.p_win), "large"));
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   gdouble d_s = g.d_scale;
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN,
                          g.d_x + TOOL_W * d_s, g.d_y + TOOL_H * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_UPDATE, g.d_x + 300 * d_s,
                          g.d_y + 200 * d_s);
   fire(fx.p_win, "win.enhance-1"); /* in flight: same size, new pixels */
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN, g.d_x + 100 * d_s,
                          g.d_y + 100 * d_s);
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Preview still"));
   wait_for_texture_change(fx.p_win, fx.p_orig);
   assert_texture_size(fx.p_win, TOOL_W, TOOL_H);
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   d_s = g.d_scale;
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_UPDATE, g.d_x + 150 * d_s,
                          g.d_y + 150 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 150 * d_s,
                          g.d_y + 150 * d_s);
   ggaze_window_set_hold_original(fx.p_win, TRUE);
   ggtest_drain_main(50);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN, g.d_x + 100 * d_s,
                          g.d_y + 100 * d_s);
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Release Space"));
   ggaze_window_set_hold_original(fx.p_win, FALSE);
   ggtest_drain_main(50);
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, 300, 200);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "Auto-fix"));
   tool_fx_close(&fx);
}

/* The overlay's "is this the base?" test is an identity the controller
 * remembers, not a texture-cache lookup: the cache's get stats the file and
 * evicts a stale entry, and doing that from every snapshot and pointer
 * motion made a `touch` on the file (its mtime bumped -- an attribute
 * change the folder monitor ignores, so nothing reloads) drop the
 * rectangle mid-session. Here the file is touched AND the cache emptied
 * under an open crop tool, and then the same file is RELOADED under it (a
 * preset toggled on and off again: the restore misses the emptied cache
 * and decodes a new texture object): the next drag still moves the
 * rectangle and Enter still commits it. Before the round-4 fix the drag
 * was ignored and Enter answered "Preview still rendering" for ever; a
 * remembered identity that was never refreshed (round 4) passed the touch
 * and failed the reload the same way, which is what the reload step tells
 * apart. */
static void
test_crop_overlay_survives_a_touch(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.crop");
   GgazeViewer *p_v = GGAZE_VIEWER(
      gtk_stack_get_child_by_name(ggaze_window_get_stack(fx.p_win), "large"));
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   gdouble d_s = g.d_scale;
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN,
                          g.d_x + TOOL_W * d_s, g.d_y + TOOL_H * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 300 * d_s,
                          g.d_y + 200 * d_s);
   touch_file(fx.c_path);
   ggaze_window_clear_texture_cache(fx.p_win); /* the entry is gone */
   ggtest_drain_main(400); /* past the monitor's debounce: no rescan */
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   fire_and_wait(fx.p_win, "win.enhance-1"); /* a preset under the tool */
   GdkTexture *p_render = ref_viewer_texture(fx.p_win);
   fire(fx.p_win, "win.enhance-1"); /* off again: the restore reloads */
   wait_for_fresh_texture(fx.p_win, p_render, fx.p_orig);
   g_object_unref(p_render);
   assert_texture_size(fx.p_win, TOOL_W, TOOL_H);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN, g.d_x + 100 * d_s,
                          g.d_y + 100 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 120 * d_s,
                          g.d_y + 120 * d_s);
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, 300, 200);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "crop"));
   tool_fx_close(&fx);
}

/* `c` over a crop that a straighten pushed entirely outside the view: that
 * crop crops nothing (its intersection with the base is empty), so the
 * whole base is what is applied, and that is what the tool starts from.
 * Clamping the stored rectangle into the base used to hand out an 8-px
 * sliver at the nearest corner that nobody drew, and Enter committed it. */
static void
test_crop_tool_over_an_outside_crop_starts_full(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.crop");
   for (guint u = 0; u < 131; u++) {
      crop_shrink_right(fx.p_win); /* right edge in to the minimum ... */
   }
   for (guint u = 0; u < 131; u++) {
      tool_key(fx.p_win, GDK_KEY_l); /* ... then slid to the right border */
   }
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, 8, TOOL_H);
   fire(fx.p_win, "win.straighten");
   for (guint u = 0; u < 20; u++) {
      tool_key(fx.p_win, GDK_KEY_l); /* 10 degrees: the crop is outside */
   }
   Transform t;
   transform_init(&t);
   t.d_degrees = 10.0;
   gdouble d_bw, d_bh;
   transform_base_size(&t, TOOL_W, TOOL_H, &d_bw, &d_bh);
   wait_for_texture_size(fx.p_win, (gint)d_bw, (gint)d_bh);
   tool_key(fx.p_win, GDK_KEY_Return); /* keep the angle; the crop stays */
   g_assert_nonnull(
      g_strstr_len(window_title(fx.p_win), -1, "crop (outside view)"));
   GdkTexture *p_before = ref_viewer_texture(fx.p_win);
   fire(fx.p_win, "win.crop"); /* the base without the crop: rendered again */
   wait_for_texture_change(fx.p_win, p_before);
   assert_texture_size(fx.p_win, (gint)d_bw, (gint)d_bh);
   tool_key(fx.p_win, GDK_KEY_Return); /* the whole base, as laid out */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Crop removed"));
   ggtest_drain_main(300);
   assert_texture_size(fx.p_win, (gint)d_bw, (gint)d_bh);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "crop"));
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "straighten"));
   g_object_unref(p_before);
   tool_fx_close(&fx);
}

/* The current file rewritten in place with another size (an external edit
 * through `e`, a `!` script): the folder monitor's rescan keeps the file's
 * identity, and the controller used to keep the old original's size with
 * it, so `c` afterwards laid its rectangle out on 400x300 over a 200x150
 * picture. A same-file rescan now re-checks the original against the cache
 * (stale: evicted) and forgets it, and `c` after the reload lays out on
 * the new base: ten `H` presses (1 px each on the smaller image) crop to
 * 190x150. On the old base they were 3 px each on 400, and the enhancer
 * intersected that with the real 200x150 image: nothing cropped. A preset
 * is applied and discarded first: that is what records the size from a
 * decode and makes the rescan a same-file one for the controller (with no
 * preview ever launched it re-derived everything anyway), and it is the
 * realistic order -- enhance, discard, edit the file outside. */
static void
test_rewritten_file_rebases_the_crop_tool(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire_and_wait(fx.p_win, "win.enhance-1"); /* the decode says 400x300 */
   revert_edits(fx.p_win);                   /* x: discarded, original */
   ggtest_drain_main(100);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   rewrite_as_200x150(fx.c_path);
   wait_for_texture_size(fx.p_win, 200, 150); /* the rescan reloaded it */
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "tool.png"));
   fire(fx.p_win, "win.crop");
   for (guint u = 0; u < 10; u++) {
      crop_shrink_right(fx.p_win);
   }
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, 190, 150);
   tool_fx_close(&fx);
}

/* The original's identity is learned where the viewer learns it -- the
 * window's texture choke point, for every decoded texture it shows -- not
 * once from the cache. A same-file reload WITHOUT a rescan (`c`, Esc, a
 * preset, the file touched, Esc: the discard's restore finds the cache
 * entry stale and decodes a NEW texture object for the same file) used to
 * leave the remembered identity on the old object: every later `c` +
 * Enter was refused with "Preview still rendering", the rectangle never
 * drawn, a horizon drag at 0 degrees refused the same way, and Space (a
 * no-op with nothing active) could not cure it. Now the reload teaches the
 * controller the new object and both tools work on it at once. */
static void
test_reload_refreshes_the_original_identity(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.crop"); /* the tool learns the original ... */
   fire(fx.p_win, "win.back"); /* ... and Esc cancels it */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   fire_and_wait(fx.p_win, "win.enhance-1");
   GdkTexture *p_render = ref_viewer_texture(fx.p_win);
   touch_file(fx.c_path);  /* the cache entry is stale now */
   revert_edits(fx.p_win); /* x: discarded -> the restore reloads */
   wait_for_fresh_texture(fx.p_win, p_render, fx.p_orig);
   g_object_unref(p_render);
   assert_texture_size(fx.p_win, TOOL_W, TOOL_H);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   GgazeViewer *p_v = GGAZE_VIEWER(
      gtk_stack_get_child_by_name(ggaze_window_get_stack(fx.p_win), "large"));
   GgazeViewerGeom g;
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   gdouble d_s = g.d_scale;
   fire(fx.p_win, "win.straighten"); /* a horizon at 0 degrees: accepted */
   GdkTexture *p_before = ref_viewer_texture(fx.p_win);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN, g.d_x + 50 * d_s,
                          g.d_y + 100 * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 150 * d_s,
                          g.d_y + 117.63 * d_s);
   wait_for_texture_change(fx.p_win, p_before);
   g_object_unref(p_before);
   g_assert_nonnull(
      g_strstr_len(window_title(fx.p_win), -1, "straighten 10.0° CCW"));
   fire(fx.p_win, "win.back"); /* Esc: back to 0 degrees, reloaded again */
   ggtest_drain_main(300);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   assert_texture_size(fx.p_win, TOOL_W, TOOL_H);
   fire(fx.p_win, "win.crop");
   g_assert_true(ggaze_viewer_get_geometry(p_v, &g));
   d_s = g.d_scale;
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_BEGIN,
                          g.d_x + TOOL_W * d_s, g.d_y + TOOL_H * d_s);
   ggaze_window_tool_drag(fx.p_win, GGAZE_VIEWER_DRAG_END, g.d_x + 300 * d_s,
                          g.d_y + 200 * d_s);
   tool_key_and_wait(fx.p_win, GDK_KEY_Return); /* accepted, not refused */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   assert_texture_size(fx.p_win, 300, 200);
   tool_fx_close(&fx);
}

/* A preset is active when the file is rewritten in place with another
 * size: the rescan finds the original evicted (a real rewrite, not the
 * "-enhanced" copy a save writes next to it, whose rescan keeps a fresh
 * entry and must not re-render -- the render count says so) and renders
 * the preview again from the file as it is now, so what is on screen is a
 * NEW render of the NEW size, and hold-Space compares it against the new
 * original. Before the fix the preview kept showing the old 400x300 render
 * over a 200x150 file until the next state change. */
static void
test_rewrite_under_a_preset_rerenders(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire_and_wait(fx.p_win, "win.enhance-1");
   GdkTexture *p_r1 = ref_viewer_texture(fx.p_win);
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win), ==, 1);
   fire(fx.p_win, "win.enhance-save");
   char *c_out = g_build_filename(fx.c_dir, "tool-enhanced.png", NULL);
   wait_for_file(c_out);
   g_free(c_out);
   ggtest_drain_main(700); /* past the monitor's debounce: the copy's
                            * rescan, with a fresh entry: no re-render */
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win), ==, 1);
   g_assert_true(viewer_texture(fx.p_win) == p_r1);
   rewrite_as_200x150(fx.c_path);
   wait_for_texture_size(fx.p_win, 200, 150); /* a new render, new size */
   g_assert_true(viewer_texture(fx.p_win) != p_r1);
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win), ==, 2);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "Auto-fix"));
   GdkTexture *p_r2 = ref_viewer_texture(fx.p_win);
   ggaze_window_set_hold_original(fx.p_win, TRUE);
   ggtest_drain_main(50);
   GdkTexture *p_held = viewer_texture(fx.p_win);
   g_assert_nonnull(p_held);
   g_assert_true(p_held != p_r2 && p_held != fx.p_orig); /* the new one */
   assert_texture_size(fx.p_win, 200, 150);
   ggaze_window_set_hold_original(fx.p_win, FALSE);
   ggtest_drain_main(50);
   g_assert_true(viewer_texture(fx.p_win) == p_r2);
   g_object_unref(p_r2);
   g_object_unref(p_r1);
   tool_fx_close(&fx);
}

/* The crop tool is OPEN when the file is rewritten in place with another
 * size: once the rescan's reload has put the new picture on screen the
 * rectangle is laid out on the new base and drawn over it with no key
 * pressed and nothing dragged (the reload teaches the controller the new
 * original, and the controller tells the tool). It used to stay hidden --
 * laid out on the old 400x300 base, over a picture that was no longer it
 * -- until the first key or drag laid it out again. */
static void
test_crop_tool_relays_out_on_the_rewritten_base(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.crop");
   CropRect t_rect;
   gint     i_bw, i_bh;
   g_assert_true(ggaze_window_tool_crop_rect(fx.p_win, &t_rect, &i_bw, &i_bh));
   g_assert_cmpint(i_bw, ==, TOOL_W);
   g_assert_cmpint(i_bh, ==, TOOL_H);
   rewrite_as_200x150(fx.c_path);
   wait_for_texture_size(fx.p_win, 200, 150); /* the rescan reloaded it */
   ggtest_drain_main(100);                    /* a frame, nothing else */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_true(ggaze_window_tool_crop_rect(fx.p_win, &t_rect, &i_bw, &i_bh));
   g_assert_cmpint(i_bw, ==, 200);
   g_assert_cmpint(i_bh, ==, 150);
   g_assert_true(croprect_is_full(&t_rect, 200.0, 150.0));
   for (guint u = 0; u < 10; u++) {
      crop_shrink_right(fx.p_win);
   }
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, 190, 150);
   tool_fx_close(&fx);
}

/* --- wb2 sixth review round ----------------------------------------------
 *
 * An open (`o`, a drop, a single-instance activation) rebuilds the
 * navigator, and navigator_set_current_file emits no "changed" when the
 * file sorts first in its folder (a folder open places no cursor at all),
 * so the two nav_changed choke points never ran for it: a tool and a
 * transform left over from the previous file survived into the new folder.
 * The open now runs the same choke point a navigation does
 * (window.c _open_rebuild). The other subtests here pin the render of a
 * rewritten file landing before the file's own decode, and the baseline an
 * open leaves for the folder's first rescan. */

/* A fresh folder holding one 200x150 PNG, "b.png": the only file, so it
 * sorts first and its open never emits "changed". */
static char *
make_first_sorted_dir(char **c_path_out) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-open-b-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   char *c_path = g_build_filename(c_dir, "b.png", NULL);
   rewrite_as_200x150(c_path);
   *c_path_out = c_path;
   return (c_dir);
}

/* What every open of B must leave behind, whatever A had going: B's own
 * 200x150 decode on screen (never A's render under B's title), no tool,
 * no rectangle, no transform in the title, nothing dirty. */
static void
assert_b_opened_clean(GgazeWindow *p_win) {
   wait_for_texture_size(p_win, 200, 150);
   g_assert_nonnull(g_strstr_len(window_title(p_win), -1, "b.png"));
   g_assert_null(g_strstr_len(window_title(p_win), -1, "°"));
   g_assert_cmpint(ggaze_window_get_tool(p_win), ==, GGAZE_TOOL_NONE);
   CropRect t_rect;
   gint     i_bw, i_bh;
   g_assert_false(ggaze_window_tool_crop_rect(p_win, &t_rect, &i_bw, &i_bh));
   g_assert_false(ggaze_window_enhance_is_dirty(p_win));
}

/* A's turn saved (not dirty: the open asks nothing) and the crop tool up
 * over it; then a first-sorted B in another folder is opened. Before the
 * fix nothing reset: B decoded and was learned as the original, but the
 * override still returned A's turned render -- A's picture under B's
 * title, the rectangle drawn over it, Enter applying A's turn and crop to
 * B, and `s` exporting A once more. Now: tool gone, transform gone, B's
 * own decode, and `s` has nothing to save. */
static void
test_open_ends_the_tool_and_the_saved_transform(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire_and_wait(fx.p_win, "win.rotate-cw");
   assert_texture_size(fx.p_win, TOOL_H, TOOL_W);
   fire(fx.p_win, "win.enhance-save");
   char *c_out = g_build_filename(fx.c_dir, "tool-enhanced.png", NULL);
   wait_for_file(c_out);
   wait_for_status_prefix(fx.p_win, "Saved ");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   fire(fx.p_win, "win.crop");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   char  *c_b     = NULL;
   char  *c_other = make_first_sorted_dir(&c_b);
   GFile *p_b     = g_file_new_for_path(c_b);
   ggaze_window_open(fx.p_win, p_b); /* saved: no prompt */
   ggtest_drain_main(100);
   g_assert_cmpuint(ggtest_count_dialogs(GTK_WINDOW(fx.p_win), "Cancel"), ==,
                    0);
   assert_b_opened_clean(fx.p_win);
   fire(fx.p_win, "win.enhance-save");
   wait_for_status_prefix(fx.p_win, "Nothing to save");
   ggtest_drain_main(200);
   g_assert_false(file_exists_in(c_other, "b-enhanced.png"));
   g_assert_false(file_exists_in(fx.c_dir, "tool-enhanced-1.png"));
   g_object_unref(p_b);
   g_free(c_b);
   g_free(c_out);
   tool_fx_close(&fx);
   ggtest_cleanup_temp_dir(c_other);
}

/* A dirty (a turn, the crop tool up), the open of a first-sorted B is
 * answered with Save: the export of A lands (turned, 300x400), and B then
 * comes up clean. Before the fix the save went through and the open showed
 * A's turned render under B's title all the same. */
static void
test_open_save_exports_then_shows_the_new_file_clean(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire_and_wait(fx.p_win, "win.rotate-cw");
   fire(fx.p_win, "win.crop");
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   char      *c_b     = NULL;
   char      *c_other = make_first_sorted_dir(&c_b);
   GFile     *p_b     = g_file_new_for_path(c_b);
   GtkWindow *p_own   = GTK_WINDOW(fx.p_win);
   ggaze_window_open(fx.p_win, p_b);
   GGTEST_ASSERT_DIALOG_UP(p_own, "Save");
   g_assert_true(ggtest_click_dialog_button(p_own, "Save"));
   char *c_out = g_build_filename(fx.c_dir, "tool-enhanced.png", NULL);
   wait_for_file(c_out);
   assert_b_opened_clean(fx.p_win);
   GError     *p_err = NULL;
   GdkTexture *p_tex = gdk_texture_new_from_filename(c_out, &p_err);
   g_assert_no_error(p_err);
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, TOOL_H);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, TOOL_W);
   g_object_unref(p_tex);
   g_assert_false(file_exists_in(c_other, "b-enhanced.png"));
   g_object_unref(p_b);
   g_free(c_b);
   g_free(c_out);
   tool_fx_close(&fx);
   ggtest_cleanup_temp_dir(c_other);
}

/* Overwrite c_path IN PLACE (truncate and write: the same inode, which the
 * cache's stamp also checks) as a 200x150 PNG of exactly i_size bytes -- a
 * tEXt chunk pads it: every comment character is one more byte. The
 * folder monitor sees the content change and schedules a rescan. */
static void
write_200x150_padded_in_place(const char *c_path, goffset i_size) {
   GdkPixbuf *p_pix = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 200, 150);
   gdk_pixbuf_fill(p_pix, 0x99cc33ffu);
   gchar  *c_buf = NULL;
   gsize   u_len = 0;
   GError *p_err = NULL;
   g_assert_true(gdk_pixbuf_save_to_buffer(p_pix, &c_buf, &u_len, "png", &p_err,
                                           "tEXt::Comment", "x", NULL));
   g_assert_no_error(p_err);
   g_assert_cmpint((goffset)u_len, <=, i_size); /* the smaller compresses
                                                 * smaller: the premise */
   char *c_pad = g_strnfill(1 + (gsize)(i_size - (goffset)u_len), 'x');
   g_free(c_buf);
   g_assert_true(gdk_pixbuf_save_to_buffer(p_pix, &c_buf, &u_len, "png", &p_err,
                                           "tEXt::Comment", c_pad, NULL));
   g_assert_no_error(p_err);
   g_assert_cmpint((goffset)u_len, ==, i_size);
   FILE *p_fp = fopen(c_path, "wb");
   g_assert_nonnull(p_fp);
   g_assert_cmpuint(fwrite(c_buf, 1, u_len, p_fp), ==, u_len);
   g_assert_cmpint(fclose(p_fp), ==, 0);
   g_free(c_pad);
   g_free(c_buf);
   g_object_unref(p_pix);
}

/* Rewrite c_path as 200x150 keeping the texture cache's WHOLE stamp -- the
 * byte count, the inode (written in place) and the mtime to the
 * nanosecond, copied back -- so the viewer's reload after the folder
 * monitor's rescan is a cache HIT of the old 400x300 decode and only a
 * GEGL render ever reads the new contents: the deterministic form of "the
 * render lands before the decode", which a real rewrite only races. It is
 * also the one rewrite the stamp cannot tell (a same-size rewrite within
 * one coarse mtime tick, or within one second on a filesystem that keeps
 * whole seconds only), so the tool must cope with it rather than the
 * cache. The folder monitor ignores the mtime change itself; the rescan
 * the in-place write scheduled is what follows. */
static void
rewrite_as_200x150_same_stamp(const char *c_path) {
   GgtestFileStamp t_st;
   ggtest_read_stamp(c_path, &t_st);
   write_200x150_padded_in_place(c_path, t_st.i_size);
   ggtest_set_mtime(c_path, t_st.u_sec, t_st.u_nsec);
}

/* Rewrite c_path as 200x150 within the SAME SECOND to the same byte count,
 * in place: whole seconds, size and inode kept, only the sub-second part
 * of the mtime moved (by half a second, so it differs whatever the clock
 * did). The seconds + size stamp read that as unchanged (fd2); the cache
 * must miss on the sub-second part now. */
static void
rewrite_as_200x150_same_second(const char *c_path) {
   GgtestFileStamp t_st;
   ggtest_read_stamp(c_path, &t_st);
   write_200x150_padded_in_place(c_path, t_st.i_size);
   ggtest_set_mtime(c_path, t_st.u_sec,
                    (t_st.u_nsec + 500000000u) % 1000000000u);
}

/* Pump until the window title contains c_part (up to 5 s) and assert it
 * does: a folder rescan's observable effect when the listing changed --
 * the title's "n/total" follows the navigator's count. Draining a fixed
 * time past the monitor's 250 ms debounce instead passed vacuously when
 * the monitor was late (seventh review). */
static void
wait_for_title_part(GgazeWindow *p_win, const char *c_part) {
   for (guint u = 0;
        u < 5000 && g_strstr_len(window_title(p_win), -1, c_part) == NULL;
        u++) {
      g_main_context_iteration(NULL, FALSE);
      g_usleep(1000);
   }
   g_assert_nonnull(g_strstr_len(window_title(p_win), -1, c_part));
   ggtest_drain_main(50);
}

/* Pump until the status line is hidden (up to 2 s) and assert it is: a
 * folder rescan's observable effect when the listing did NOT change (the
 * same file rewritten in place), since the window's "changed" handler
 * dismisses the info overlay unconditionally (_dismiss_info_for_nav)
 * while nothing about the picture, the title or the count moves. The
 * label must be up when this is called, with a status whose auto-hide is
 * further off than the 2 s waited here (the crop hint's is 4 s: 2 s + 1 s
 * per 40 characters), so within the deadline only the rescan can hide it
 * -- a late monitor fails here instead of passing by the fixed drain it
 * replaced (seventh review). */
static void
wait_for_status_dismissed(GgazeWindow *p_win) {
   GtkWidget *p_lbl = ggaze_window_get_info_label(p_win);
   g_assert_true(gtk_widget_get_visible(p_lbl));
   for (guint u = 0; u < 2000 && gtk_widget_get_visible(p_lbl); u++) {
      g_main_context_iteration(NULL, FALSE);
      g_usleep(1000);
   }
   g_assert_false(gtk_widget_get_visible(p_lbl));
   ggtest_drain_main(50);
}

/* The crop tool is open on the 400x300 base when a render decodes the file
 * at another size before any reload has (rewrite_as_200x150_same_stamp:
 * the reload is a cache hit of the old decode, the preset's render reads
 * the 200x150 file). The render's original size is told to the tool as a
 * new original is, so the rectangle is laid out again on 200x150 and
 * reported full on it; before the fix the seam reported the 400x300 layout
 * as drawn over the 200x150 render. Then the keys crop on the true base. */
static void
test_crop_tool_follows_a_render_of_another_size(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.crop");
   CropRect t_rect;
   gint     i_bw, i_bh;
   g_assert_true(ggaze_window_tool_crop_rect(fx.p_win, &t_rect, &i_bw, &i_bh));
   g_assert_cmpint(i_bw, ==, TOOL_W);
   g_assert_cmpint(i_bh, ==, TOOL_H);
   rewrite_as_200x150_same_stamp(fx.c_path);
   wait_for_status_dismissed(fx.p_win); /* the rescan happened (it hides
                                         * the crop hint): the entry was
                                         * fresh, the reload a hit */
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   fire_and_wait(fx.p_win, "win.enhance-1"); /* the render reads 200x150 */
   assert_texture_size(fx.p_win, 200, 150);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_true(ggaze_window_tool_crop_rect(fx.p_win, &t_rect, &i_bw, &i_bh));
   g_assert_cmpint(i_bw, ==, 200);
   g_assert_cmpint(i_bh, ==, 150);
   g_assert_true(croprect_is_full(&t_rect, 200.0, 150.0));
   for (guint u = 0; u < 10; u++) {
      crop_shrink_right(fx.p_win);
   }
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, 190, 150);
   tool_fx_close(&fx);
}

/* --- fd2: the same-second, same-size rewrite ----------------------------
 *
 * The crop tool is open on the 400x300 base when the file is rewritten in
 * place within the same second, to the same byte count
 * (rewrite_as_200x150_same_second). The old stamp (whole seconds + size)
 * read that as unchanged: the rescan's reload was a cache hit of the
 * 400x300 decode, the stale picture stayed on screen, and the first
 * render's 200x150 taught the tool a base the viewer never showed. The
 * sub-second part of the mtime tells the rewrite now: the reload decodes
 * the new picture and shows it, and the rectangle is laid out on it as
 * for any other rewrite (test_crop_tool_relays_out_on_the_rewritten_base;
 * here the cache had every other reason to hit). */
static void
test_same_second_same_size_rewrite_shows_the_new_picture(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.crop");
   CropRect t_rect;
   gint     i_bw, i_bh;
   g_assert_true(ggaze_window_tool_crop_rect(fx.p_win, &t_rect, &i_bw, &i_bh));
   g_assert_cmpint(i_bw, ==, TOOL_W);
   g_assert_cmpint(i_bh, ==, TOOL_H);
   rewrite_as_200x150_same_second(fx.c_path);
   wait_for_texture_size(fx.p_win, 200, 150); /* the rescan's reload was
                                               * a miss: the new decode */
   ggtest_drain_main(100);
   g_assert_true(viewer_texture(fx.p_win) != fx.p_orig);
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win), ==, 0);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_true(ggaze_window_tool_crop_rect(fx.p_win, &t_rect, &i_bw, &i_bh));
   g_assert_cmpint(i_bw, ==, 200);
   g_assert_cmpint(i_bh, ==, 150);
   g_assert_true(croprect_is_full(&t_rect, 200.0, 150.0));
   for (guint u = 0; u < 10; u++) {
      crop_shrink_right(fx.p_win);
   }
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, 190, 150);
   tool_fx_close(&fx);
}

/* An open leaves the controller comparing against the folder's current
 * file, so the folder's first rescan (a sibling appearing) under an open
 * crop tool -- nothing rendered yet -- is a same-file event: the tool, its
 * rectangle and its base stay, and no render is launched. (With no
 * baseline the rescan read as an identity change; the reload in the same
 * callback re-learned the original, so this guards the baseline rather
 * than a visible failure.) */
static void
test_first_rescan_after_an_open_is_a_same_file_event(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.crop");
   for (guint u = 0; u < 10; u++) {
      crop_shrink_right(fx.p_win);
   }
   CropRect t_rect;
   gint     i_bw, i_bh;
   g_assert_true(ggaze_window_tool_crop_rect(fx.p_win, &t_rect, &i_bw, &i_bh));
   g_assert_cmpfloat(t_rect.d_w, ==, TOOL_W - 30.0); /* 10 steps of 3 px */
   copy_fixture(fx.c_dir, "plain.jpg"); /* a sibling: the folder rescans */
   wait_for_title_part(fx.p_win, "/2"); /* ... and the rescan happened:
                                         * two files in the title now */
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win), ==, 0);
   g_assert_true(ggaze_window_tool_crop_rect(fx.p_win, &t_rect, &i_bw, &i_bh));
   g_assert_cmpint(i_bw, ==, TOOL_W);
   g_assert_cmpfloat(t_rect.d_w, ==, TOOL_W - 30.0);
   tool_key_and_wait(fx.p_win, GDK_KEY_Return);
   assert_texture_size(fx.p_win, TOOL_W - 30, TOOL_H);
   tool_fx_close(&fx);
}

static void
add_tool_tests(void) {
   g_test_add_func("/enhance_flow/rotate_cw_repeats_to_the_original",
                   test_rotate_cw_repeats_to_the_original);
   g_test_add_func("/enhance_flow/rotate_ccw_save_exports_the_turned_copy",
                   test_rotate_ccw_save_exports_the_turned_copy);
   g_test_add_func("/enhance_flow/rotate_composes_with_a_preset",
                   test_rotate_composes_with_a_preset);
   g_test_add_func("/enhance_flow/dirty_rotate_gates_navigation",
                   test_dirty_rotate_gates_navigation);
   g_test_add_func("/enhance_flow/crop_keys_then_enter_commits",
                   test_crop_keys_then_enter_commits);
   g_test_add_func("/enhance_flow/crop_esc_and_toggle_cancel",
                   test_crop_esc_and_toggle_cancel);
   g_test_add_func("/enhance_flow/crop_tool_holds_animation_first_frame",
                   test_crop_tool_holds_animation_first_frame);
   g_test_add_func("/enhance_flow/crop_aspect_presets",
                   test_crop_aspect_presets);
   g_test_add_func("/enhance_flow/crop_drag_resizes_and_moves",
                   test_crop_drag_resizes_and_moves);
   g_test_add_func("/enhance_flow/straighten_nudge_and_autocrop",
                   test_straighten_nudge_and_autocrop);
   g_test_add_func("/enhance_flow/straighten_horizon_drag_levels",
                   test_straighten_horizon_drag_levels);
   g_test_add_func("/enhance_flow/straighten_esc_restores_the_turn",
                   test_straighten_esc_restores_the_turn);
   g_test_add_func("/enhance_flow/tool_refuses_switch_and_turn_while_active",
                   test_tool_refuses_switch_and_turn_while_active);
   g_test_add_func("/enhance_flow/crop_enter_refused_while_rendering",
                   test_crop_enter_refused_while_rendering);
   g_test_add_func("/enhance_flow/crop_turns_with_the_image",
                   test_crop_turns_with_the_image);
   g_test_add_func("/enhance_flow/tool_abandoned_on_navigation_and_view",
                   test_tool_abandoned_on_navigation_and_view_change);
}

/* 7i2: undo / redo of edit steps in the open panel. */
static void
add_edit_undo_tests(void) {
   g_test_add_func("/enhance_flow/undo_redo_a_preset", test_undo_redo_a_preset);
   g_test_add_func("/enhance_flow/undo_redo_chords_buttons_and_cards",
                   test_undo_redo_chords_buttons_and_cards);
   g_test_add_func("/enhance_flow/undo_redo_a_quarter_turn",
                   test_undo_redo_a_quarter_turn);
   g_test_add_func("/enhance_flow/undo_redo_a_crop", test_undo_redo_a_crop);
   g_test_add_func("/enhance_flow/undo_redo_a_straighten",
                   test_undo_redo_a_straighten);
   g_test_add_func("/enhance_flow/undo_under_a_tool", test_undo_under_a_tool);
   g_test_add_func("/enhance_flow/undo_brings_back_a_revert",
                   test_undo_brings_back_a_revert);
   g_test_add_func("/enhance_flow/undo_to_the_saved_state_is_clean",
                   test_undo_to_the_saved_state_is_clean);
   g_test_add_func("/enhance_flow/navigation_clears_the_history",
                   test_navigation_clears_the_history);
   g_test_add_func("/enhance_flow/discard_clears_the_history",
                   test_discard_clears_the_history);
   g_test_add_func("/enhance_flow/slideshow_discard_clears_the_history",
                   test_slideshow_discard_clears_the_history);
   g_test_add_func("/enhance_flow/u_outside_the_panel_undoes_a_trash",
                   test_u_outside_the_panel_undoes_a_trash);
}

/* wb2 review round: the crop tool's preview override, the saved pair, the
 * drag guard, render coalescing, a crop following the base, and the tools'
 * capture-phase key controller. */
static void
add_tool_review_tests(void) {
   g_test_add_func("/enhance_flow/reopened_crop_tool_keeps_the_crop_as_work",
                   test_reopened_crop_tool_keeps_the_crop_as_work);
   g_test_add_func("/enhance_flow/navigation_inside_crop_tool_prompts",
                   test_navigation_inside_crop_tool_prompts);
   g_test_add_func("/enhance_flow/saved_state_survives_a_tool_cancel",
                   test_saved_state_survives_a_tool_cancel);
   g_test_add_func("/enhance_flow/straighten_drag_end_without_begin_ignored",
                   test_straighten_drag_end_without_begin_is_ignored);
   g_test_add_func("/enhance_flow/rapid_nudges_coalesce_into_two_renders",
                   test_rapid_nudges_coalesce_into_two_renders);
   g_test_add_func("/enhance_flow/crop_follows_straighten",
                   test_crop_follows_straighten);
   g_test_add_func("/enhance_flow/crop_outside_the_view_comes_back",
                   test_crop_outside_the_view_comes_back);
   g_test_add_func("/enhance_flow/digits_are_inert_with_the_panel_closed",
                   test_digits_are_inert_with_the_panel_closed);
   g_test_add_func("/enhance_flow/tool_keys_open_the_panel",
                   test_tool_keys_open_the_panel);
   g_test_add_func("/enhance_flow/crop_shift_and_ctrl_move_every_side",
                   test_crop_shift_and_ctrl_move_every_side);
   g_test_add_func("/enhance_flow/hint_bar_follows_the_mode",
                   test_hint_bar_follows_the_mode);
   g_test_add_func("/enhance_flow/caps_lock_keys_in_the_crop_tool",
                   test_caps_lock_keys_in_the_crop_tool);
   g_test_add_func("/enhance_flow/hidden_panel_in_the_grid_is_inert",
                   test_hidden_panel_in_the_grid_is_inert);
   g_test_add_func("/enhance_flow/closing_the_panel_cancels_the_tool",
                   test_closing_the_panel_cancels_the_tool);
   g_test_add_func("/enhance_flow/tool_key_controller_claims_keys_only_active",
                   test_tool_key_controller_claims_keys_only_while_active);
   /* zb2: the touch swipe goes through the same gate and tool rules. */
   g_test_add_func("/enhance_flow/swipe_away_from_dirty_preview_prompts",
                   test_swipe_away_from_dirty_preview_prompts);
   g_test_add_func("/enhance_flow/swipe_refused_while_tool_active",
                   test_swipe_refused_while_tool_active);
   /* zb2 review: a pinch takes a tool drag away with a CANCEL. */
   g_test_add_func("/enhance_flow/pinch_in_straighten_levels_nothing",
                   test_pinch_in_straighten_levels_nothing);
   g_test_add_func("/enhance_flow/pinch_in_crop_keeps_the_dragged_rect",
                   test_pinch_in_crop_keeps_the_dragged_rect);
   g_test_add_func("/enhance_flow/tap_in_crop_leaves_the_rect",
                   test_tap_in_crop_leaves_the_rect);
   /* zb2 third review: CANCEL / REVERT reach a tool with no texture. */
   g_test_add_func("/enhance_flow/cancel_without_texture_drops_the_line",
                   test_cancel_without_texture_drops_the_line);
   g_test_add_func("/enhance_flow/cancel_without_texture_lets_go_of_the_rect",
                   test_cancel_without_texture_lets_go_of_the_rect);
}

/* wb2 second review round: a discard ends the tool first, the crop tool
 * never swallows foreign keys, and drags obey the overlay's size guard. */
static void
add_tool_review2_tests(void) {
   g_test_add_func("/enhance_flow/discard_ends_the_straighten_tool",
                   test_discard_ends_the_straighten_tool);
   g_test_add_func("/enhance_flow/discard_ends_the_crop_tool_and_its_turn",
                   test_discard_ends_the_crop_tool_and_its_turn);
   g_test_add_func("/enhance_flow/crop_tool_passes_foreign_keys_base_unknown",
                   test_crop_tool_passes_foreign_keys_while_base_unknown);
   g_test_add_func("/enhance_flow/crop_drag_ignored_while_sizes_disagree",
                   test_crop_drag_ignored_while_sizes_disagree);
}

/* Registration split in two so neither function runs past the ~30-line
 * convention: the original feature coverage, and the round-2 subtests that
 * answer the real Save/Discard/Cancel prompt (see the file header). */
static void
add_feature_tests(void) {
   g_test_add_func("/enhance_flow/panel_opens_beside_viewer_with_thumbnails",
                   test_panel_opens_beside_viewer_with_thumbnails);
   g_test_add_func("/enhance_flow/panel_fits_eight_presets_at_1280x800",
                   test_panel_fits_eight_presets_at_1280x800);
   g_test_add_func("/enhance_flow/panel_label_only_mode_has_no_pictures",
                   test_panel_label_only_mode_has_no_pictures);
   g_test_add_func("/enhance_flow/panel_cards_track_mask_and_thumbnails_stay",
                   test_panel_cards_track_mask_and_thumbnails_stay);
   g_test_add_func("/enhance_flow/manual_save_clears_dirty_until_next_change",
                   test_manual_save_clears_dirty_until_next_change);
   g_test_add_func("/enhance_flow/esc_never_discards_the_edit",
                   test_esc_never_discards_the_edit);
   g_test_add_func("/enhance_flow/zero_zooms_and_x_reverts",
                   test_zero_zooms_and_x_reverts);
   g_test_add_func("/enhance_flow/panel_persists_across_navigation",
                   test_panel_persists_across_navigation);
   g_test_add_func("/enhance_flow/apply_is_async_and_original_untouched",
                   test_apply_is_async_and_original_untouched);
   g_test_add_func("/enhance_flow/toggle_off_resets_to_original",
                   test_toggle_off_resets_to_original);
   g_test_add_func("/enhance_flow/hold_space_compares_then_restores",
                   test_hold_space_compares_then_restores);
   g_test_add_func("/enhance_flow/hold_flag_not_stuck_after_mask_cleared",
                   test_hold_flag_not_stuck_after_mask_cleared);
   g_test_add_func("/enhance_flow/info_plots_preview", test_info_plots_preview);
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
   g_test_add_func("/enhance_flow/closing_the_prompt_keeps_the_preview",
                   test_closing_the_prompt_keeps_the_preview);
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
 * _enhance_dispose's "a request is still parked" branch used to be listed here
 * as NOT covered: driving it left the dialog's GTask unfinished and leaked the
 * _SaveCtx, so the subtest was dropped rather than shipped leaking. Task 2w0
 * cancels the prompt in dispose, and the subtest is back --
 * test_dispose_under_a_live_prompt_releases_it, registered by
 * add_dispose_prompt_tests below. */
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

/* Task 4w0: both directions of `d`'s cursor advance -- the same gap as
 * round 5's, left standing in the third member of the destructive trio. */
static void
add_trash_cursor_tests(void) {
   g_test_add_func("/enhance_flow/trash_advances_when_current_image_binned",
                   test_trash_advances_when_the_current_image_is_binned);
   g_test_add_func("/enhance_flow/trash_does_not_advance_past_unseen_image",
                   test_trash_does_not_advance_past_an_unseen_image);
}

/* Task 2w0: the branch round 4 had to drop -- dispose under a live prompt --
 * plus the close-request hole the review of that fix turned up. */
static void
add_dispose_prompt_tests(void) {
   g_test_add_func("/enhance_flow/dispose_under_a_live_prompt_releases_it",
                   test_dispose_under_a_live_prompt_releases_it);
   g_test_add_func("/enhance_flow/close_request_blocked_while_prompt_is_up",
                   test_close_request_blocked_while_prompt_is_up);
}

/* wb2 third review round: the tools act only on the exact render the
 * state produced (identity, not size), a horizon is never measured on a
 * stale or held-original picture, and every discard path ends a tool. */
static void
add_tool_review3_tests(void) {
   g_test_add_func("/enhance_flow/straighten_drag_refused_while_rendering",
                   test_straighten_drag_refused_while_rendering);
   g_test_add_func("/enhance_flow/straighten_drag_refused_while_holding",
                   test_straighten_drag_refused_while_holding_original);
   g_test_add_func("/enhance_flow/crop_tool_waits_for_the_exact_render",
                   test_crop_tool_waits_for_the_exact_render);
   g_test_add_func("/enhance_flow/crop_tool_waits_for_same_size_preset",
                   test_crop_tool_waits_for_a_same_size_preset_render);
   g_test_add_func("/enhance_flow/gate_discard_ends_the_straighten_tool",
                   test_gate_discard_ends_the_straighten_tool);
   g_test_add_func("/enhance_flow/slideshow_discard_ends_the_tool",
                   test_slideshow_discard_ends_the_tool);
   g_test_add_func("/enhance_flow/failed_render_ends_the_tool",
                   test_failed_render_ends_the_tool);
}

/* wb2 fourth review round: a refused drag BEGIN grabs nothing, the overlay
 * guard is a remembered identity (no cache lookup per frame), `c` over a
 * crop outside the view starts from the whole base, and a same-file
 * rewrite re-bases the crop tool. */
static void
add_tool_review4_tests(void) {
   g_test_add_func("/enhance_flow/refused_drag_begin_grabs_nothing",
                   test_refused_drag_begin_grabs_nothing);
   g_test_add_func("/enhance_flow/crop_overlay_survives_a_touch",
                   test_crop_overlay_survives_a_touch);
   g_test_add_func("/enhance_flow/crop_tool_over_outside_crop_starts_full",
                   test_crop_tool_over_an_outside_crop_starts_full);
   g_test_add_func("/enhance_flow/rewritten_file_rebases_the_crop_tool",
                   test_rewritten_file_rebases_the_crop_tool);
}

/* wb2 fifth review round: the original's identity is learned at the
 * window's texture choke point (a same-file reload refreshes it), a rewrite
 * under a preset re-renders, and a crop tool open through a rewrite is laid
 * out on the new base without any input. */
static void
add_tool_review5_tests(void) {
   g_test_add_func("/enhance_flow/reload_refreshes_the_original_identity",
                   test_reload_refreshes_the_original_identity);
   g_test_add_func("/enhance_flow/rewrite_under_a_preset_rerenders",
                   test_rewrite_under_a_preset_rerenders);
   g_test_add_func("/enhance_flow/crop_tool_relays_out_on_rewritten_base",
                   test_crop_tool_relays_out_on_the_rewritten_base);
}

/* --- wb2 seventh review round ------------------------------------------ */

/* The reload after a rewrite fails: the file is replaced with bytes no
 * decoder accepts (its size changes, so the rescan finds the cache entry
 * stale, forgets the original and the reload decodes the file afresh),
 * viewload clears the canvas and reports "Cannot show". Under the open
 * crop tool every key and Enter then answered "Preview still rendering --
 * try again in a moment" for as long as the user kept trying, though no
 * render was coming. Now they say there is no picture on screen and name
 * Esc as the way out, the tool stays until asked to leave, and Esc leaves
 * it. The wording is "yet ... wait for the image, or press Esc" rather
 * than "did not load": the controller cannot tell a failed reload from a
 * first decode still in flight, so the message has to hold for both. */
static void
test_failed_reload_under_the_crop_tool_says_so(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.crop");
   g_assert_true(
      g_file_set_contents(fx.c_path, "not an image at all", -1, NULL));
   wait_for_status_prefix(fx.p_win, "Cannot show");
   g_assert_null(viewer_texture(fx.p_win));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   tool_key(fx.p_win, GDK_KEY_h);
   g_assert_true(
      g_str_has_prefix(status_text(fx.p_win), "No picture on screen"));
   tool_key(fx.p_win, GDK_KEY_a);
   g_assert_true(
      g_str_has_prefix(status_text(fx.p_win), "No picture on screen"));
   tool_key(fx.p_win, GDK_KEY_Return);
   g_assert_true(
      g_str_has_prefix(status_text(fx.p_win), "No picture on screen"));
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_CROP);
   tool_key(fx.p_win, GDK_KEY_Escape);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_true(g_str_has_prefix(status_text(fx.p_win), "Crop cancelled"));
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   tool_fx_close(&fx);
}

/* wb2 sixth review round: an open runs the choke point a navigation does,
 * a render of another size re-bases the crop tool, an open's baseline. */
static void
add_tool_review6_tests(void) {
   g_test_add_func("/enhance_flow/open_ends_the_tool_and_the_saved_transform",
                   test_open_ends_the_tool_and_the_saved_transform);
   g_test_add_func("/enhance_flow/open_save_exports_then_shows_new_file_clean",
                   test_open_save_exports_then_shows_the_new_file_clean);
   g_test_add_func("/enhance_flow/crop_tool_follows_a_render_of_another_size",
                   test_crop_tool_follows_a_render_of_another_size);
   g_test_add_func(
      "/enhance_flow/same_second_same_size_rewrite_shows_the_new_picture",
      test_same_second_same_size_rewrite_shows_the_new_picture);
   g_test_add_func("/enhance_flow/first_rescan_after_an_open_is_same_file",
                   test_first_rescan_after_an_open_is_a_same_file_event);
}

/* --- xb2: colour management (decision #45) ------------------------------
 *
 * swapped.png stores pure red under a profile with sRGB's red and blue
 * primaries swapped. The enhance preview on screen must be BLUE (the chain
 * is colour-managed end to end: GEGL's ICC-aware loader tags the buffer,
 * babl converts for the texture), and the `s` export must carry the
 * source's profile byte for byte, and hold-Space must compare against the
 * managed original, blue too. Nothing here says what the plain view
 * shows: that depends on whether the host's gdk-pixbuf loaders apply
 * profiles (a glycin desktop does, fedora:40's native loaders do not) and
 * is, by decision #45, not ggaze's to manage. The CMYK and grey fixtures
 * (whose chain runs in sRGB) must compare against their managed original
 * too (xb2 review 2). */

/* Pixel (0, 0) of p_tex (i_w x i_h), read in an explicit R8G8B8A8 layout:
 * gdk_texture_download() would hand back GDK_MEMORY_DEFAULT, B8G8R8A8 on
 * little-endian hosts. Within 15 of (r, g, b). */
static void
assert_texture_pixel(GdkTexture *p_tex, gint i_w, gint i_h, int i_r, int i_g,
                     int i_b) {
   g_assert_cmpint(gdk_texture_get_width(p_tex), ==, i_w);
   g_assert_cmpint(gdk_texture_get_height(p_tex), ==, i_h);
   GdkTextureDownloader *p_dl = gdk_texture_downloader_new(p_tex);
   gdk_texture_downloader_set_format(p_dl, GDK_MEMORY_R8G8B8A8);
   gsize   u_stride   = 0;
   GBytes *p_bytes    = gdk_texture_downloader_download_bytes(p_dl, &u_stride);
   const guint8 *c_px = g_bytes_get_data(p_bytes, NULL);
   g_test_message("pixel: %u %u %u", c_px[0], c_px[1], c_px[2]);
   g_assert_cmpint(ABS((int)c_px[0] - i_r), <=, 15);
   g_assert_cmpint(ABS((int)c_px[1] - i_g), <=, 15);
   g_assert_cmpint(ABS((int)c_px[2] - i_b), <=, 15);
   g_bytes_unref(p_bytes);
   gdk_texture_downloader_free(p_dl);
}

/* What the managed original of a fixture looks like: its size and the
 * managed colour of pixel (0, 0). */
typedef struct {
   gint i_w, i_h;
   int  i_r, i_g, i_b;
} ManagedLook;

/* Hold-Space on a managed preview p_prev: at once the plain original
 * p_orig (the managed one is fetched lazily, in a worker, on this first
 * press), then the MANAGED original when it lands -- p_look's colour, and
 * the `i` card plots it (it stands for the current file's original) --
 * and the release brings p_prev back. A second press shows the fetched
 * one at once. Returns the managed original (a new ref). */
static GdkTexture *
assert_hold_shows_the_managed_original(GgazeWindow *p_win, GdkTexture *p_orig,
                                       GdkTexture        *p_prev,
                                       const ManagedLook *p_look) {
   ggaze_window_set_hold_original(p_win, TRUE);
   g_assert_true(viewer_texture(p_win) == p_orig);
   wait_for_texture_change(p_win, p_orig);
   GdkTexture *p_held = ref_viewer_texture(p_win);
   g_assert_true(p_held != p_orig);
   g_assert_true(p_held != p_prev);
   assert_texture_pixel(p_held, p_look->i_w, p_look->i_h, p_look->i_r,
                        p_look->i_g, p_look->i_b);
   fire(p_win, "win.info");
   wait_for_plot_of(p_win, p_held);
   fire(p_win, "win.info"); /* the card down again */
   ggaze_window_set_hold_original(p_win, FALSE);
   g_assert_true(viewer_texture(p_win) == p_prev);
   ggaze_window_set_hold_original(p_win, TRUE);
   g_assert_true(viewer_texture(p_win) == p_held);
   ggaze_window_set_hold_original(p_win, FALSE);
   return (p_held);
}

/* c_out embeds p_src's ICC profile byte for byte. */
static void
assert_same_profile(GFile *p_src, const char *c_out) {
   GError *p_err  = NULL;
   GFile  *p_out  = g_file_new_for_path(c_out);
   GBytes *p_want = icc_read_embedded(p_src, &p_err);
   g_assert_no_error(p_err);
   GBytes *p_got = icc_read_embedded(p_out, &p_err);
   g_assert_no_error(p_err);
   g_assert_nonnull(p_got);
   g_assert_true(g_bytes_equal(p_want, p_got));
   g_bytes_unref(p_want);
   g_bytes_unref(p_got);
   g_object_unref(p_out);
}

/* A window over c_dir/c_name (copied from the fixtures), loaded at
 * i_w x i_h; *pp_file receives the file (a new ref). */
static GgazeWindow *
open_fixture_copy(const char *c_dir, const char *c_name, gint i_w, gint i_h,
                  GFile **pp_file) {
   copy_fixture(c_dir, c_name);
   char *c_path = g_build_filename(c_dir, c_name, NULL);
   *pp_file     = g_file_new_for_path(c_path);
   g_free(c_path);
   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, *pp_file);
   wait_for_load(p_win, i_w, i_h);
   return (p_win);
}

static void
close_window(GgazeWindow *p_win) {
   gtk_window_destroy(GTK_WINDOW(p_win));
   ggtest_drain_main(300);
}

static void
test_icc_preview_is_managed_and_export_keeps_profile(void) {
   static const ManagedLook C_BLUE = {6, 3, 0, 0, 255};
   GError                  *p_err  = NULL;
   char                    *c_dir  = g_dir_make_tmp("ggaze-icc-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   GFile       *p_file = NULL;
   GgazeWindow *p_win  = open_fixture_copy(c_dir, "swapped.png", 6, 3, &p_file);

   GdkTexture *p_orig = ref_viewer_texture(p_win);
   fire(p_win, "win.enhance-3"); /* Contrast: keeps a pure colour pure */
   wait_for_texture_change(p_win, p_orig);
   GdkTexture *p_prev = ref_viewer_texture(p_win);
   g_assert_true(p_prev != p_orig);
   assert_texture_pixel(p_prev, 6, 3, 0, 0, 255);
   g_object_unref(
      assert_hold_shows_the_managed_original(p_win, p_orig, p_prev, &C_BLUE));
   g_object_unref(p_prev);

   char *c_out = g_build_filename(c_dir, "swapped-enhanced.png", NULL);
   fire(p_win, "win.enhance-save");
   wait_for_file(c_out);
   ggtest_drain_main(100);
   assert_same_profile(p_file, c_out);
   g_free(c_out);
   g_object_unref(p_orig);
   g_object_unref(p_file);
   close_window(p_win);
   ggtest_cleanup_temp_dir(c_dir);
}

/* The CMYK and grey fixtures: their chain runs in sRGB, yet the render
 * was managed, so hold-Space compares against the MANAGED original (blue
 * for the CMYK JPEG's printer profile, ~188 for the linear grey PNG), not
 * the plain decode. A discard drops the managed original (memory): the
 * next preview's first press shows the plain original again and fetches
 * it anew. The CMYK JPEG is managed only with libjpeg (GGAZE_HAVE_JPEG). */
static void
check_hold_on(const char *c_name, const ManagedLook *p_look) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-icc-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   GFile       *p_file = NULL;
   GgazeWindow *p_win =
      open_fixture_copy(c_dir, c_name, p_look->i_w, p_look->i_h, &p_file);
   GdkTexture *p_orig = ref_viewer_texture(p_win);
   fire(p_win, "win.enhance-3");
   wait_for_texture_change(p_win, p_orig);
   GdkTexture *p_prev = ref_viewer_texture(p_win);
   GdkTexture *p_held =
      assert_hold_shows_the_managed_original(p_win, p_orig, p_prev, p_look);
   g_object_unref(p_prev);

   revert_edits(p_win); /* x: discard (Esc keeps it since 6i2) */
   ggtest_drain_main(100);
   GdkTexture *p_orig2 = ref_viewer_texture(p_win);
   fire(p_win, "win.enhance-3");
   wait_for_texture_change(p_win, p_orig2);
   GdkTexture *p_prev2 = ref_viewer_texture(p_win);
   ggaze_window_set_hold_original(p_win, TRUE);
   g_assert_true(viewer_texture(p_win) != p_held); /* dropped, not reused */
   wait_for_texture_change(p_win, p_orig2);
   assert_texture_pixel(viewer_texture(p_win), p_look->i_w, p_look->i_h,
                        p_look->i_r, p_look->i_g, p_look->i_b);
   ggaze_window_set_hold_original(p_win, FALSE);
   g_object_unref(p_prev2);
   g_object_unref(p_orig2);
   g_object_unref(p_held);
   g_object_unref(p_orig);
   g_object_unref(p_file);
   close_window(p_win);
   ggtest_cleanup_temp_dir(c_dir);
}

static void
test_icc_hold_space_on_cmyk_and_grey(void) {
   static const ManagedLook C_GREY = {4, 2, 188, 188, 188};
   static const ManagedLook C_CMYK = {8, 8, 0, 0, 255};
   check_hold_on("grey-icc.png", &C_GREY);
#if GGAZE_HAVE_JPEG
   check_hold_on("cmyk-icc.jpg", &C_CMYK);
#else
   (void)C_CMYK;
#endif
}

/* --- xb2 review 3: the lazy fetch's edges --------------------------------
 *
 * The managed original is fetched on the first Space press, in a worker;
 * whatever happens before it lands must not put it (or a stale one) on
 * screen. Each subtest starts from swapped.png (6x3) under a Contrast
 * preview; its managed original is BLUE. The press / release /
 * navigation / discard below all run before any main-loop iteration, so
 * the fetch is in flight when they happen, deterministically. */

typedef struct {
   char        *c_dir;
   GFile       *p_file;
   GgazeWindow *p_win;
   GdkTexture  *p_orig; /* the plain decode (ref) */
   GdkTexture  *p_prev; /* the Contrast render (ref) */
} IccFx;

static void
icc_fx_open(IccFx *p_fx, const char *c_extra) {
   GError *p_err = NULL;
   p_fx->c_dir   = g_dir_make_tmp("ggaze-iccfetch-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   if (c_extra != NULL) {
      copy_fixture(p_fx->c_dir, c_extra);
   }
   p_fx->p_win =
      open_fixture_copy(p_fx->c_dir, "swapped.png", 6, 3, &p_fx->p_file);
   p_fx->p_orig = ref_viewer_texture(p_fx->p_win);
   fire(p_fx->p_win, "win.enhance-3");
   wait_for_texture_change(p_fx->p_win, p_fx->p_orig);
   p_fx->p_prev = ref_viewer_texture(p_fx->p_win);
   g_assert_cmpuint(ggaze_window_enhance_managed_fetch_count(p_fx->p_win), ==,
                    0); /* lazy: no press, no fetch */
}

static void
icc_fx_close(IccFx *p_fx) {
   g_clear_object(&p_fx->p_orig);
   g_clear_object(&p_fx->p_prev);
   g_clear_object(&p_fx->p_file);
   close_window(p_fx->p_win);
   ggtest_cleanup_temp_dir(p_fx->c_dir);
}

/* Pump for u_ms: long enough for a fetch of a fixture to land. */
static void
pump_ms(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(NULL, FALSE);
      g_usleep(1000);
   }
}

/* Pump until the controller holds a landed managed original (5 s max). */
static void
wait_for_managed(GgazeWindow *p_win) {
   for (guint u = 0;
        u < 5000 && !ggaze_window_enhance_has_managed_original(p_win); u++) {
      g_main_context_iteration(NULL, FALSE);
      g_usleep(1000);
   }
   g_assert_true(ggaze_window_enhance_has_managed_original(p_win));
}

/* Space released before the fetch lands: the release brings the preview
 * back, the landing keeps the managed original WITHOUT putting it up, and
 * the next press shows it at once -- no second fetch. */
static void
test_icc_release_before_the_fetch_lands(void) {
   static const ManagedLook C_BLUE = {6, 3, 0, 0, 255};
   IccFx                    fx;
   icc_fx_open(&fx, NULL);
   ggaze_window_set_hold_original(fx.p_win, TRUE);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   ggaze_window_set_hold_original(fx.p_win, FALSE);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_prev);
   wait_for_managed(fx.p_win);
   pump_ms(50);
   g_assert_true(viewer_texture(fx.p_win) == fx.p_prev); /* not put up */
   ggaze_window_set_hold_original(fx.p_win, TRUE);
   GdkTexture *p_held = viewer_texture(fx.p_win);
   g_assert_true(p_held != fx.p_orig && p_held != fx.p_prev);
   assert_texture_pixel(p_held, C_BLUE.i_w, C_BLUE.i_h, C_BLUE.i_r, C_BLUE.i_g,
                        C_BLUE.i_b);
   ggaze_window_set_hold_original(fx.p_win, FALSE);
   g_assert_cmpuint(ggaze_window_enhance_managed_fetch_count(fx.p_win), ==, 1);
   icc_fx_close(&fx);
}

/* Navigation while the fetch is in flight (Space still held): the fetch
 * is dropped, its landing puts nothing up and keeps nothing, and the next
 * file is on screen. The preview is saved first so the move is not gated;
 * the folder is then swapped.png, swapped-enhanced.png, small.png. */
static void
test_icc_navigation_mid_fetch(void) {
   IccFx fx;
   icc_fx_open(&fx, "small.png");
   char *c_out = g_build_filename(fx.c_dir, "swapped-enhanced.png", NULL);
   fire(fx.p_win, "win.enhance-save");
   wait_for_file(c_out);
   ggtest_drain_main(700); /* the copy's rescan, past the debounce */
   g_free(c_out);
   ggaze_window_set_hold_original(fx.p_win, TRUE);
   g_assert_cmpuint(ggaze_window_enhance_managed_fetch_count(fx.p_win), ==, 1);
   fire(fx.p_win, "win.next");
   g_assert_nonnull(
      g_strstr_len(window_title(fx.p_win), -1, "swapped-enhanced.png"));
   wait_for_texture_change(fx.p_win, fx.p_prev);
   pump_ms(300); /* the dropped fetch lands, stale */
   g_assert_false(ggaze_window_enhance_has_managed_original(fx.p_win));
   GdkTexture *p_now = viewer_texture(fx.p_win);
   g_assert_true(p_now != fx.p_orig && p_now != fx.p_prev);
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "Contrast"));
   ggaze_window_set_hold_original(fx.p_win, FALSE);
   icc_fx_close(&fx);
}

/* Discard while the fetch is in flight: the plain original is back, the
 * landing keeps nothing, and the next preview's first press fetches
 * again (a second launch). */
static void
test_icc_discard_mid_fetch(void) {
   IccFx fx;
   icc_fx_open(&fx, NULL);
   ggaze_window_set_hold_original(fx.p_win, TRUE);
   ggaze_window_set_hold_original(fx.p_win, FALSE);
   revert_edits(fx.p_win); /* x: discard (Esc keeps it since 6i2) */
   pump_ms(300);
   g_assert_false(ggaze_window_enhance_has_managed_original(fx.p_win));
   GdkTexture *p_plain = ref_viewer_texture(fx.p_win);
   g_assert_true(p_plain != fx.p_prev);
   fire(fx.p_win, "win.enhance-3");
   wait_for_texture_change(fx.p_win, p_plain);
   ggaze_window_set_hold_original(fx.p_win, TRUE);
   g_assert_cmpuint(ggaze_window_enhance_managed_fetch_count(fx.p_win), ==, 2);
   wait_for_managed(fx.p_win);
   ggaze_window_set_hold_original(fx.p_win, FALSE);
   g_object_unref(p_plain);
   icc_fx_close(&fx);
}

/* swapped.png rewritten in place as grey-icc.png's bytes (4x2, a linear
 * grey profile: managed ~188), by rename like an editor's save. */
static void
rewrite_as_grey_icc(IccFx *p_fx) {
   const gchar *c_fx   = g_getenv("GGAZE_FIXTURES_DIR");
   char        *c_src  = g_build_filename(c_fx, "grey-icc.png", NULL);
   char        *c_data = NULL;
   gsize        u_len  = 0;
   g_assert_true(g_file_get_contents(c_src, &c_data, &u_len, NULL));
   char *c_path = g_file_get_path(p_fx->p_file);
   g_assert_true(g_file_set_contents(c_path, c_data, (gssize)u_len, NULL));
   g_free(c_path);
   g_free(c_data);
   g_free(c_src);
}

/* After a rewrite the managed original of the OLD contents is gone -- it
 * was fetched (b_landed) or in flight -- and the next press, on the
 * re-render of the new contents, fetches the new file's: 4x2, ~188. */
static void
check_rewrite_drops_the_managed_original(gboolean b_landed) {
   static const ManagedLook C_GREY = {4, 2, 188, 188, 188};
   IccFx                    fx;
   icc_fx_open(&fx, NULL);
   ggaze_window_set_hold_original(fx.p_win, TRUE);
   ggaze_window_set_hold_original(fx.p_win, FALSE);
   if (b_landed) {
      wait_for_managed(fx.p_win);
   }
   rewrite_as_grey_icc(&fx);
   wait_for_texture_size(fx.p_win, 4, 2); /* the re-render, new contents */
   pump_ms(300);
   g_assert_false(ggaze_window_enhance_has_managed_original(fx.p_win));
   GdkTexture *p_rend = ref_viewer_texture(fx.p_win);
   ggaze_window_set_hold_original(fx.p_win, TRUE);
   g_assert_cmpuint(ggaze_window_enhance_managed_fetch_count(fx.p_win), ==, 2);
   wait_for_managed(fx.p_win);
   pump_ms(50);
   assert_texture_pixel(viewer_texture(fx.p_win), C_GREY.i_w, C_GREY.i_h,
                        C_GREY.i_r, C_GREY.i_g, C_GREY.i_b);
   ggaze_window_set_hold_original(fx.p_win, FALSE);
   g_assert_true(viewer_texture(fx.p_win) == p_rend);
   g_object_unref(p_rend);
   icc_fx_close(&fx);
}

static void
test_icc_rewrite_mid_fetch(void) {
   check_rewrite_drops_the_managed_original(FALSE);
}

static void
test_icc_rewrite_drops_the_managed_original(void) {
   check_rewrite_drops_the_managed_original(TRUE);
}

static void
add_icc_tests(void) {
   g_test_add_func("/enhance_flow/icc_preview_is_managed_and_export_keeps_"
                   "profile",
                   test_icc_preview_is_managed_and_export_keeps_profile);
   g_test_add_func("/enhance_flow/icc_hold_space_on_cmyk_and_grey",
                   test_icc_hold_space_on_cmyk_and_grey);
   g_test_add_func("/enhance_flow/icc_release_before_the_fetch_lands",
                   test_icc_release_before_the_fetch_lands);
   g_test_add_func("/enhance_flow/icc_navigation_mid_fetch",
                   test_icc_navigation_mid_fetch);
   g_test_add_func("/enhance_flow/icc_discard_mid_fetch",
                   test_icc_discard_mid_fetch);
   g_test_add_func("/enhance_flow/icc_rewrite_mid_fetch",
                   test_icc_rewrite_mid_fetch);
   g_test_add_func("/enhance_flow/icc_rewrite_drops_the_managed_original",
                   test_icc_rewrite_drops_the_managed_original);
}

/* Seventh review round: a failed reload under the crop tool is named. */
static void
add_tool_review7_tests(void) {
   g_test_add_func("/enhance_flow/failed_reload_under_the_crop_tool_says_so",
                   test_failed_reload_under_the_crop_tool_says_so);
}

/* --- gd2: a multi-file open is one pass ----------------------------------
 *
 * With the panel up, an open re-points it at the new file and starts one
 * thumbnail batch for it (enhance_ctrl_nav_changed -> _retarget_panel).
 * ggaze_window_open_files used to open the first file's FOLDER and place
 * the cursor afterwards: for a start file not sorted first that emitted
 * "changed" and ran the whole sequence again -- two loads and two batches,
 * the first of each cancelled. Now the start file arrives with the folder
 * (window.c _open_now), so the load and the batch happen once. rot6.jpg
 * sorts after plain.jpg in the second folder: the exact case that doubled. */
static void
test_open_many_re_points_the_panel_once(void) {
   Settings *p_cfg = settings_new();
   settings_set_enhance_preview_thumbnails(p_cfg, TRUE);
   DirtyFixture             fx      = {0};
   static const char *const c_two[] = {"plain.jpg", "rot6.jpg", NULL};
   fixture_open_clean(&fx, "ggaze-open-many-a-XXXXXX", c_two);
   fire(fx.p_win, "win.enhance");
   g_assert_nonnull(
      find_label_prefix(find_panel(fx.p_win), "as plain-enhanced"));
   g_assert_cmpuint(ggaze_window_enhance_preview_count(fx.p_win), ==, 1);
   guint u_loads = ggaze_window_load_count(fx.p_win);

   GError *p_err   = NULL;
   char   *c_other = g_dir_make_tmp("ggaze-open-many-b-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_other, "plain.jpg");
   copy_fixture(c_other, "rot6.jpg");
   GFile *pp_files[2];
   pp_files[0] = g_file_new_build_filename(c_other, "rot6.jpg", NULL);
   pp_files[1] = g_file_new_build_filename(c_other, "plain.jpg", NULL);
   ggaze_window_open_files(fx.p_win, pp_files, 2); /* clean: no prompt */
   /* Synchronous through the gate (nothing dirty): final before any
    * main-loop iteration could deliver a second "changed". */
   g_assert_cmpuint(ggaze_window_load_count(fx.p_win), ==, u_loads + 1);
   g_assert_cmpuint(ggaze_window_enhance_preview_count(fx.p_win), ==, 2);
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win), ==, 0);
   g_assert_cmpstr(
      gtk_stack_get_visible_child_name(ggaze_window_get_stack(fx.p_win)), ==,
      "grid");
   g_assert_nonnull(
      find_label_prefix(find_panel(fx.p_win), "as rot6-enhanced"));
   wait_for_texture_size(fx.p_win, 4, 8); /* the start file's own decode */
   g_assert_cmpuint(ggaze_window_load_count(fx.p_win), ==, u_loads + 1);
   g_assert_cmpuint(ggaze_window_enhance_preview_count(fx.p_win), ==, 2);

   g_object_unref(pp_files[0]);
   g_object_unref(pp_files[1]);
   g_settings_reset(settings_get_gsettings(p_cfg),
                    "enhance-preview-thumbnails");
   settings_delete(p_cfg);
   fixture_teardown(&fx);
   ggtest_cleanup_temp_dir(c_other);
}

static void
add_open_many_tests(void) {
   g_test_add_func("/enhance_flow/open_many_re_points_the_panel_once",
                   test_open_many_re_points_the_panel_once);
}

/* --- 8i2: the selected card and preset strengths ---------------------------
 *
 * With the edit panel open, j / k move a selection between the preset
 * cards, Enter toggles the selected one, and h / l (Shift: five steps)
 * lower / raise its strength, turning it on -- the vi keys alone: the
 * arrows keep changing the image and panning; a tunable
 * card shows its strength, and the selected one's slider is the mouse
 * path to the same number. Keys go through the window's edit-key router
 * (ggaze_window_edit_key), the path a real key press takes. */

/* The first label with c_class under p_root, or NULL. */
static GtkWidget *
find_class_label(GtkWidget *p_root, const char *c_class) {
   if (GTK_IS_LABEL(p_root) && gtk_widget_has_css_class(p_root, c_class)) {
      return (p_root);
   }
   GtkWidget *p_child = gtk_widget_get_first_child(p_root);
   while (p_child != NULL) {
      GtkWidget *p_found = find_class_label(p_child, c_class);
      if (p_found != NULL) {
         return (p_found);
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (NULL);
}

/* Card i_idx of the open panel (asserted present). */
static GtkWidget *
panel_card(GgazeWindow *p_win, gint i_idx) {
   GtkWidget *p_card = find_card(find_panel(p_win), i_idx);
   g_assert_nonnull(p_card);
   return (p_card);
}

/* The strength card i_idx shows ("+0.6"), or NULL for a card without. */
static const char *
card_value(GgazeWindow *p_win, gint i_idx) {
   GtkWidget *p_lbl =
      find_class_label(panel_card(p_win, i_idx), GGAZE_ENHANCE_VALUE_CLASS);
   return (p_lbl != NULL ? gtk_label_get_text(GTK_LABEL(p_lbl)) : NULL);
}

/* Exactly card i_idx wears the selection ring, and only a selected tunable
 * card shows its slider. */
static void
assert_selected(GgazeWindow *p_win, gint i_idx) {
   for (gint i = 0; i < 8; i++) {
      GtkWidget *p_card = panel_card(p_win, i);
      g_assert_cmpint(
         gtk_widget_has_css_class(p_card, GGAZE_ENHANCE_SELECTED_CLASS), ==,
         i == i_idx);
      GtkWidget *p_scale = find_scale(find_panel(p_win), i);
      if (p_scale != NULL) {
         g_assert_cmpint(gtk_widget_get_visible(p_scale), ==, i == i_idx);
      }
   }
}

/* Pump until the window title contains c_part (up to 10 s), then assert
 * it does. A strength change re-renders, and the title follows the render
 * that lands. */
static void
wait_for_title(GgazeWindow *p_win, const char *c_part) {
   for (guint u = 0;
        u < 10000 && g_strstr_len(window_title(p_win), -1, c_part) == NULL;
        u++) {
      g_main_context_iteration(NULL, FALSE);
      g_usleep(1000);
   }
   if (g_strstr_len(window_title(p_win), -1, c_part) == NULL) {
      g_error("title \"%s\", wanted \"%s\"", window_title(p_win), c_part);
   }
}

/* Pump until the window title no longer contains c_part (up to 10 s),
 * then assert so. */
static void
wait_for_title_without(GgazeWindow *p_win, const char *c_part) {
   for (guint u = 0;
        u < 10000 && g_strstr_len(window_title(p_win), -1, c_part) != NULL;
        u++) {
      g_main_context_iteration(NULL, FALSE);
      g_usleep(1000);
   }
   if (g_strstr_len(window_title(p_win), -1, c_part) != NULL) {
      g_error("title \"%s\" still has \"%s\"", window_title(p_win), c_part);
   }
}

/* j / k move the selection (stopping at the ends, Caps Lock no matter), a
 * digit selects its own row, and Enter toggles the selected preset
 * exactly as its digit does. The hint bar lists the new keys. */
static void
test_strength_select_and_enter(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   char *c_hint = plain_hint(fx.p_win);
   g_assert_nonnull(
      g_strstr_len(c_hint, -1, "j/k select  ·  Enter toggle  ·  h/l strength"));
   g_free(c_hint);
   assert_selected(fx.p_win, 0);
   edit_key(fx.p_win, GDK_KEY_k, 0); /* stops at the top */
   assert_selected(fx.p_win, 0);
   edit_key(fx.p_win, GDK_KEY_j, 0);
   assert_selected(fx.p_win, 1);
   edit_key(fx.p_win, GDK_KEY_j, 0);
   assert_selected(fx.p_win, 2);
   edit_key(fx.p_win, GDK_KEY_J, GDK_LOCK_MASK); /* Caps Lock: still j */
   assert_selected(fx.p_win, 3);
   edit_key(fx.p_win, GDK_KEY_k, 0);
   assert_selected(fx.p_win, 2);
   for (guint u = 0; u < 9; u++) {
      edit_key(fx.p_win, GDK_KEY_j, 0);
   }
   assert_selected(fx.p_win, 7); /* stops at the bottom */
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win)); /* no edit */
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win), ==, 0);

   edit_key_and_wait(fx.p_win, GDK_KEY_Return, 0);
   g_assert_true(
      gtk_widget_has_css_class(panel_card(fx.p_win, 7), "ggaze-enhance-on"));
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key_and_wait(fx.p_win, GDK_KEY_KP_Enter, 0); /* off again */
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key_and_wait(fx.p_win, GDK_KEY_3, 0); /* the digit selects too */
   assert_selected(fx.p_win, 2);
   edit_key_and_wait(fx.p_win, GDK_KEY_u, 0);
   assert_status_prefix(fx.p_win, "Undid: Contrast on");
   assert_selected(fx.p_win, 2); /* the selection is no edit */
   tool_fx_close(&fx);
}

/* h / l step the selected preset's strength and turn it on; the card, the
 * slider and the title show the value (a default is not named in the
 * title); Shift steps five, and the range ends with a status line. */
static void
test_strength_keys_turn_on_and_name_it(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   g_assert_cmpstr(card_value(fx.p_win, 1), ==, "+0.5");
   g_assert_null(card_value(fx.p_win, 0)); /* Auto-fix: nothing to tune */
   edit_key(fx.p_win, GDK_KEY_j, 0);       /* Brightness */
   edit_key_and_wait(fx.p_win, GDK_KEY_l, 0);
   g_assert_true(
      gtk_widget_has_css_class(panel_card(fx.p_win, 1), "ggaze-enhance-on"));
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   wait_for_title(fx.p_win, "Brightness +0.6");
   assert_status_prefix(fx.p_win, "Brightness +0.6");
   g_assert_cmpstr(card_value(fx.p_win, 1), ==, "+0.6");
   GtkWidget *p_scale = find_scale(find_panel(fx.p_win), 1);
   g_assert_cmpfloat(gtk_range_get_value(GTK_RANGE(p_scale)), ==, 0.6);
   edit_key(fx.p_win, GDK_KEY_h, 0);
   edit_key(fx.p_win, GDK_KEY_h, 0);
   edit_key(fx.p_win, GDK_KEY_H, GDK_LOCK_MASK); /* Caps Lock: one step */
   wait_for_title(fx.p_win, "Brightness +0.3");
   edit_key(fx.p_win, GDK_KEY_L, GDK_SHIFT_MASK);
   wait_for_title(fx.p_win, "Brightness +0.8");
   edit_key(fx.p_win, GDK_KEY_l, GDK_SHIFT_MASK); /* Shift by the modifier */
   edit_key(fx.p_win, GDK_KEY_l, 0);
   wait_for_title(fx.p_win, "Brightness +1.4");
   edit_key(fx.p_win, GDK_KEY_h, 0);
   edit_key(fx.p_win, GDK_KEY_h, 0);
   edit_key(fx.p_win, GDK_KEY_h, 0);
   edit_key(fx.p_win, GDK_KEY_h, 0);
   edit_key(fx.p_win, GDK_KEY_h, 0);
   edit_key(fx.p_win, GDK_KEY_h, 0);
   edit_key(fx.p_win, GDK_KEY_h, 0);
   edit_key(fx.p_win, GDK_KEY_h, 0);
   edit_key(fx.p_win, GDK_KEY_h, 0); /* back at the default, 0.5 */
   wait_for_title_without(fx.p_win, "Brightness +");
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "Brightness"));
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win)); /* still on */
   for (guint u = 0; u < 6; u++) {
      edit_key(fx.p_win, GDK_KEY_L, GDK_SHIFT_MASK);
   }
   assert_status_prefix(fx.p_win, "Brightness is at its maximum (+2)");
   wait_for_title(fx.p_win, "Brightness +2");
   tool_fx_close(&fx);
}

/* A preset without a tunable number refuses h / l with a status line: no
 * render, no edit, no undo step. */
static void
test_strength_refused_without_placeholder(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   guint u_renders = ggaze_window_enhance_render_count(fx.p_win);
   edit_key(fx.p_win, GDK_KEY_l, 0); /* Auto-fix, selected */
   assert_status_prefix(fx.p_win, "Auto-fix has no strength to adjust");
   /* ... and says how to change image instead, which l no longer does. */
   g_assert_nonnull(g_strstr_len(status_text(fx.p_win), -1,
                                 "\u2190/\u2192 or PgUp/PgDn change image"));
   edit_key(fx.p_win, GDK_KEY_H, GDK_SHIFT_MASK);
   ggtest_drain_main(200);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win), ==, u_renders);
   assert_history_buttons(fx.p_win, FALSE, FALSE);
   for (guint u = 0; u < 4; u++) {
      edit_key(fx.p_win, GDK_KEY_j, 0); /* 5 Warm */
   }
   edit_key(fx.p_win, GDK_KEY_h, 0);
   assert_status_prefix(fx.p_win, "Warm has no strength to adjust");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   tool_fx_close(&fx);
}

/* A run of h / l on one card is ONE undo step (back to the preset off at
 * its default), redone as one; another card, or a key of another kind,
 * starts a new step. A burst of presses costs at most two renders
 * (coalesced, the last value lands). */
static void
test_strength_run_undoes_as_one_step(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   edit_key(fx.p_win, GDK_KEY_j, 0);
   guint u_before = ggaze_window_enhance_render_count(fx.p_win);
   for (guint u = 0; u < 5; u++) {
      edit_key(fx.p_win, GDK_KEY_l, 0);
   }
   wait_for_title(fx.p_win, "Brightness +1");
   ggtest_drain_main(300); /* nothing else lands after the last value */
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win) - u_before, <=,
                    2);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "+1"));
   edit_key_and_wait(fx.p_win, GDK_KEY_u, 0);
   assert_status_prefix(fx.p_win, "Undid: Brightness +1");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_true(viewer_texture(fx.p_win) == fx.p_orig);
   g_assert_cmpstr(card_value(fx.p_win, 1), ==, "+0.5");
   assert_history_buttons(fx.p_win, FALSE, TRUE);
   edit_key_and_wait(fx.p_win, GDK_KEY_U, GDK_SHIFT_MASK);
   wait_for_title(fx.p_win, "Brightness +1");
   /* Another card closes the run: its h / l is a step of its own ... */
   edit_key(fx.p_win, GDK_KEY_j, 0);
   edit_key(fx.p_win, GDK_KEY_j, 0); /* Saturation */
   edit_key_and_wait(fx.p_win, GDK_KEY_l, 0);
   wait_for_title(fx.p_win, "Saturation 1.5");
   /* ... and so does coming back to the first card. */
   edit_key(fx.p_win, GDK_KEY_k, 0);
   edit_key(fx.p_win, GDK_KEY_k, 0);
   edit_key_and_wait(fx.p_win, GDK_KEY_h, 0);
   wait_for_title(fx.p_win, "Brightness +0.9");
   edit_key_and_wait(fx.p_win, GDK_KEY_u, 0);
   wait_for_title(fx.p_win, "Brightness +1, Saturation 1.5");
   edit_key_and_wait(fx.p_win, GDK_KEY_u, 0);
   assert_status_prefix(fx.p_win, "Undid: Saturation 1.5");
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "Saturation"));
   tool_fx_close(&fx);
}

/* Set the slider of card i_idx to d_value the way a drag moves it. */
static void
drag_scale_to(GgazeWindow *p_win, gint i_idx, gdouble d_value) {
   GtkWidget *p_scale = find_scale(find_panel(p_win), i_idx);
   g_assert_nonnull(p_scale);
   gtk_range_set_value(GTK_RANGE(p_scale), d_value);
}

/* The slider: dragging it sets the strength (snapped onto the preset's
 * step grid), selects its row and turns the preset on; one drag is one
 * undo step; it takes no focus (Space compares, h / l are its keys). */
static void
test_strength_slider_drag(void) {
   ToolFx fx;
   tool_fx_open(&fx, TRUE);
   fire(fx.p_win, "win.enhance");
   GtkWidget *p_scale = find_scale(find_panel(fx.p_win), 3); /* Saturation */
   g_assert_nonnull(p_scale);
   g_assert_false(gtk_widget_get_focusable(p_scale));
   g_assert_false(gtk_widget_get_can_focus(p_scale));
   g_assert_null(find_scale(find_panel(fx.p_win), 0)); /* Auto-fix: none */
   GdkTexture *p_before = ref_viewer_texture(fx.p_win);
   drag_scale_to(fx.p_win, 3, 1.52);
   drag_scale_to(fx.p_win, 3, 1.74);
   drag_scale_to(fx.p_win, 3, 1.66); /* snaps to 1.7 */
   wait_for_texture_change(fx.p_win, p_before);
   g_clear_object(&p_before);
   wait_for_title(fx.p_win, "Saturation 1.7");
   assert_selected(fx.p_win, 3);
   g_assert_cmpfloat(gtk_range_get_value(GTK_RANGE(p_scale)), ==, 1.7);
   g_assert_cmpstr(card_value(fx.p_win, 3), ==, "1.7");
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key_and_wait(fx.p_win, GDK_KEY_u, 0); /* the drag, as one step */
   assert_status_prefix(fx.p_win, "Undid: Saturation 1.7");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_cmpfloat(gtk_range_get_value(GTK_RANGE(p_scale)), ==, 1.4);
   tool_fx_close(&fx);
}

/* The strengths are part of what `s` writes and what saved means: the
 * export renders them (a copy at +0.6 differs from one at the default),
 * a strength moved off the saved value is dirty, and moved back is saved
 * again. */
static void
test_strength_saved_and_dirty(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   edit_key_and_wait(fx.p_win, GDK_KEY_2, 0); /* Brightness at 0.5 */
   fire(fx.p_win, "win.enhance-save");
   char *c_def = g_build_filename(fx.c_dir, "tool-enhanced.png", NULL);
   wait_for_file(c_def);
   wait_for_status_prefix(fx.p_win, "Saved ");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key_and_wait(fx.p_win, GDK_KEY_l, 0); /* row 2 is selected */
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key_and_wait(fx.p_win, GDK_KEY_h, 0);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win)); /* saved again */
   g_assert_nonnull(find_label_prefix(find_panel(fx.p_win), "Saved as "));
   edit_key_and_wait(fx.p_win, GDK_KEY_l, 0);
   fire(fx.p_win, "win.enhance-save");
   char *c_up = g_build_filename(fx.c_dir, "tool-enhanced-1.png", NULL);
   wait_for_file(c_up);
   wait_for_status_prefix(fx.p_win, "Saved ");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   gsize u_def = 0;
   gsize u_up  = 0;
   char *c_a   = load_bytes(c_def, &u_def);
   char *c_b   = load_bytes(c_up, &u_up);
   g_assert_false(u_def == u_up && memcmp(c_a, c_b, u_def) == 0);
   g_free(c_a);
   g_free(c_b);
   g_free(c_def);
   g_free(c_up);
   tool_fx_close(&fx);
}

/* Strengths are per image, like the mask: x puts every preset back at its
 * default (undoably), and so does moving to another file. */
static void
test_strength_resets_on_revert_and_navigation(void) {
   ToolFx fx;
   tool_fx_open_with_sibling(&fx);
   fire(fx.p_win, "win.enhance");
   edit_key(fx.p_win, GDK_KEY_j, 0);
   edit_key_and_wait(fx.p_win, GDK_KEY_l, 0);
   wait_for_title(fx.p_win, "Brightness +0.6");
   fire(fx.p_win, "win.edit-revert");
   ggtest_drain_main(200);
   g_assert_cmpstr(card_value(fx.p_win, 1), ==, "+0.5");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key_and_wait(fx.p_win, GDK_KEY_u, 0); /* x is a step */
   wait_for_title(fx.p_win, "Brightness +0.6");
   g_assert_cmpstr(card_value(fx.p_win, 1), ==, "+0.6");
   fire(fx.p_win, "win.enhance-save"); /* clean, so no prompt below */
   char *c_out = g_build_filename(fx.c_dir, "tool-enhanced.png", NULL);
   wait_for_file(c_out);
   wait_for_status_prefix(fx.p_win, "Saved ");
   g_free(c_out);
   fire(fx.p_win, "win.prev");
   wait_for_load(fx.p_win, PLAIN_JPG_W, PLAIN_JPG_H);
   assert_showing(fx.p_win, "a.jpg");
   g_assert_cmpstr(card_value(fx.p_win, 1), ==, "+0.5");
   assert_selected(fx.p_win, 1); /* the selection is no edit: it stays */
   edit_key_and_wait(fx.p_win, GDK_KEY_2, 0);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "Brightness"));
   g_assert_null(g_strstr_len(window_title(fx.p_win), -1, "+0."));
   tool_fx_close(&fx);
}

/* 8i2 review: a Preferences change -- ANY key: the window reloads the
 * engine lists on every one -- keeps a tuned strength. It used to reset
 * every strength to its default behind the card, the slider, the title
 * and the render, which all still showed the tuned value: the state went
 * dirty with nothing changed on screen and `s` exported the default. */
static void
test_strength_survives_a_preference_change(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   edit_key(fx.p_win, GDK_KEY_j, 0); /* Brightness */
   edit_key_and_wait(fx.p_win, GDK_KEY_l, 0);
   wait_for_title(fx.p_win, "Brightness +0.6");
   fire(fx.p_win, "win.enhance-save");
   wait_for_status_prefix(fx.p_win, "Saved ");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   guint       u_renders = ggaze_window_enhance_render_count(fx.p_win);
   GdkTexture *p_shown   = ref_viewer_texture(fx.p_win);
   Settings   *p_s       = settings_new();
   settings_set_background(p_s, settings_get_background(p_s) == GGAZE_BG_GREY
                                   ? GGAZE_BG_DARK
                                   : GGAZE_BG_GREY);
   ggtest_drain_main(300);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win), ==, u_renders);
   g_assert_true(viewer_texture(fx.p_win) == p_shown);
   g_assert_cmpstr(card_value(fx.p_win, 1), ==, "+0.6");
   g_assert_cmpfloat(
      gtk_range_get_value(GTK_RANGE(find_scale(find_panel(fx.p_win), 1))), ==,
      0.6);
   g_assert_nonnull(g_strstr_len(window_title(fx.p_win), -1, "+0.6"));
   /* The next step starts from the tuned value, not from a reset one. */
   edit_key_and_wait(fx.p_win, GDK_KEY_l, 0);
   wait_for_title(fx.p_win, "Brightness +0.7");
   g_object_unref(p_shown);
   g_settings_reset(settings_get_gsettings(p_s), "background");
   settings_delete(p_s);
   tool_fx_close(&fx);
}

/* 8i2 review: the selected card's keys wait for a crop / straighten tool.
 * The straighten tool leaves j / k and Shift+H / L alone; they used to
 * reach the panel under it, moving the selection or a strength (a render
 * and an undo step) behind the modal tool. Now the router passes them on
 * to the global table; a digit still toggles a preset under it. */
static void
test_card_keys_wait_for_the_tool(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   edit_key(fx.p_win, GDK_KEY_j, 0); /* Brightness */
   fire(fx.p_win, "win.straighten");
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_STRAIGHTEN);
   ggtest_drain_main(100);
   guint u_renders = ggaze_window_enhance_render_count(fx.p_win);
   g_assert_false(ggaze_window_edit_key(fx.p_win, GDK_KEY_j, 0));
   g_assert_false(ggaze_window_edit_key(fx.p_win, GDK_KEY_k, 0));
   g_assert_false(ggaze_window_edit_key(fx.p_win, GDK_KEY_L, GDK_SHIFT_MASK));
   g_assert_false(ggaze_window_edit_key(fx.p_win, GDK_KEY_H, GDK_SHIFT_MASK));
   ggtest_drain_main(200);
   assert_selected(fx.p_win, 1);
   g_assert_cmpstr(card_value(fx.p_win, 1), ==, "+0.5");
   g_assert_false(
      gtk_widget_has_css_class(panel_card(fx.p_win, 1), "ggaze-enhance-on"));
   g_assert_cmpuint(ggaze_window_enhance_render_count(fx.p_win), ==, u_renders);
   assert_history_buttons(fx.p_win, FALSE, FALSE);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_STRAIGHTEN);
   /* The digits are still the panel's under the tool. */
   edit_key_and_wait(fx.p_win, GDK_KEY_1, 0);
   g_assert_true(
      gtk_widget_has_css_class(panel_card(fx.p_win, 0), "ggaze-enhance-on"));
   tool_fx_close(&fx);
}

/* 8i2 review: the arrows are not the panel's. With it open, Left / Right
 * still change the image and Up / Down / Shift+arrows pan -- flicking
 * through a folder with the arrows never moves a strength. */
static void
test_arrows_stay_global_with_the_panel_open(void) {
   ToolFx fx;
   tool_fx_open_with_sibling(&fx);
   fire(fx.p_win, "win.enhance");
   edit_key(fx.p_win, GDK_KEY_j, 0); /* Brightness, tunable */
   static const guint KEYS[] = {GDK_KEY_Left, GDK_KEY_Right, GDK_KEY_Up,
                                GDK_KEY_Down};
   for (gsize u = 0; u < G_N_ELEMENTS(KEYS); u++) {
      g_assert_false(ggaze_window_edit_key(fx.p_win, KEYS[u], 0));
      g_assert_false(ggaze_window_edit_key(fx.p_win, KEYS[u], GDK_SHIFT_MASK));
   }
   assert_selected(fx.p_win, 1);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_false(emit_capture_key(fx.p_win, GDK_KEY_Left));
   activate_shortcut(fx.p_win, GDK_KEY_Left); /* -> win.prev */
   wait_for_load(fx.p_win, PLAIN_JPG_W, PLAIN_JPG_H);
   assert_showing(fx.p_win, "a.jpg");
   g_assert_nonnull(find_panel(fx.p_win)); /* the panel stays open */
   g_assert_cmpstr(card_value(fx.p_win, 1), ==, "+0.5");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   tool_fx_close(&fx);
}

/* 8i2 review: Shift held on Enter / Esc still applies / cancels a tool --
 * no tool has a Shift twin of either, so Shift is noise on them (a matcher
 * that compared Shift on every key that prints nothing refused both). */
static void
test_shift_enter_and_esc_finish_a_tool(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.crop");
   crop_shrink_right(fx.p_win);
   edit_key(fx.p_win, GDK_KEY_Escape, GDK_SHIFT_MASK);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   g_assert_cmpstr(status_text(fx.p_win), ==, "Crop cancelled");
   fire(fx.p_win, "win.crop");
   for (guint u = 0; u < 10; u++) {
      crop_shrink_right(fx.p_win);
      crop_shrink_bottom(fx.p_win);
   }
   GdkTexture *p_before = ref_viewer_texture(fx.p_win);
   edit_key(fx.p_win, GDK_KEY_Return, GDK_SHIFT_MASK);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   wait_for_texture_change(fx.p_win, p_before);
   g_object_unref(p_before);
   assert_texture_size(fx.p_win, TOOL_W - 30, TOOL_H - 30);
   fire(fx.p_win, "win.straighten");
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   edit_key(fx.p_win, GDK_KEY_Escape, GDK_SHIFT_MASK);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   wait_for_title_without(fx.p_win, "straighten");
   fire(fx.p_win, "win.straighten");
   tool_key_and_wait(fx.p_win, GDK_KEY_l);
   edit_key(fx.p_win, GDK_KEY_KP_Enter, GDK_SHIFT_MASK);
   g_assert_cmpint(ggaze_window_get_tool(fx.p_win), ==, GGAZE_TOOL_NONE);
   wait_for_title(fx.p_win, "straighten");
   tool_fx_close(&fx);
}

/* 8i2 review: saved compares the strengths of the presets that are ON --
 * a disabled preset's strength renders nothing. Contrast on and saved,
 * Brightness tuned (on) and switched off again: the render is the saved
 * one, so the state is saved again, whatever Brightness's number is. */
static void
test_strength_of_a_disabled_preset_is_not_unsaved(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   edit_key_and_wait(fx.p_win, GDK_KEY_3, 0); /* Contrast */
   fire(fx.p_win, "win.enhance-save");
   wait_for_status_prefix(fx.p_win, "Saved ");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key(fx.p_win, GDK_KEY_k, 0); /* Brightness */
   edit_key_and_wait(fx.p_win, GDK_KEY_l, 0);
   wait_for_title(fx.p_win, "Brightness +0.6");
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key_and_wait(fx.p_win, GDK_KEY_2, 0); /* Brightness off */
   g_assert_cmpstr(card_value(fx.p_win, 1), ==, "+0.6");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   g_assert_nonnull(find_label_prefix(find_panel(fx.p_win), "Saved as "));
   tool_fx_close(&fx);
}

/* 8i2 review: a save and the panel closing end a strength run, so undo
 * can stop at the value saved / closed on rather than jump past it. */
static void
test_save_and_close_end_the_strength_run(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   edit_key(fx.p_win, GDK_KEY_j, 0); /* Brightness */
   edit_key(fx.p_win, GDK_KEY_l, 0);
   edit_key(fx.p_win, GDK_KEY_l, 0);
   wait_for_title(fx.p_win, "Brightness +0.7");
   fire(fx.p_win, "win.enhance-save");
   wait_for_status_prefix(fx.p_win, "Saved ");
   edit_key(fx.p_win, GDK_KEY_l, 0);
   wait_for_title(fx.p_win, "Brightness +0.8");
   edit_key(fx.p_win, GDK_KEY_u, 0);
   wait_for_title(fx.p_win, "Brightness +0.7"); /* the saved value */
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   /* The undo closed that run; open a new one, then close the panel on
    * it: the press after it opens again is a step of its own. */
   edit_key(fx.p_win, GDK_KEY_l, 0);
   wait_for_title(fx.p_win, "Brightness +0.8");
   fire(fx.p_win, "win.enhance"); /* close ... */
   fire(fx.p_win, "win.enhance"); /* ... and open again */
   edit_key(fx.p_win, GDK_KEY_l, 0);
   wait_for_title(fx.p_win, "Brightness +0.9");
   edit_key(fx.p_win, GDK_KEY_u, 0);
   wait_for_title(fx.p_win, "Brightness +0.8");
   g_assert_cmpstr(card_value(fx.p_win, 1), ==, "+0.8");
   tool_fx_close(&fx);
}

/* 8i2 review: the wheel over the selected card's slider scrolls the cards'
 * list, not the strength: the slider's own scroll controller (GtkRange's)
 * is switched off, so the event bubbles on to the scrolled window. */
static void
test_strength_slider_ignores_the_wheel(void) {
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   GtkWidget  *p_scale = find_scale(find_panel(fx.p_win), 1);
   GListModel *p_ctrls = gtk_widget_observe_controllers(p_scale);
   guint       u_found = 0;
   for (guint u = 0; u < g_list_model_get_n_items(p_ctrls); u++) {
      GtkEventController *p_c = g_list_model_get_item(p_ctrls, u);
      if (GTK_IS_EVENT_CONTROLLER_SCROLL(p_c)) {
         g_assert_cmpint(gtk_event_controller_get_propagation_phase(p_c), ==,
                         GTK_PHASE_NONE);
         u_found++;
      }
      g_object_unref(p_c);
   }
   g_object_unref(p_ctrls);
   g_assert_cmpuint(u_found, >, 0); /* GtkRange's own is there, and off */
   tool_fx_close(&fx);
}

/* 8i2: the selected card, its strength keys and its slider. */
static void
add_strength_tests(void) {
   g_test_add_func("/enhance_flow/strength_select_and_enter",
                   test_strength_select_and_enter);
   g_test_add_func("/enhance_flow/strength_keys_turn_on_and_name_it",
                   test_strength_keys_turn_on_and_name_it);
   g_test_add_func("/enhance_flow/strength_refused_without_placeholder",
                   test_strength_refused_without_placeholder);
   g_test_add_func("/enhance_flow/strength_run_undoes_as_one_step",
                   test_strength_run_undoes_as_one_step);
   g_test_add_func("/enhance_flow/strength_slider_drag",
                   test_strength_slider_drag);
   g_test_add_func("/enhance_flow/strength_saved_and_dirty",
                   test_strength_saved_and_dirty);
   g_test_add_func("/enhance_flow/strength_resets_on_revert_and_navigation",
                   test_strength_resets_on_revert_and_navigation);
   g_test_add_func("/enhance_flow/strength_survives_a_preference_change",
                   test_strength_survives_a_preference_change);
   g_test_add_func("/enhance_flow/card_keys_wait_for_the_tool",
                   test_card_keys_wait_for_the_tool);
   g_test_add_func("/enhance_flow/arrows_stay_global_with_the_panel_open",
                   test_arrows_stay_global_with_the_panel_open);
   g_test_add_func("/enhance_flow/shift_enter_and_esc_finish_a_tool",
                   test_shift_enter_and_esc_finish_a_tool);
   g_test_add_func("/enhance_flow/strength_of_a_disabled_preset_is_not_unsaved",
                   test_strength_of_a_disabled_preset_is_not_unsaved);
   g_test_add_func("/enhance_flow/save_and_close_end_the_strength_run",
                   test_save_and_close_end_the_strength_run);
   g_test_add_func("/enhance_flow/strength_slider_ignores_the_wheel",
                   test_strength_slider_ignores_the_wheel);
}

/* --- ai2: the user's presets are panel rows -------------------------------
 *
 * The enhance-presets setting's presets follow the eight built-ins as rows
 * of the edit panel: j / k reach every one (the list scrolls it into
 * view), Enter toggles it, h / l and its slider tune a tunable one, and it
 * is part of the title, dirty / saved and undo like a built-in; the digits
 * stay 1-8. A Preferences change of the list carries the edit to the rows
 * its presets have now. The setting lives in the memory GSettings backend
 * the suite runs on; every subtest that sets it resets it. */

/* Set the enhance-presets setting to u_n (name, graph) pairs. The window
 * picks the change up live (its settings "changed" handler). */
static void
set_user_presets(const char *const *c_pairs, guint u_n) {
   Settings  *p_s     = settings_new();
   GPtrArray *p_pairs = settings_pair_array_new();
   for (guint u = 0; u < u_n; u++) {
      g_ptr_array_add(p_pairs,
                      settings_pair_new(c_pairs[2 * u], c_pairs[2 * u + 1]));
   }
   settings_set_enhance_presets(p_s, p_pairs);
   g_ptr_array_unref(p_pairs);
   settings_delete(p_s);
}

/* u_n generated presets "User 1" .. "User <u_n>" (distinct graphs). */
static void
set_many_user_presets(guint u_n) {
   GPtrArray *p_strs = g_ptr_array_new_with_free_func(g_free);
   for (guint u = 0; u < u_n; u++) {
      g_ptr_array_add(p_strs, g_strdup_printf("User %u", u + 1));
      g_ptr_array_add(p_strs,
                      g_strdup_printf("gegl:saturation scale=1.%02u", u + 1));
   }
   set_user_presets((const char *const *)p_strs->pdata, u_n);
   g_ptr_array_unref(p_strs);
}

static void
reset_user_presets(void) {
   Settings *p_s = settings_new();
   g_settings_reset(settings_get_gsettings(p_s), "enhance-presets");
   settings_delete(p_s);
}

/* The first label under p_root that is not a strength value, or NULL. */
static GtkWidget *
find_name_label(GtkWidget *p_root) {
   if (GTK_IS_LABEL(p_root) &&
       !gtk_widget_has_css_class(p_root, GGAZE_ENHANCE_VALUE_CLASS)) {
      return (p_root);
   }
   for (GtkWidget *p_c = gtk_widget_get_first_child(p_root); p_c != NULL;
        p_c            = gtk_widget_get_next_sibling(p_c)) {
      GtkWidget *p_found = find_name_label(p_c);
      if (p_found != NULL) {
         return (p_found);
      }
   }
   return (NULL);
}

/* Card i_idx's name ("1  Auto-fix", or a user preset's bare name). */
static const char *
card_name(GgazeWindow *p_win, gint i_idx) {
   GtkWidget *p_lbl = find_name_label(panel_card(p_win, i_idx));
   g_assert_nonnull(p_lbl);
   return (gtk_label_get_text(GTK_LABEL(p_lbl)));
}

/* Exactly row i_idx of u_rows is selected (the ring), and only its
 * slider, if it has one, shows. */
static void
assert_selected_row(GgazeWindow *p_win, gint i_idx, gint i_rows) {
   for (gint i = 0; i < i_rows; i++) {
      GtkWidget *p_card = panel_card(p_win, i);
      g_assert_cmpint(
         gtk_widget_has_css_class(p_card, GGAZE_ENHANCE_SELECTED_CLASS), ==,
         i == i_idx);
      GtkWidget *p_scale = find_scale(find_panel(p_win), i);
      if (p_scale != NULL) {
         g_assert_cmpint(gtk_widget_get_visible(p_scale), ==, i == i_idx);
      }
   }
   g_assert_null(find_card(find_panel(p_win), i_rows)); /* no row past */
}

/* TRUE iff card i_idx is on (highlighted). */
static gboolean
card_on(GgazeWindow *p_win, gint i_idx) {
   return (
      gtk_widget_has_css_class(panel_card(p_win, i_idx), "ggaze-enhance-on"));
}

#define _ALPHA "Alpha", "gegl:exposure exposure={s:0.3:0..1:0.1}"
#define _BETA "Beta", "gegl:brightness-contrast contrast=0.5"
#define _GAMMA "Gamma", "gegl:saturation scale={s:0.5:0..2:0.1}"

/* Row 8 (Gamma, the first user preset, tunable) is reached by j past the
 * built-ins, toggled by Enter, tuned by l (the title and the card name
 * the value) and by its slider, which shows under it. */
static void
rows_tune_gamma(GgazeWindow *p_win) {
   for (guint u = 0; u < 8; u++) {
      edit_key(p_win, GDK_KEY_j, 0);
   }
   assert_selected_row(p_win, 8, 10);
   edit_key_and_wait(p_win, GDK_KEY_Return, 0);
   g_assert_true(card_on(p_win, 8));
   g_assert_true(ggaze_window_enhance_is_dirty(p_win));
   wait_for_title(p_win, "Gamma");
   edit_key_and_wait(p_win, GDK_KEY_l, 0);
   wait_for_title(p_win, "Gamma 0.6");
   g_assert_cmpstr(card_value(p_win, 8), ==, "0.6");
   GtkWidget *p_scale = find_scale(find_panel(p_win), 8);
   g_assert_true(gtk_widget_get_visible(p_scale));
   drag_scale_to(p_win, 8, 1.04); /* snaps to 1 */
   wait_for_title(p_win, "Gamma 1");
   g_assert_cmpfloat(gtk_range_get_value(GTK_RANGE(p_scale)), ==, 1.0);
}

/* Two user presets, one tunable and one plain, are rows 9 and 10 after
 * the built-ins: named without a digit, reached by j, toggled by Enter,
 * the tunable one tuned by h / l and its slider; both in the title, dirty
 * / saved and undo like a built-in. Digits stay 1-8: 9 is not the panel's,
 * and the hint bar still says 1–8. */
static void
test_user_presets_are_panel_rows(void) {
   static const char *const PAIRS[] = {_GAMMA, _BETA};
   set_user_presets(PAIRS, 2);
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   fire(fx.p_win, "win.enhance");
   g_assert_cmpstr(card_name(fx.p_win, 7), ==, "8  Denoise");
   g_assert_cmpstr(card_name(fx.p_win, 8), ==, "Gamma"); /* no digit */
   g_assert_cmpstr(card_name(fx.p_win, 9), ==, "Beta");
   g_assert_cmpstr(card_value(fx.p_win, 8), ==, "0.5");
   g_assert_null(card_value(fx.p_win, 9));
   char *c_hint = plain_hint(fx.p_win);
   g_assert_nonnull(g_strstr_len(c_hint, -1, "1\u20138 presets"));
   g_free(c_hint);
   g_assert_false(ggaze_window_edit_key(fx.p_win, GDK_KEY_9, 0));
   rows_tune_gamma(fx.p_win);
   edit_key(fx.p_win, GDK_KEY_j, 0);
   assert_selected_row(fx.p_win, 9, 10);
   edit_key(fx.p_win, GDK_KEY_l, 0);
   assert_status_prefix(fx.p_win, "Beta has no strength to adjust");
   edit_key_and_wait(fx.p_win, GDK_KEY_KP_Enter, 0);
   g_assert_true(card_on(fx.p_win, 9));
   wait_for_title(fx.p_win, "Gamma 1, Beta");
   fire(fx.p_win, "win.enhance-save");
   wait_for_status_prefix(fx.p_win, "Saved ");
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key_and_wait(fx.p_win, GDK_KEY_u, 0);
   assert_status_prefix(fx.p_win, "Undid: Beta on");
   g_assert_false(card_on(fx.p_win, 9));
   g_assert_true(ggaze_window_enhance_is_dirty(fx.p_win));
   edit_key_and_wait(fx.p_win, GDK_KEY_U, GDK_SHIFT_MASK);
   g_assert_false(ggaze_window_enhance_is_dirty(fx.p_win)); /* saved again */
   edit_key_and_wait(fx.p_win, GDK_KEY_u, 0);
   edit_key_and_wait(fx.p_win, GDK_KEY_u, 0); /* the drag */
   assert_status_prefix(fx.p_win, "Undid: Gamma 1");
   /* The digits still toggle the built-ins, and select their row. */
   edit_key_and_wait(fx.p_win, GDK_KEY_8, 0);
   g_assert_true(card_on(fx.p_win, 7));
   assert_selected_row(fx.p_win, 7, 10);
   tool_fx_close(&fx);
   reset_user_presets();
}

/* TRUE iff card i_idx lies wholly inside its list's scrolled view. */
static gboolean
card_in_view(GgazeWindow *p_win, gint i_idx) {
   GtkWidget *p_card = panel_card(p_win, i_idx);
   GtkWidget *p_sw = gtk_widget_get_ancestor(p_card, GTK_TYPE_SCROLLED_WINDOW);
   graphene_rect_t r_card;
   if (p_sw == NULL || !gtk_widget_compute_bounds(p_card, p_sw, &r_card)) {
      return (FALSE);
   }
   return (r_card.origin.y >= -0.5f &&
           r_card.origin.y + r_card.size.height <=
              (gfloat)gtk_widget_get_height(p_sw) + 0.5f);
}

/* Pump until card i_idx is in view (up to 5 s), then assert it is. */
static void
wait_card_in_view(GgazeWindow *p_win, gint i_idx) {
   for (guint u = 0; u < 5000 && !card_in_view(p_win, i_idx); u++) {
      g_main_context_iteration(NULL, FALSE);
      g_usleep(1000);
   }
   g_assert_true(card_in_view(p_win, i_idx));
}

/* The cap: 25 user presets, the 25th ignored -- 32 rows. At 1280x800 they
 * do not fit, so the preset list scrolls while Transform and the Actions
 * stay on screen; j to the last row scrolls it into view (the first one
 * out), k back to the top brings the first back. */
static void
test_user_presets_scroll_and_cap(void) {
   set_many_user_presets(GGAZE_ENHANCE_MAX_USER_PRESETS + 1);
   char        *c_dir  = NULL;
   char        *c_path = NULL;
   GgazeWindow *p_win = open_presented_sized(FALSE, "ggaze-enhance-many-XXXXXX",
                                             1280, 800, &c_dir, &c_path);
   fire(p_win, "win.enhance");
   GtkWidget *p_panel = find_panel(p_win);
   g_assert_nonnull(find_card(p_panel, GGAZE_ENHANCE_MAX_PRESETS - 1));
   g_assert_null(find_card(p_panel, GGAZE_ENHANCE_MAX_PRESETS)); /* capped */
   gint i_last = GGAZE_ENHANCE_MAX_PRESETS - 1;
   wait_card_in_view(p_win, 0);
   g_assert_false(card_in_view(p_win, i_last)); /* the list does scroll */
   for (gint i = 0; i < i_last + 3; i++) {
      edit_key(p_win, GDK_KEY_j, 0); /* stops at the last row */
   }
   assert_selected_row(p_win, i_last, GGAZE_ENHANCE_MAX_PRESETS);
   wait_card_in_view(p_win, i_last);
   g_assert_false(card_in_view(p_win, 0));
   g_assert_cmpstr(card_name(p_win, i_last), ==, "User 24");
   /* Transform and the Actions never scrolled away. */
   graphene_rect_t r_redo, r_panel;
   g_assert_true(gtk_widget_compute_bounds(
      find_action_button(p_panel, "win.edit-redo"), p_panel, &r_redo));
   g_assert_true(gtk_widget_compute_bounds(p_panel, p_panel, &r_panel));
   g_assert_cmpfloat(r_redo.origin.y + r_redo.size.height, <=,
                     r_panel.size.height);
   g_assert_cmpint(gtk_widget_get_height(GTK_WIDGET(p_win)), <=, 800);
   edit_key_and_wait(p_win, GDK_KEY_Return, 0); /* the last row toggles */
   g_assert_true(card_on(p_win, i_last));
   wait_for_title(p_win, "User 24");
   for (gint i = 0; i < i_last; i++) {
      edit_key(p_win, GDK_KEY_k, 0);
   }
   wait_card_in_view(p_win, 0);
   g_assert_false(card_in_view(p_win, i_last));
   /* A dirty preview would prompt on close: throw it away first. */
   fire(p_win, "win.edit-revert");
   ggtest_drain_main(100);
   close_presented(p_win, c_dir, c_path);
   reset_user_presets();
}

/* Set the presets and wait for the render that change causes -- one
 * launched by the change itself (the render count), not a texture that
 * happened to change. */
static void
set_user_presets_and_wait(GgazeWindow *p_win, const char *const *c_pairs,
                          guint u_n) {
   guint       u_renders = ggaze_window_enhance_render_count(p_win);
   GdkTexture *p_before  = ref_viewer_texture(p_win);
   set_user_presets(c_pairs, u_n);
   g_assert_cmpuint(ggaze_window_enhance_render_count(p_win), ==,
                    u_renders + 1);
   wait_for_texture_change(p_win, p_before);
   g_assert_true(viewer_texture(p_win) != p_before); /* (the wait is silent) */
   g_object_unref(p_before);
}

/* Over Alpha / Beta / Gamma: Beta on, Gamma tuned to 0.6 (on, selected),
 * saved. Returns the render count after it. */
static guint
carry_setup(GgazeWindow *p_win) {
   fire(p_win, "win.enhance");
   for (guint u = 0; u < 9; u++) {
      edit_key(p_win, GDK_KEY_j, 0);
   }
   edit_key_and_wait(p_win, GDK_KEY_Return, 0); /* Beta on */
   edit_key(p_win, GDK_KEY_j, 0);
   edit_key_and_wait(p_win, GDK_KEY_l, 0); /* Gamma 0.6, on */
   wait_for_title(p_win, "Beta, Gamma 0.6");
   fire(p_win, "win.enhance-save");
   wait_for_status_prefix(p_win, "Saved ");
   return (ggaze_window_enhance_render_count(p_win));
}

/* An added preset, then a reorder that keeps Beta before Gamma (Alpha,
 * off, to the end; the added one removed): no render, still saved, each
 * preset's state and the selection in its new row, and undo / redo walk
 * the steps taken before. */
static void
carry_add_and_reorder(GgazeWindow *p_win, guint u_renders) {
   static const char *const ABGD[] = {_ALPHA, _BETA, _GAMMA, "Delta",
                                      "gegl:saturation scale=0.2"};
   static const char *const BGA[]  = {_BETA, _GAMMA, _ALPHA};
   set_user_presets(ABGD, 4);
   ggtest_drain_main(300);
   g_assert_cmpstr(card_name(p_win, 11), ==, "Delta");
   g_assert_false(card_on(p_win, 11));
   g_assert_cmpuint(ggaze_window_enhance_render_count(p_win), ==, u_renders);
   g_assert_false(ggaze_window_enhance_is_dirty(p_win));
   assert_selected_row(p_win, 10, 12);
   set_user_presets(BGA, 3);
   ggtest_drain_main(300);
   g_assert_cmpuint(ggaze_window_enhance_render_count(p_win), ==, u_renders);
   g_assert_false(ggaze_window_enhance_is_dirty(p_win));
   g_assert_nonnull(find_label_prefix(find_panel(p_win), "Saved as "));
   g_assert_true(card_on(p_win, 8) && card_on(p_win, 9));
   g_assert_false(card_on(p_win, 10));
   g_assert_cmpstr(card_name(p_win, 9), ==, "Gamma");
   g_assert_cmpstr(card_value(p_win, 9), ==, "0.6");
   g_assert_cmpstr(card_value(p_win, 10), ==, "0.3");
   assert_selected_row(p_win, 9, 11); /* on Gamma still */
   wait_for_title(p_win, "Beta, Gamma 0.6");
   edit_key_and_wait(p_win, GDK_KEY_u, 0);
   assert_status_prefix(p_win, "Undid: Gamma 0.6");
   g_assert_false(card_on(p_win, 9));
   g_assert_cmpstr(card_value(p_win, 9), ==, "0.5");
   edit_key_and_wait(p_win, GDK_KEY_U, GDK_SHIFT_MASK);
   g_assert_false(ggaze_window_enhance_is_dirty(p_win)); /* saved again */
}

/* Two enabled presets swapped (and Alpha removed): a render, and the
 * saved copy no longer counts (another chain); Beta's graph edited in
 * place: still on, rendered anew; Beta removed while on: gone, with the
 * undo step that only toggled it -- Gamma's step still undoes. */
static void
carry_swap_edit_remove(GgazeWindow *p_win, GdkTexture *p_orig) {
   static const char *const GB[]  = {_GAMMA, _BETA};
   static const char *const GB2[] = {_GAMMA, "Beta",
                                     "gegl:brightness-contrast contrast=0.7"};
   static const char *const G[]   = {_GAMMA};
   set_user_presets_and_wait(p_win, GB, 2);
   wait_for_title(p_win, "Gamma 0.6, Beta");
   g_assert_true(ggaze_window_enhance_is_dirty(p_win));
   g_assert_null(find_card(find_panel(p_win), 10));
   set_user_presets_and_wait(p_win, GB2, 2);
   g_assert_true(card_on(p_win, 9));
   wait_for_title(p_win, "Gamma 0.6, Beta");
   set_user_presets_and_wait(p_win, G, 1);
   wait_for_title_without(p_win, "Beta");
   g_assert_null(find_card(find_panel(p_win), 9));
   edit_key_and_wait(p_win, GDK_KEY_u, 0);
   assert_status_prefix(p_win, "Undid: Gamma 0.6");
   g_assert_false(ggaze_window_enhance_is_dirty(p_win));
   g_assert_true(viewer_texture(p_win) == p_orig);
   assert_history_buttons(p_win, FALSE, TRUE);
}

/* Preferences edits the user presets while some are on: the edit follows
 * its presets to their new rows (see the steps above). */
static void
test_preferences_carry_the_edit(void) {
   static const char *const ABG[] = {_ALPHA, _BETA, _GAMMA};
   set_user_presets(ABG, 3);
   ToolFx fx;
   tool_fx_open(&fx, FALSE);
   guint u_renders = carry_setup(fx.p_win);
   carry_add_and_reorder(fx.p_win, u_renders);
   carry_swap_edit_remove(fx.p_win, fx.p_orig);
   tool_fx_close(&fx);
   reset_user_presets();
}

/* ai2: the user presets as panel rows. */
static void
add_user_preset_tests(void) {
   g_test_add_func("/enhance_flow/user_presets_are_panel_rows",
                   test_user_presets_are_panel_rows);
   g_test_add_func("/enhance_flow/user_presets_scroll_and_cap",
                   test_user_presets_scroll_and_cap);
   g_test_add_func("/enhance_flow/preferences_carry_the_edit",
                   test_preferences_carry_the_edit);
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
   enhancer_babl_ready(); /* as app.c, right after gegl_init() */
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

   add_feature_tests();
   add_prompt_outcome_tests();
   add_behind_the_prompt_tests();
   add_round4_tests();
   add_round5_tests();
   add_trash_cursor_tests();
   add_dispose_prompt_tests();
   add_tool_tests();
   add_tool_review_tests();
   add_edit_undo_tests();
   add_strength_tests();
   add_user_preset_tests();
   add_tool_review2_tests();
   add_tool_review3_tests();
   add_tool_review4_tests();
   add_tool_review5_tests();
   add_tool_review6_tests();
   add_tool_review7_tests();
   add_open_many_tests();
   add_icc_tests();
   return (g_test_run());
}
