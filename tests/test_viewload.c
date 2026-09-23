/*:*
 * ggaze — view-load pipeline unit test (no display)
 *
 * Drives viewload.c with a fake host over a real temp folder: a cache miss
 * takes the async path and paints the current file; a load that finishes
 * after the user moved on is dropped (last-write-wins); a corrupt current
 * file clears the canvas and reports a status line instead of leaving the
 * previous picture on screen; a texture rewritten in place is decoded
 * afresh (cache staleness); dispose mid-load never calls the host again;
 * a JPEG's low-res partial reaches the host through show_partial, never
 * show_texture, and a PNG shows no partial at all.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "viewload.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "ggaze-config.h"
#include "navigator.h"

/* --- fake host ----------------------------------------------------------- */

typedef struct {
   GdkTexture *p_shown;    /* last texture handed to show_texture (ref'd) */
   guint       u_shows;    /* show_texture calls (incl. NULL) */
   guint       u_partials; /* show_partial calls (a progressive loader's
                            * low-res stand-in; never counted as a show) */
   guint    u_clears;      /* show_texture(NULL) calls */
   guint    u_headers;     /* update_header calls */
   char    *c_status;      /* last status line */
   gboolean b_dead;        /* set after dispose: any call is a bug */
} FakeHost;

static void
_fh_show_texture(gpointer p_host, GdkTexture *p_tex) {
   FakeHost *p_h = (FakeHost *)p_host;
   g_assert_false(p_h->b_dead);
   g_set_object(&p_h->p_shown, p_tex);
   p_h->u_shows++;
   if (p_tex == NULL) {
      p_h->u_clears++;
   }
}

/* A partial is counted apart from the shows and never remembered as
 * p_shown: the window's host op treats it the same way (it is not the
 * file's picture, and a host that learns the original from what it showed
 * must not learn a low-res stand-in), so a pipeline that handed a partial
 * to show_texture instead would count it as a show here and fail the
 * partial assertions below. */
static void
_fh_show_partial(gpointer p_host, GdkTexture *p_tex) {
   FakeHost *p_h = (FakeHost *)p_host;
   g_assert_false(p_h->b_dead);
   g_assert_nonnull(p_tex);
   p_h->u_partials++;
}

static void
_fh_update_header(gpointer p_host) {
   FakeHost *p_h = (FakeHost *)p_host;
   g_assert_false(p_h->b_dead);
   p_h->u_headers++;
}

static void
_fh_show_status(gpointer p_host, const char *c_msg) {
   FakeHost *p_h = (FakeHost *)p_host;
   g_assert_false(p_h->b_dead);
   g_free(p_h->c_status);
   p_h->c_status = g_strdup(c_msg);
}

static const ViewLoadHostOps FAKE_OPS = {
   .show_texture  = _fh_show_texture,
   .show_partial  = _fh_show_partial,
   .update_header = _fh_update_header,
   .show_status   = _fh_show_status,
};

/* --- helpers ------------------------------------------------------------- */

static void
copy_fixture(const char *c_dir, const char *c_name, const char *c_as) {
   const gchar *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char   *c_src = g_build_filename(c_fx, c_name, NULL);
   char   *c_dst = g_build_filename(c_dir, c_as, NULL);
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
write_bytes(const char *c_dir, const char *c_name, const char *c_body) {
   char *c_path = g_build_filename(c_dir, c_name, NULL);
   g_assert_true(g_file_set_contents(c_path, c_body, -1, NULL));
   g_free(c_path);
}

static void
cleanup_temp_dir(char *c_dir) {
   GDir *p_d = g_dir_open(c_dir, 0, NULL);
   if (p_d != NULL) {
      const char *c_n;
      while ((c_n = g_dir_read_name(p_d)) != NULL) {
         char *c_p = g_build_filename(c_dir, c_n, NULL);
         g_unlink(c_p);
         g_free(c_p);
      }
      g_dir_close(p_d);
   }
   g_rmdir(c_dir);
   g_free(c_dir);
}

/* a.jpg, b.png, c.jpg: three decodable files in name order. */
static char *
make_folder(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-viewload-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg", "a.jpg");
   copy_fixture(c_dir, "small.png", "b.png");
   copy_fixture(c_dir, "rot6.jpg", "c.jpg");
   return (c_dir);
}

static void
pump(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(NULL, FALSE);
      g_usleep(1000);
   }
}

/* Pump until p_file's full decode landed in the cache (or 3 s). A JPEG's
 * progressive partial is shown first, so "shown once" is not "loaded". */
static void
pump_until_cached(ViewLoad *p_vl, GFile *p_file) {
   for (guint u = 0; u < 3000 && viewload_get_cached(p_vl, p_file) == NULL;
        u++) {
      g_main_context_iteration(NULL, FALSE);
      g_usleep(1000);
   }
}

/* Pump until the host saw u_clears show_texture(NULL) calls (or 3 s). */
static void
pump_until_clears(FakeHost *p_h, guint u_clears) {
   for (guint u = 0; u < 3000 && p_h->u_clears < u_clears; u++) {
      g_main_context_iteration(NULL, FALSE);
      g_usleep(1000);
   }
}

static void
fake_host_clear(FakeHost *p_h) {
   g_clear_object(&p_h->p_shown);
   g_clear_pointer(&p_h->c_status, g_free);
}

/* --- tests --------------------------------------------------------------- */

static void
test_miss_then_hit(void) {
   char      *c_dir = make_folder();
   GFile     *p_dir = g_file_new_for_path(c_dir);
   Navigator *p_nav = navigator_new(p_dir, GGAZE_SORT_NAME, FALSE, TRUE);
   FakeHost   st_h  = {0};
   ViewLoad  *p_vl  = viewload_new(&FAKE_OPS, &st_h, 4);
   viewload_set_navigator(p_vl, p_nav);

   viewload_load_current(p_vl); /* miss: async */
   g_assert_cmpuint(st_h.u_headers, ==, 1);
   pump_until_cached(p_vl, navigator_get_current(p_nav));
   g_assert_nonnull(st_h.p_shown);
   g_assert_nonnull(viewload_get_cached(p_vl, navigator_get_current(p_nav)));
   /* The JPEG backend's low-res first pass went through show_partial, and
    * what the host was left showing is the full decode -- the very object
    * the cache holds -- not the stand-in. Without the direct JPEG backend
    * (the minimal lane) GdkPixbuf decodes it in one go and shows none. */
#if GGAZE_HAVE_JPEG
   g_assert_cmpuint(st_h.u_partials, >=, 1);
#else
   g_assert_cmpuint(st_h.u_partials, ==, 0);
#endif
   g_assert_true(st_h.p_shown ==
                 viewload_get_cached(p_vl, navigator_get_current(p_nav)));

   guint u_before = st_h.u_shows;
   viewload_load_current(p_vl); /* hit: synchronous */
   g_assert_cmpuint(st_h.u_shows, ==, u_before + 1);
   pump(100); /* let the prefetch round finish */

   viewload_delete(p_vl);
   fake_host_clear(&st_h);
   navigator_delete(p_nav);
   g_object_unref(p_dir);
   cleanup_temp_dir(c_dir);
}

/* A PNG has no progressive pass: its load shows no partial, whatever the
 * lane, and what is shown is again the cached object. */
static void
test_png_shows_no_partial(void) {
   char      *c_dir = make_folder();
   GFile     *p_dir = g_file_new_for_path(c_dir);
   Navigator *p_nav = navigator_new(p_dir, GGAZE_SORT_NAME, FALSE, TRUE);
   FakeHost   st_h  = {0};
   ViewLoad  *p_vl  = viewload_new(&FAKE_OPS, &st_h, 4);
   viewload_set_navigator(p_vl, p_nav);

   g_assert_true(navigator_next(p_nav)); /* b.png */
   viewload_load_current(p_vl);
   pump_until_cached(p_vl, navigator_get_current(p_nav));
   g_assert_cmpuint(st_h.u_partials, ==, 0);
   g_assert_nonnull(st_h.p_shown);
   g_assert_true(st_h.p_shown ==
                 viewload_get_cached(p_vl, navigator_get_current(p_nav)));
   pump(100); /* the neighbour prefetch: no partials from it either */
   g_assert_cmpuint(st_h.u_partials, ==, 0);

   viewload_delete(p_vl);
   fake_host_clear(&st_h);
   navigator_delete(p_nav);
   g_object_unref(p_dir);
   cleanup_temp_dir(c_dir);
}

/* The result of a superseded load must never reach the viewer. */
static void
test_last_write_wins(void) {
   char      *c_dir = make_folder();
   GFile     *p_dir = g_file_new_for_path(c_dir);
   Navigator *p_nav = navigator_new(p_dir, GGAZE_SORT_NAME, FALSE, TRUE);
   FakeHost   st_h  = {0};
   ViewLoad  *p_vl  = viewload_new(&FAKE_OPS, &st_h, 4);
   viewload_set_navigator(p_vl, p_nav);

   viewload_load_current(p_vl); /* a.jpg starts loading */
   navigator_next(p_nav);       /* user moved on before it finished */
   viewload_load_current(p_vl); /* b.png starts; a.jpg cancelled */
   pump(500);
   /* Whatever is shown is b.png (the current), never a.jpg -- which may
    * still land in the cache, but only through b's neighbour prefetch. */
   g_assert_nonnull(st_h.p_shown);
   GFile *p_a = navigator_get_file(p_nav, 0);
   GFile *p_b = navigator_get_file(p_nav, 1);
   g_assert_nonnull(viewload_get_cached(p_vl, p_b));
   g_assert_true(st_h.p_shown == viewload_get_cached(p_vl, p_b));
   g_assert_true(st_h.p_shown != viewload_get_cached(p_vl, p_a));

   viewload_delete(p_vl);
   fake_host_clear(&st_h);
   navigator_delete(p_nav);
   g_object_unref(p_dir);
   cleanup_temp_dir(c_dir);
}

/* A current file that fails to decode clears the canvas and reports it. */
static void
test_failure_clears_and_reports(void) {
   char *c_dir = make_folder();
   write_bytes(c_dir, "bad.jpg", "\xff\xd8\xff garbage");
   GFile     *p_dir = g_file_new_for_path(c_dir);
   Navigator *p_nav = navigator_new(p_dir, GGAZE_SORT_NAME, FALSE, TRUE);
   FakeHost   st_h  = {0};
   ViewLoad  *p_vl  = viewload_new(&FAKE_OPS, &st_h, 4);
   viewload_set_navigator(p_vl, p_nav);

   viewload_load_current(p_vl); /* a.jpg */
   pump_until_cached(p_vl, navigator_get_current(p_nav));
   g_assert_nonnull(st_h.p_shown);
   char  *c_bad = g_build_filename(c_dir, "bad.jpg", NULL);
   GFile *p_bad = g_file_new_for_path(c_bad);
   g_assert_true(navigator_set_current_file(p_nav, p_bad));
   g_object_unref(p_bad);
   g_free(c_bad);
   viewload_load_current(p_vl);
   pump_until_clears(&st_h, 1);
   g_assert_cmpuint(st_h.u_clears, ==, 1);
   g_assert_null(st_h.p_shown);
   g_assert_nonnull(st_h.c_status);
   g_assert_true(g_str_has_prefix(st_h.c_status, "Cannot show bad.jpg"));

   viewload_delete(p_vl);
   fake_host_clear(&st_h);
   navigator_delete(p_nav);
   g_object_unref(p_dir);
   cleanup_temp_dir(c_dir);
}

/* A file rewritten in place (different size) is not served from the cache. */
static void
test_rewritten_file_is_reloaded(void) {
   char      *c_dir = make_folder();
   GFile     *p_dir = g_file_new_for_path(c_dir);
   Navigator *p_nav = navigator_new(p_dir, GGAZE_SORT_NAME, FALSE, TRUE);
   FakeHost   st_h  = {0};
   ViewLoad  *p_vl  = viewload_new(&FAKE_OPS, &st_h, 4);
   viewload_set_navigator(p_vl, p_nav);

   GFile *p_a = navigator_get_current(p_nav);
   viewload_load_current(p_vl);
   pump_until_cached(p_vl, p_a);
   GdkTexture *p_first = st_h.p_shown;
   g_assert_nonnull(p_first);
   g_object_ref(p_first);
   copy_fixture(c_dir, "rot6.jpg", "a.jpg");      /* rewrite in place */
   g_assert_null(viewload_get_cached(p_vl, p_a)); /* stale: evicted */
   viewload_load_current(p_vl);                   /* misses, reloads */
   pump_until_cached(p_vl, p_a);
   g_assert_nonnull(st_h.p_shown);
   g_assert_true(st_h.p_shown != p_first);
   g_object_unref(p_first);

   viewload_delete(p_vl);
   fake_host_clear(&st_h);
   navigator_delete(p_nav);
   g_object_unref(p_dir);
   cleanup_temp_dir(c_dir);
}

/* Dispose while a load is in flight: the host is never called again. */
static void
test_dispose_mid_load(void) {
   char      *c_dir = make_folder();
   GFile     *p_dir = g_file_new_for_path(c_dir);
   Navigator *p_nav = navigator_new(p_dir, GGAZE_SORT_NAME, FALSE, TRUE);
   FakeHost   st_h  = {0};
   ViewLoad  *p_vl  = viewload_new(&FAKE_OPS, &st_h, 4);
   viewload_set_navigator(p_vl, p_nav);

   viewload_load_current(p_vl);
   viewload_dispose(p_vl);
   st_h.b_dead = TRUE;
   pump(300); /* the cancelled load completes without touching the host */
   viewload_delete(p_vl);
   fake_host_clear(&st_h);
   navigator_delete(p_nav);
   g_object_unref(p_dir);
   cleanup_temp_dir(c_dir);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/viewload/miss_then_hit", test_miss_then_hit);
   g_test_add_func("/viewload/png_shows_no_partial", test_png_shows_no_partial);
   g_test_add_func("/viewload/last_write_wins", test_last_write_wins);
   g_test_add_func("/viewload/failure_clears_and_reports",
                   test_failure_clears_and_reports);
   g_test_add_func("/viewload/rewritten_file_is_reloaded",
                   test_rewritten_file_is_reloaded);
   g_test_add_func("/viewload/dispose_mid_load", test_dispose_mid_load);
   return (g_test_run());
}
