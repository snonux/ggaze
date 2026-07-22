/*:*
 * ggaze — navigator unit test
 *
 * Exercises the Navigator over temp directories (no display, no GTK widgets):
 * listing + image filter, RAW-sidecar hiding, sort (name/time/size), cursor +
 * prev/next/wrap, marks (toggle/range/all/clear/survive-resort), rescan +
 * nearest-fallback, navigator_remove, the GFileMonitor debounce path, the
 * "changed" signal, and empty-dir accessors.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "navigator.h"

#include <gio/gio.h>
#include <glib.h>
#include <stdlib.h>

/* --- helpers ------------------------------------------------------------- */
/* write_file/touch_file create a file on disk and return nothing (no GFile to
 * leak). file_ref() returns a fresh GFile handle for tests that need to keep a
 * reference (to pass to the navigator, delete, or set attributes). */

static char *
make_temp_dir(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-nav-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   return (c_dir);
}

/* Recursively remove a flat temp dir tree, then free the path string. */
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

static void
write_file(const char *c_dir, const char *c_name, const char *c_body,
           gssize i_len) {
   char          *c_path = g_build_filename(c_dir, c_name, NULL);
   GFile         *p_file = g_file_new_for_path(c_path);
   GError        *p_err  = NULL;
   GOutputStream *p_out  = (GOutputStream *)g_file_replace(
      p_file, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL, &p_err);
   g_assert_no_error(p_err);
   if (i_len < 0) {
      i_len = (gssize)strlen(c_body);
   }
   gsize u_written = 0;
   g_assert_true(g_output_stream_write_all(p_out, c_body, (gsize)i_len,
                                           &u_written, NULL, &p_err));
   g_assert_no_error(p_err);
   g_assert_true(g_output_stream_close(p_out, NULL, &p_err));
   g_assert_no_error(p_err);
   g_object_unref(p_out);
   g_object_unref(p_file);
   g_free(c_path);
}

static void
touch_file(const char *c_dir, const char *c_name) {
   write_file(c_dir, c_name, "x", 1);
}

static GFile *
file_ref(const char *c_dir, const char *c_name) {
   char  *c_path = g_build_filename(c_dir, c_name, NULL);
   GFile *p_file = g_file_new_for_path(c_path);
   g_free(c_path);
   return (p_file); /* (transfer full) */
}

/* Get the basename, assert it equals c_expected, and free it (LSan-clean). */
static void
assert_name(GFile *p_file, const char *c_expected) {
   char *c_name = g_file_get_basename(p_file);
   g_assert_cmpstr(c_name, ==, c_expected);
   g_free(c_name);
}

/* pump the default main context for up to u_ms, returning TRUE if p_pred
 * became TRUE. */
static gboolean
pump_until(gboolean (*p_pred)(gpointer), gpointer p_data, guint u_ms) {
   GMainContext *p_ctx = g_main_context_default();
   for (guint u = 0; u < u_ms; u++) {
      if (p_pred != NULL && p_pred(p_data)) {
         return (TRUE);
      }
      g_main_context_iteration(p_ctx, FALSE);
      g_usleep(1000); /* 1 ms */
   }
   return (p_pred != NULL && p_pred(p_data));
}

static gboolean
_count_is(gpointer p_data) {
   return (navigator_get_count((Navigator *)p_data) ==
           GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(p_data), "want")));
}

/* --- tests --------------------------------------------------------------- */

static void
test_filter_and_listing(void) {
   char *c_dir = make_temp_dir();
   write_file(c_dir, "a.jpg", "xx", 2);
   write_file(c_dir, "b.png", "yyy", 3);
   write_file(c_dir, "c.txt", "z", 1);       /* excluded: not an image */
   write_file(c_dir, ".hidden.jpg", "h", 1); /* excluded: dotfile */
   write_file(c_dir, "d.gif", "yy", 2);

   GFile     *p_dirf = g_file_new_for_path(c_dir);
   Navigator *p_nav  = navigator_new(p_dirf, GGAZE_SORT_NAME, TRUE, TRUE);
   g_assert_cmpint(navigator_get_count(p_nav), ==, 3); /* a, b, d */
   assert_name(navigator_get_file(p_nav, 0), "a.jpg");
   assert_name(navigator_get_file(p_nav, 1), "b.png");
   assert_name(navigator_get_file(p_nav, 2), "d.gif");

   navigator_delete(p_nav);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

static void
test_hide_raw_sidecars(void) {
   char *c_dir = make_temp_dir();
   write_file(c_dir, "img.jpg", "x", 1);
   write_file(c_dir, "img.raf", "x", 1);    /* RAW twin of img.jpg */
   write_file(c_dir, "lonely.nef", "x", 1); /* no JPEG twin -> kept */

   GFile     *p_dirf = g_file_new_for_path(c_dir);
   Navigator *p_nav  = navigator_new(p_dirf, GGAZE_SORT_NAME, TRUE, TRUE);
   g_assert_cmpint(navigator_get_count(p_nav), ==, 2); /* img.jpg, lonely.nef */
   g_assert_true(navigator_get_hide_raw(p_nav));

   navigator_set_hide_raw(p_nav, FALSE);
   g_assert_cmpint(navigator_get_count(p_nav), ==, 3); /* all three */

   navigator_delete(p_nav);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

static void
test_sort_time_and_size(void) {
   char *c_dir = make_temp_dir();
   write_file(c_dir, "a.jpg", "12345", 5);
   write_file(c_dir, "b.jpg", "0123456789ABCDEF", 16);
   GFile *p_small = file_ref(c_dir, "a.jpg");
   GFile *p_big   = file_ref(c_dir, "b.jpg");

   GFile     *p_dirf = g_file_new_for_path(c_dir);
   Navigator *p_nav  = navigator_new(p_dirf, GGAZE_SORT_NAME, TRUE, TRUE);
   g_assert_cmpint(navigator_get_sort(p_nav), ==, GGAZE_SORT_NAME);
   assert_name(navigator_get_current(p_nav), "a.jpg");

   navigator_set_sort(p_nav, GGAZE_SORT_SIZE);
   assert_name(navigator_get_file(p_nav, 0), "a.jpg"); /* size 5 < 16 */

   /* Give a.jpg a newer mtime than b.jpg via a direct attribute set. */
   guint64 u_t_old  = (guint64)1000000;
   guint64 u_t_new  = (guint64)9000000;
   GError *p_seterr = NULL;
   g_assert_true(g_file_set_attribute(p_big, "time::modified",
                                      G_FILE_ATTRIBUTE_TYPE_UINT64, &u_t_old,
                                      G_FILE_QUERY_INFO_NONE, NULL, &p_seterr));
   g_assert_no_error(p_seterr);
   g_assert_true(g_file_set_attribute(p_small, "time::modified",
                                      G_FILE_ATTRIBUTE_TYPE_UINT64, &u_t_new,
                                      G_FILE_QUERY_INFO_NONE, NULL, &p_seterr));
   g_assert_no_error(p_seterr);

   navigator_set_sort(p_nav, GGAZE_SORT_TIME);
   /* ascending: b.jpg (older) first, a.jpg (newer) last. */
   assert_name(navigator_get_file(p_nav, 0), "b.jpg");
   assert_name(navigator_get_file(p_nav, navigator_get_count(p_nav) - 1),
               "a.jpg");

   navigator_delete(p_nav);
   g_object_unref(p_small);
   g_object_unref(p_big);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

static void
test_prev_next_wrap(void) {
   char *c_dir = make_temp_dir();
   write_file(c_dir, "a.jpg", "x", 1);
   write_file(c_dir, "b.jpg", "x", 1);
   write_file(c_dir, "c.jpg", "x", 1);

   GFile     *p_dirf = g_file_new_for_path(c_dir);
   Navigator *p_nav  = navigator_new(p_dirf, GGAZE_SORT_NAME, TRUE, TRUE);
   g_assert_cmpint(navigator_get_current_index(p_nav), ==, 0);

   g_assert_true(navigator_next(p_nav));
   g_assert_cmpint(navigator_get_current_index(p_nav), ==, 1);
   g_assert_true(navigator_next(p_nav));
   g_assert_cmpint(navigator_get_current_index(p_nav), ==, 2);
   g_assert_true(navigator_next(p_nav)); /* wrap to 0 */
   g_assert_cmpint(navigator_get_current_index(p_nav), ==, 0);
   g_assert_true(navigator_prev(p_nav)); /* wrap to last */
   g_assert_cmpint(navigator_get_current_index(p_nav), ==, 2);

   navigator_set_wrap(p_nav, FALSE);
   g_assert_cmpint(navigator_get_wrap(p_nav), ==, FALSE);
   navigator_last(p_nav);
   g_assert_cmpint(navigator_get_current_index(p_nav), ==, 2);
   g_assert_false(navigator_next(p_nav)); /* no wrap */
   navigator_first(p_nav);
   g_assert_cmpint(navigator_get_current_index(p_nav), ==, 0);
   g_assert_false(navigator_prev(p_nav)); /* no wrap */

   navigator_delete(p_nav);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

static void
test_set_current_file(void) {
   char *c_dir = make_temp_dir();
   write_file(c_dir, "a.jpg", "x", 1);
   write_file(c_dir, "b.jpg", "x", 1);
   GFile *p_b = file_ref(c_dir, "b.jpg");

   GFile     *p_dirf = g_file_new_for_path(c_dir);
   Navigator *p_nav  = navigator_new(p_dirf, GGAZE_SORT_NAME, TRUE, TRUE);
   g_assert_cmpint(navigator_get_current_index(p_nav), ==, 0); /* a.jpg */

   g_assert_true(navigator_set_current_file(p_nav, p_b)); /* b.jpg at idx 1 */
   g_assert_cmpint(navigator_get_current_index(p_nav), ==, 1);
   assert_name(navigator_get_current(p_nav), "b.jpg");

   navigator_delete(p_nav);
   g_object_unref(p_b);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

static void
test_marks(void) {
   char *c_dir = make_temp_dir();
   write_file(c_dir, "a.jpg", "x", 1);
   write_file(c_dir, "b.jpg", "x", 1);
   write_file(c_dir, "c.jpg", "x", 1);
   GFile *p_a = file_ref(c_dir, "a.jpg");
   GFile *p_b = file_ref(c_dir, "b.jpg");
   GFile *p_c = file_ref(c_dir, "c.jpg");

   GFile     *p_dirf = g_file_new_for_path(c_dir);
   Navigator *p_nav  = navigator_new(p_dirf, GGAZE_SORT_NAME, TRUE, TRUE);

   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 0);
   navigator_toggle_mark(p_nav, p_b);
   g_assert_true(navigator_is_marked(p_nav, p_b));
   g_assert_false(navigator_is_marked(p_nav, p_a));
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 1);
   navigator_toggle_mark(p_nav, p_b); /* untoggle */
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 0);

   navigator_mark_range(p_nav, p_a, p_c); /* mark a, b, c */
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 3);

   /* marks survive a re-sort */
   navigator_set_sort(p_nav, GGAZE_SORT_SIZE);
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 3);
   g_assert_true(navigator_is_marked(p_nav, p_a));

   GList *p_marks = navigator_get_marks(p_nav);
   g_assert_cmpint(g_list_length(p_marks), ==, 3);
   g_list_free_full(p_marks, (GDestroyNotify)g_object_unref);

   navigator_clear_marks(p_nav);
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 0);

   navigator_mark_all(p_nav);
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 3);

   navigator_delete(p_nav);
   g_object_unref(p_a);
   g_object_unref(p_b);
   g_object_unref(p_c);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

/* `v` toggle-on records the anchor used by `V` range-mark; unmarking the
 * anchor drops it. navigator_mark_removed clears the mark (decision Q) and
 * the anchor if it was the removed file, without touching unrelated marks. */
static void
test_marks_anchor_and_remove(void) {
   char *c_dir = make_temp_dir();
   write_file(c_dir, "a.jpg", "x", 1);
   write_file(c_dir, "b.jpg", "x", 1);
   write_file(c_dir, "c.jpg", "x", 1);
   GFile *p_a = file_ref(c_dir, "a.jpg");
   GFile *p_b = file_ref(c_dir, "b.jpg");
   GFile *p_c = file_ref(c_dir, "c.jpg");

   GFile     *p_dirf = g_file_new_for_path(c_dir);
   Navigator *p_nav  = navigator_new(p_dirf, GGAZE_SORT_NAME, TRUE, TRUE);

   /* No anchor until a mark is toggled on. */
   g_assert_null(navigator_get_last_mark(p_nav));
   navigator_toggle_mark(p_nav, p_a); /* mark a, anchor = a */
   g_assert_true(g_file_equal(navigator_get_last_mark(p_nav), p_a));
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 1);

   /* Range from anchor a to current c marks a, b, c. */
   navigator_set_current_file(p_nav, p_c);
   navigator_mark_range(p_nav, navigator_get_last_mark(p_nav), p_c);
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 3);

   /* Unmarking the anchor drops it; the other marks remain. */
   navigator_toggle_mark(p_nav, p_a); /* unmark a (was the anchor) */
   g_assert_null(navigator_get_last_mark(p_nav));
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 2);
   g_assert_true(navigator_is_marked(p_nav, p_b));
   g_assert_true(navigator_is_marked(p_nav, p_c));

   /* Re-marking a sets the anchor again; clearing marks drops it. */
   navigator_toggle_mark(p_nav, p_a); /* mark a, anchor = a */
   g_assert_true(g_file_equal(navigator_get_last_mark(p_nav), p_a));
   navigator_clear_marks(p_nav);
   g_assert_null(navigator_get_last_mark(p_nav));
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 0);

   /* mark_removed clears that file's mark but leaves unrelated marks, and
    * drops the anchor when the removed file was the anchor. */
   navigator_toggle_mark(p_nav, p_b); /* mark b, anchor = b */
   navigator_toggle_mark(p_nav, p_c); /* mark c, anchor = c */
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 2);
   navigator_mark_removed(p_nav, p_c);
   g_assert_false(navigator_is_marked(p_nav, p_c));
   g_assert_true(navigator_is_marked(p_nav, p_b));
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 1);
   g_assert_null(navigator_get_last_mark(p_nav)); /* anchor was c -> cleared */
   /* mark_removed on the remaining (non-anchor) mark clears it too. */
   navigator_mark_removed(p_nav, p_b);
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 0);

   navigator_delete(p_nav);
   g_object_unref(p_a);
   g_object_unref(p_b);
   g_object_unref(p_c);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

/* The range anchor is dropped when its file leaves the listing via an
 * external delete + rescan (the monitor path), so `V` no-ops instead of
 * range-marking from a stale path. */
static void
test_marks_anchor_pruned_on_rescan(void) {
   char *c_dir = make_temp_dir();
   write_file(c_dir, "a.jpg", "x", 1);
   write_file(c_dir, "b.jpg", "x", 1);
   write_file(c_dir, "c.jpg", "x", 1);
   GFile *p_a = file_ref(c_dir, "a.jpg");
   GFile *p_b = file_ref(c_dir, "b.jpg");
   GFile *p_c = file_ref(c_dir, "c.jpg");

   GFile     *p_dirf = g_file_new_for_path(c_dir);
   Navigator *p_nav  = navigator_new(p_dirf, GGAZE_SORT_NAME, TRUE, TRUE);

   navigator_toggle_mark(p_nav, p_a); /* anchor = a */
   navigator_toggle_mark(p_nav, p_b);
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 2);
   g_assert_true(g_file_equal(navigator_get_last_mark(p_nav), p_b));

   /* Externally delete a (the anchor's mark prunes on rescan). */
   g_assert_true(g_file_delete(p_a, NULL, NULL));
   navigator_rescan(p_nav);
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 1); /* b remains */
   g_assert_false(navigator_is_marked(p_nav, p_a));

   /* Externally delete b: the anchor's file is gone, so the anchor drops. */
   g_assert_true(g_file_delete(p_b, NULL, NULL));
   navigator_rescan(p_nav);
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 0);
   g_assert_null(navigator_get_last_mark(p_nav)); /* anchor pruned */

   navigator_delete(p_nav);
   g_object_unref(p_a);
   g_object_unref(p_b);
   g_object_unref(p_c);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

static void
test_rescan_and_nearest_fallback(void) {
   char *c_dir = make_temp_dir();
   write_file(c_dir, "a.jpg", "x", 1);
   write_file(c_dir, "b.jpg", "x", 1);
   write_file(c_dir, "c.jpg", "x", 1);
   GFile *p_b = file_ref(c_dir, "b.jpg");

   GFile     *p_dirf = g_file_new_for_path(c_dir);
   Navigator *p_nav  = navigator_new(p_dirf, GGAZE_SORT_NAME, TRUE, TRUE);
   navigator_set_current_file(p_nav, p_b); /* current = b.jpg (idx 1) */
   g_assert_cmpint(navigator_get_current_index(p_nav), ==, 1);

   /* Delete b.jpg from disk and rescan: current gone -> nearest fallback. */
   g_assert_true(g_file_delete(p_b, NULL, NULL));
   navigator_rescan(p_nav);
   g_assert_cmpint(navigator_get_count(p_nav), ==, 2);
   /* nearest by position: old idx 1 -> stays at idx 1 (now c.jpg). */
   assert_name(navigator_get_current(p_nav), "c.jpg");

   /* Adding a file via rescan keeps the current. */
   touch_file(c_dir, "d.jpg");
   navigator_rescan(p_nav);
   g_assert_cmpint(navigator_get_count(p_nav), ==, 3);
   assert_name(navigator_get_current(p_nav), "c.jpg");

   navigator_delete(p_nav);
   g_object_unref(p_b);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

static void
test_remove_clears_mark_and_falls_back(void) {
   char *c_dir = make_temp_dir();
   write_file(c_dir, "a.jpg", "x", 1);
   write_file(c_dir, "b.jpg", "x", 1);
   write_file(c_dir, "c.jpg", "x", 1);
   GFile *p_b = file_ref(c_dir, "b.jpg");

   GFile     *p_dirf = g_file_new_for_path(c_dir);
   Navigator *p_nav  = navigator_new(p_dirf, GGAZE_SORT_NAME, TRUE, TRUE);
   navigator_set_current_file(p_nav, p_b);
   navigator_mark_all(p_nav);
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 3);

   g_assert_true(navigator_remove(p_nav, p_b)); /* remove current */
   g_assert_cmpint(navigator_get_count(p_nav), ==, 2);
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==,
                   2); /* b's mark cleared */
   /* nearest fallback: was idx 1 -> now c.jpg at idx 1. */
   assert_name(navigator_get_current(p_nav), "c.jpg");

   g_assert_false(navigator_remove(p_nav, p_b)); /* already gone */

   navigator_delete(p_nav);
   g_object_unref(p_b);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

static void
test_monitor_add(void) {
   char *c_dir = make_temp_dir();
   write_file(c_dir, "a.jpg", "x", 1);

   GFile     *p_dirf = g_file_new_for_path(c_dir);
   Navigator *p_nav  = navigator_new(p_dirf, GGAZE_SORT_NAME, TRUE, TRUE);
   navigator_set_debounce_ms(p_nav, 5); /* fast for the test */
   g_assert_cmpint(navigator_get_count(p_nav), ==, 1);

   touch_file(c_dir, "b.jpg");
   g_object_set_data(G_OBJECT(p_nav), "want", GUINT_TO_POINTER(2));
   g_assert_true(pump_until(_count_is, p_nav, 2000));

   navigator_delete(p_nav);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

static void
on_changed_cb(Navigator *p_nav, gpointer p_data) {
   (void)p_nav;
   (*(gint *)p_data)++;
}

static void
test_changed_signal(void) {
   char *c_dir = make_temp_dir();
   touch_file(c_dir, "a.jpg");
   touch_file(c_dir, "b.jpg");
   GFile     *p_dirf  = g_file_new_for_path(c_dir);
   Navigator *p_nav   = navigator_new(p_dirf, GGAZE_SORT_NAME, TRUE, TRUE);
   gint       i_count = 0;
   g_signal_connect(p_nav, "changed", G_CALLBACK(on_changed_cb), &i_count);

   navigator_emit_changed(p_nav);
   g_assert_cmpint(i_count, ==, 1);
   navigator_next(p_nav);
   g_assert_cmpint(i_count, ==, 2);
   navigator_set_sort(p_nav, GGAZE_SORT_SIZE);
   g_assert_cmpint(i_count, ==, 3);
   navigator_clear_marks(p_nav);
   g_assert_cmpint(i_count, ==, 4);

   navigator_delete(p_nav);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

static void
test_empty_dir(void) {
   char      *c_dir  = make_temp_dir();
   GFile     *p_dirf = g_file_new_for_path(c_dir);
   Navigator *p_nav  = navigator_new(p_dirf, GGAZE_SORT_NAME, TRUE, TRUE);

   g_assert_cmpint(navigator_get_count(p_nav), ==, 0);
   g_assert_cmpint(navigator_get_current_index(p_nav), ==, -1);
   g_assert_null(navigator_get_current(p_nav));
   g_assert_false(navigator_prev(p_nav));
   g_assert_false(navigator_next(p_nav));
   g_assert_false(navigator_first(p_nav));
   g_assert_false(navigator_last(p_nav));
   g_assert_cmpint(navigator_get_mark_count(p_nav), ==, 0);
   g_assert_null(navigator_get_marks(p_nav));
   g_assert_cmpint(navigator_get_remaining(p_nav), ==, 0);
   g_assert_true(g_file_equal(navigator_get_dir(p_nav), p_dirf));

   navigator_delete(p_nav);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

static void
test_remove_before_cursor(void) {
   char *c_dir = make_temp_dir();
   write_file(c_dir, "a.jpg", "x", 1);
   write_file(c_dir, "b.jpg", "x", 1);
   write_file(c_dir, "c.jpg", "x", 1);
   GFile *p_a = file_ref(c_dir, "a.jpg");
   GFile *p_c = file_ref(c_dir, "c.jpg");

   GFile     *p_dirf = g_file_new_for_path(c_dir);
   Navigator *p_nav  = navigator_new(p_dirf, GGAZE_SORT_NAME, TRUE, TRUE);
   navigator_set_current_file(p_nav, p_c); /* current = c.jpg (idx 2) */
   g_assert_cmpint(navigator_get_current_index(p_nav), ==, 2);
   g_assert_true(navigator_remove(p_nav, p_a)); /* remove idx 0 */
   /* cursor decrements: c.jpg stays current, now at idx 1. */
   g_assert_cmpint(navigator_get_current_index(p_nav), ==, 1);
   assert_name(navigator_get_current(p_nav), "c.jpg");

   navigator_delete(p_nav);
   g_object_unref(p_a);
   g_object_unref(p_c);
   g_object_unref(p_dirf);
   cleanup_temp_dir(c_dir);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/navigator/filter_and_listing", test_filter_and_listing);
   g_test_add_func("/navigator/hide_raw_sidecars", test_hide_raw_sidecars);
   g_test_add_func("/navigator/sort_time_and_size", test_sort_time_and_size);
   g_test_add_func("/navigator/prev_next_wrap", test_prev_next_wrap);
   g_test_add_func("/navigator/set_current_file", test_set_current_file);
   g_test_add_func("/navigator/marks", test_marks);
   g_test_add_func("/navigator/marks_anchor_and_remove",
                   test_marks_anchor_and_remove);
   g_test_add_func("/navigator/marks_anchor_pruned_on_rescan",
                   test_marks_anchor_pruned_on_rescan);
   g_test_add_func("/navigator/rescan_fallback",
                   test_rescan_and_nearest_fallback);
   g_test_add_func("/navigator/remove", test_remove_clears_mark_and_falls_back);
   g_test_add_func("/navigator/remove_before_cursor",
                   test_remove_before_cursor);
   g_test_add_func("/navigator/monitor_add", test_monitor_add);
   g_test_add_func("/navigator/changed_signal", test_changed_signal);
   g_test_add_func("/navigator/empty_dir", test_empty_dir);
   return (g_test_run());
}