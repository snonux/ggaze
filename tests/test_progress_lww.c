/*:*
 * ggaze — progressive last-write-wins regression test
 *
 * Regression for the stale-progressive-partial bug. _on_progress_main used to
 * call ggaze_viewer_set_texture for every queued partial without checking that
 * the partial's source file still equals navigator.current, and the JPEG
 * backend ignored cancellation. The dangerous case is the cache-hit window:
 *
 *   1. load A (cache miss)  -> A's progressive load starts in a worker thread;
 *      its low-res partial is emitted and queued onto the main context;
 *   2. navigate to B, which is already cached -> _load_current shows B from
 *      the cache immediately, cancels A, and starts NO new load (so no
 *      _load_finish_cb will run to correct the viewer afterwards);
 *   3. the main loop next drains A's already-queued partial.
 *
 * With the bug, step 3 overwrote B with A's partial and nothing corrected it
 * (B was a cache hit), so the displayed content diverged from navigator.current
 * and stayed wrong indefinitely. The fix carries the partial's source GFile to
 * the main thread and drops it unless it still equals navigator.current, and
 * the JPEG backend stops emitting once cancelled.
 *
 * Determinism: A is a large generated JPEG whose low-res decode is still in
 * flight when we navigate back to the cached B, so A's partial is guaranteed
 * to be dispatched AFTER B is current (and after A's load was cancelled), with
 * no subsequent load to correct the viewer. Tiny fixtures decode faster than
 * the navigation gap, which lets the partial land before B and be corrected --
 * not a faithful regression. Needs a display (CI: xvfb) and libjpeg.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "viewer.h"
#include "window.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

/* Dimensions of the generated large JPEG (A). Big enough that the 1/8-scale
 * low-res decode runs for several milliseconds, spanning the synchronous
 * next->prev navigation so the partial is queued after B is current. */
#define A_W 2048
#define A_H 1536

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

/* Generate a large JPEG in c_dir named c_name so its progressive low-res
 * decode takes real time. Returns the absolute path (caller frees). */
static char *
make_large_jpeg(const char *c_dir, const char *c_name) {
   GdkPixbuf *p_pix = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, A_W, A_H);
   g_assert_nonnull(p_pix);
   /* Fill with a non-trivial vertical gradient so the entropy isn't zero
    * (keeps libjpeg doing real DCT work without ballooning the file). */
   guchar *p_px  = gdk_pixbuf_get_pixels(p_pix);
   int     i_row = gdk_pixbuf_get_rowstride(p_pix);
   int     i_ch  = gdk_pixbuf_get_n_channels(p_pix);
   for (int y = 0; y < A_H; y++) {
      for (int x = 0; x < A_W; x++) {
         guchar *p = p_px + (gsize)y * i_row + (gsize)x * i_ch;
         p[0]      = (guchar)((x * 255) / A_W);
         p[1]      = (guchar)((y * 255) / A_H);
         p[2]      = (guchar)(((x + y) * 127) / (A_W + A_H));
      }
   }
   char   *c_path = g_build_filename(c_dir, c_name, NULL);
   GError *p_err  = NULL;
   g_assert_true(
      gdk_pixbuf_save(p_pix, c_path, "jpeg", &p_err, "quality", "60", NULL));
   g_assert_no_error(p_err);
   g_object_unref(p_pix);
   return (c_path);
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

static GgazeWindow *
new_window(void) {
   return (GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL)));
}

static GdkTexture *
viewer_texture(GgazeWindow *p_win) {
   GtkStack  *p_stack = ggaze_window_get_stack(p_win);
   GtkWidget *p_large = gtk_stack_get_child_by_name(p_stack, "large");
   return (ggaze_viewer_get_texture(GGAZE_VIEWER(p_large)));
}

/* Drain in-flight async loads so their callbacks (which hold a ref on the
 * window via LoadCtx) fire and release before the process exits. */
static void
drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

static gboolean
pump_until(gboolean (*p_pred)(gpointer), gpointer p_data, guint u_ms) {
   GMainContext *p_ctx = g_main_context_default();
   for (guint u = 0; u < u_ms; u++) {
      if (p_pred != NULL && p_pred(p_data)) {
         return (TRUE);
      }
      g_main_context_iteration(p_ctx, FALSE);
      g_usleep(1000);
   }
   return (p_pred != NULL && p_pred(p_data));
}

static gboolean
_dims_are(gpointer p_data) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   GdkTexture  *p_t   = viewer_texture(p_win);
   if (p_t == NULL) {
      return (FALSE);
   }
   gint i_w = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(p_win), "w"));
   gint i_h = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(p_win), "h"));
   return (gdk_texture_get_width(p_t) == i_w &&
           gdk_texture_get_height(p_t) == i_h);
}

/* plain.jpg is 6x3 (unrotated) and is the cached file we keep current (B). The
 * generated aaaa_large.jpg (A) low-res progressive partial is A_W/8 x A_H/8 =
 * 256x192. If A's stale partial were shown over the cached plain.jpg, the
 * viewer would read 256x192, not 6x3 -> the assertion fails. Name sort puts
 * "aaaa_large.jpg" (A) at idx 0 and "plain.jpg" (B) at idx 1. */
static void
test_stale_partial_does_not_overwrite_cache_hit(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-lww-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   char *c_a_path = make_large_jpeg(c_dir, "aaaa_large.jpg"); /* idx 0 */
   copy_fixture(c_dir, "plain.jpg");                          /* idx 1 */

   /* Open B (plain.jpg, idx 1) directly so it is current and gets cached. */
   char  *c_b_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_b      = g_file_new_for_path(c_b_path);
   g_free(c_b_path);
   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_b); /* current = plain.jpg */

   /* Step 1: let plain.jpg fully load AND enter the texture cache. */
   g_object_set_data(G_OBJECT(p_win), "w", GINT_TO_POINTER(6));
   g_object_set_data(G_OBJECT(p_win), "h", GINT_TO_POINTER(3));
   g_assert_true(pump_until(_dims_are, p_win, 5000));

   /* Step 2: navigate to A (aaaa_large.jpg, idx 0) -- a cache miss, so its
    * progressive load starts. Do NOT pump; A's low-res decode is still running
    * (it spans the next->prev gap below). */
   ggaze_window_prev(p_win); /* current = A (idx 0), cache miss */

   /* Step 3: immediately navigate back to B (plain.jpg, idx 1), a cache hit.
    * _load_current shows B (6x3) at once, cancels A, and starts NO new load --
    * so nothing will correct the viewer afterwards. This happens while A's
    * worker is still decoding, before A's partial is emitted. */
   ggaze_window_next(p_win); /* current = B (idx 1), cache hit */
   g_assert_true(gdk_texture_get_width(viewer_texture(p_win)) == 6);
   g_assert_true(gdk_texture_get_height(viewer_texture(p_win)) == 3);

   /* Step 4: drain long enough for A's worker to emit its partial and for
    * A's full decode to finish (or be cancelled) -- i.e. reach steady state.
    * Then assert the viewer still shows B. We must NOT use pump_until(dims==B)
    * here: that would succeed on the first check, before A's stale partial is
    * dispatched, and miss the corruption. With the bug, A's partial (256x192)
    * overwrote B here and nothing corrected it (B was a cache hit, A's full
    * result is dropped by the finish guard); with the fix the partial is
    * never shown (cancelled at the source, or dropped as stale). */
   drain_main(3000);
   g_object_set_data(G_OBJECT(p_win), "w", GINT_TO_POINTER(6));
   g_object_set_data(G_OBJECT(p_win), "h", GINT_TO_POINTER(3));
   g_assert_true(_dims_are(p_win));

   g_object_unref(p_win);
   drain_main(800);
   g_object_unref(p_b);
   g_free(c_a_path);
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
   g_test_add_func("/progress_lww/stale_partial_vs_cache_hit",
                   test_stale_partial_does_not_overwrite_cache_hit);
   return (g_test_run());
}