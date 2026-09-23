/*:*
 * ggaze — texture cache unit test
 *
 * Exercises the bounded LRU: capacity cap + eviction, MRU ordering on get,
 * replace, and miss; then the stamp that evicts an entry whose file changed
 * on disk -- size, whole seconds, the sub-second part of the mtime and the
 * inode each on their own, and the whole-second-filesystem fallback -- and
 * that the stamp an entry is put under is the one read BEFORE the decode
 * (a write landing mid-decode must not make the old pixels fresh). Uses
 * 1x1 GdkMemoryTextures (no display needed).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "texturecache.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>

#include "file_stamp.h"

static GdkTexture *
mk_tex(void) {
   static const guint8 u_px[4] = {0, 0, 0, 255};
   GBytes             *p_b     = g_bytes_new_static(u_px, 4);
   GdkTexture *p_t = gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8, p_b, 4);
   g_bytes_unref(p_b);
   return (p_t);
}

static void
test_cap_and_evict(void) {
   TextureCache *p_c      = texturecache_new(4);
   const char   *names[5] = {"a.jpg", "b.jpg", "c.jpg", "d.jpg", "e.jpg"};
   GFile        *f[5];
   GdkTexture   *t[5];
   for (gint i = 0; i < 5; i++) {
      f[i] = g_file_new_for_path(names[i]);
      t[i] = mk_tex();
      texturecache_put(p_c, f[i], t[i]);
   }
   g_assert_cmpint(texturecache_get_size(p_c), ==, 4); /* cap 4, 5 put */
   g_assert_null(texturecache_get(p_c, f[0]));         /* LRU evicted */
   g_assert_nonnull(texturecache_get(p_c, f[4]));      /* newest kept */

   for (gint i = 0; i < 5; i++) {
      g_object_unref(f[i]);
      g_object_unref(t[i]);
   }
   texturecache_delete(p_c);
}

static void
test_lru_order(void) {
   TextureCache *p_c = texturecache_new(2);
   GFile        *f1  = g_file_new_for_path("1.jpg");
   GFile        *f2  = g_file_new_for_path("2.jpg");
   GFile        *f3  = g_file_new_for_path("3.jpg");
   GdkTexture   *t1 = mk_tex(), *t2 = mk_tex(), *t3 = mk_tex();

   texturecache_put(p_c, f1, t1);
   texturecache_put(p_c, f2, t2);
   /* Touch f1 -> f2 becomes LRU. */
   g_assert_nonnull(texturecache_get(p_c, f1));
   texturecache_put(p_c, f3, t3); /* evicts LRU = f2 */
   g_assert_null(texturecache_get(p_c, f2));
   g_assert_nonnull(texturecache_get(p_c, f1));
   g_assert_nonnull(texturecache_get(p_c, f3));

   g_object_unref(f1);
   g_object_unref(f2);
   g_object_unref(f3);
   g_object_unref(t1);
   g_object_unref(t2);
   g_object_unref(t3);
   texturecache_delete(p_c);
}

static void
test_replace_and_miss(void) {
   TextureCache *p_c = texturecache_new(4);
   GFile        *f   = g_file_new_for_path("x.jpg");
   GdkTexture   *t1 = mk_tex(), *t2 = mk_tex();

   texturecache_put(p_c, f, t1);
   g_assert_cmpint(texturecache_get_size(p_c), ==, 1);
   texturecache_put(p_c, f, t2); /* replace, no size growth */
   g_assert_cmpint(texturecache_get_size(p_c), ==, 1);
   g_assert_true(texturecache_get(p_c, f) == t2);

   GFile *f_other = g_file_new_for_path("other.jpg");
   g_assert_null(texturecache_get(p_c, f_other)); /* miss */

   g_object_unref(f);
   g_object_unref(f_other);
   g_object_unref(t1);
   g_object_unref(t2);
   texturecache_delete(p_c);
}

/* --- stamp checks on a real file ---------------------------------------- */

/* A temp folder holding one file, a cache with its entry, the entry's
 * texture: what the stamp subtests share. */
typedef struct {
   char         *c_dir;
   char         *c_path;
   GFile        *p_file;
   TextureCache *p_cache;
   GdkTexture   *p_tex;
} FileFx;

static void
file_fx_open(FileFx *p_fx, const char *c_text) {
   GError *p_err = NULL;
   p_fx->c_dir   = g_dir_make_tmp("ggaze-tc-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   p_fx->c_path = g_build_filename(p_fx->c_dir, "img.bin", NULL);
   g_assert_true(g_file_set_contents(p_fx->c_path, c_text, -1, NULL));
   p_fx->p_file  = g_file_new_for_path(p_fx->c_path);
   p_fx->p_cache = texturecache_new(4);
   p_fx->p_tex   = mk_tex();
   texturecache_put(p_fx->p_cache, p_fx->p_file, p_fx->p_tex);
   g_assert_true(texturecache_get(p_fx->p_cache, p_fx->p_file) == p_fx->p_tex);
}

static void
file_fx_close(FileFx *p_fx) {
   g_object_unref(p_fx->p_tex);
   texturecache_delete(p_fx->p_cache);
   g_object_unref(p_fx->p_file);
   g_remove(p_fx->c_path);
   g_rmdir(p_fx->c_dir);
   g_free(p_fx->c_path);
   g_free(p_fx->c_dir);
}

/* Overwrite c_path in place -- truncate and write, the same inode -- unlike
 * g_file_set_contents, which writes a temp file and renames it over. */
static void
write_in_place(const char *c_path, const char *c_text) {
   FILE *p_fp = fopen(c_path, "wb");
   g_assert_nonnull(p_fp);
   g_assert_cmpint(fputs(c_text, p_fp), >=, 0);
   g_assert_cmpint(fclose(p_fp), ==, 0);
}

/* An entry for a real file is evicted once the file changes on disk (here
 * through g_file_set_contents, which changes the byte count AND, by its
 * rename over the original, the inode), so edits are never served stale;
 * a removed entry misses. */
static void
test_stale_entry_evicted(void) {
   FileFx fx;
   file_fx_open(&fx, "one");
   g_assert_true(g_file_set_contents(fx.c_path, "rewritten", -1, NULL));
   g_assert_null(texturecache_get(fx.p_cache, fx.p_file)); /* stale */
   g_assert_cmpuint(texturecache_get_size(fx.p_cache), ==, 0);
   texturecache_put(fx.p_cache, fx.p_file, fx.p_tex);
   texturecache_remove(fx.p_cache, fx.p_file);
   g_assert_null(texturecache_get(fx.p_cache, fx.p_file));
   file_fx_close(&fx);
}

/* The byte count on its own: an in-place rewrite (same inode) to another
 * size with the mtime restored to the nanosecond, so the size is the only
 * part of the stamp that differs. */
static void
test_size_only_change_evicted(void) {
   FileFx fx;
   file_fx_open(&fx, "one");
   GgtestFileStamp t_old, t_new;
   ggtest_read_stamp(fx.c_path, &t_old);
   write_in_place(fx.c_path, "longer");
   ggtest_set_mtime(fx.c_path, t_old.u_sec, t_old.u_nsec);
   ggtest_read_stamp(fx.c_path, &t_new);
   g_assert_cmpuint(t_new.u_inode, ==, t_old.u_inode); /* the premise: */
   g_assert_cmpuint(t_new.u_sec, ==, t_old.u_sec);     /* only the size */
   g_assert_cmpuint(t_new.u_nsec, ==, t_old.u_nsec);   /* moved */
   g_assert_cmpint(t_new.i_size, !=, t_old.i_size);
   g_assert_null(texturecache_get(fx.p_cache, fx.p_file)); /* stale */
   g_assert_cmpuint(texturecache_get_size(fx.p_cache), ==, 0);
   file_fx_close(&fx);
}

/* The fd2 case: a rewrite within the same second to the same byte count,
 * in place. Whole seconds + size read that as unchanged and the viewer
 * kept the stale decode; the sub-second part of the mtime tells it. The
 * new sub-second value is the old one moved by half a second, so it
 * differs whatever the clock did. */
static void
test_same_second_same_size_rewrite_evicted(void) {
   FileFx fx;
   file_fx_open(&fx, "one");
   GgtestFileStamp t_st;
   ggtest_read_stamp(fx.c_path, &t_st);
   write_in_place(fx.c_path, "two"); /* the same 3 bytes */
   ggtest_set_mtime(fx.c_path, t_st.u_sec,
                    (t_st.u_nsec + 500000000u) % 1000000000u);
   g_assert_null(texturecache_get(fx.p_cache, fx.p_file)); /* stale */
   g_assert_cmpuint(texturecache_get_size(fx.p_cache), ==, 0);
   file_fx_close(&fx);
}

/* A filesystem that records whole seconds only reads 0 below the second at
 * put and get alike (simulated here by setting the mtime to .000000000 on
 * both sides), so the seconds + size check stands on its own as it did:
 * the same stamp is a hit -- the one rewrite the cache cannot tell -- and
 * another second is a miss. The simulation needs a filesystem that DOES
 * keep sub-second mtimes (the first move to .0 must be a change), so a
 * whole-second one fails up front and says so. */
static void
test_whole_second_stamp_falls_back(void) {
   FileFx fx;
   file_fx_open(&fx, "one");
   GgtestFileStamp t_st;
   ggtest_read_stamp(fx.c_path, &t_st);
   ggtest_require_subsecond_mtime(fx.c_path, &t_st);
   ggtest_set_mtime(fx.c_path, t_st.u_sec, 0);
   g_assert_null(texturecache_get(fx.p_cache, fx.p_file)); /* moved */
   texturecache_put(fx.p_cache, fx.p_file, fx.p_tex);      /* stamp: .0 */
   write_in_place(fx.c_path, "two");
   ggtest_set_mtime(fx.c_path, t_st.u_sec, 0);
   g_assert_true(texturecache_get(fx.p_cache, fx.p_file) == fx.p_tex);
   ggtest_set_mtime(fx.c_path, t_st.u_sec - 1, 0);
   g_assert_null(texturecache_get(fx.p_cache, fx.p_file)); /* seconds */
   g_assert_cmpuint(texturecache_get_size(fx.p_cache), ==, 0);
   file_fx_close(&fx);
}

/* An atomic replace (an editor writing a temp file and renaming it over
 * the original: g_file_set_contents) with the mtime copied back and the
 * same byte count is told by the inode, which the rename changes. */
static void
test_replaced_inode_evicted(void) {
   FileFx fx;
   file_fx_open(&fx, "one");
   GgtestFileStamp t_old, t_new;
   ggtest_read_stamp(fx.c_path, &t_old);
   g_assert_true(g_file_set_contents(fx.c_path, "one", -1, NULL));
   ggtest_set_mtime(fx.c_path, t_old.u_sec, t_old.u_nsec);
   ggtest_read_stamp(fx.c_path, &t_new);
   g_assert_cmpuint(t_new.u_inode, !=, t_old.u_inode);     /* the premise */
   g_assert_null(texturecache_get(fx.p_cache, fx.p_file)); /* stale */
   g_assert_cmpuint(texturecache_get_size(fx.p_cache), ==, 0);
   file_fx_close(&fx);
}

/* --- the stamp is taken BEFORE the decode ------------------------------- */

/* The viewer's sequence with a writer finishing mid-decode: the miss hands
 * out the file's stamp (texturecache_lookup), the decode runs on the old
 * bytes while the file is rewritten, and the finished texture is put
 * under the PRE-decode stamp. The next get must miss -- the texture shows
 * the old file. A stamp read at put time (texturecache_put, what viewload
 * used to do) describes the NEW file and keeps the old pixels a hit for
 * good; the second half pins that difference so the test cannot pass
 * without the pre-decode stamp mattering. */
static void
test_prestamp_put_evicted_after_mid_decode_write(void) {
   FileFx fx;
   file_fx_open(&fx, "one");
   texturecache_remove(fx.p_cache, fx.p_file); /* start from a miss */
   TextureStamp t_pre;
   g_assert_null(texturecache_lookup(fx.p_cache, fx.p_file, &t_pre));
   g_assert_true(t_pre.b_valid);
   write_in_place(fx.c_path, "rewritten mid-decode"); /* the writer */
   texturecache_put_stamped(fx.p_cache, fx.p_file, fx.p_tex, &t_pre);
   g_assert_null(texturecache_get(fx.p_cache, fx.p_file)); /* stale */
   g_assert_cmpuint(texturecache_get_size(fx.p_cache), ==, 0);

   /* The post-decode stamp the fix replaced: the old pixels stay a hit. */
   texturecache_put(fx.p_cache, fx.p_file, fx.p_tex);
   g_assert_true(texturecache_get(fx.p_cache, fx.p_file) == fx.p_tex);
   file_fx_close(&fx);
}

/* A miss on a STALE entry hands out the stamp its freshness check read (no
 * second query), and a put under it is a hit while the file stays put; a
 * hit on an entry of an unqueryable file reads nothing (b_valid FALSE),
 * and a put with no stamp is trusted. */
static void
test_lookup_hands_out_stamp(void) {
   FileFx fx;
   file_fx_open(&fx, "one");
   write_in_place(fx.c_path, "two!");
   TextureStamp t_miss;
   g_assert_null(texturecache_lookup(fx.p_cache, fx.p_file, &t_miss));
   g_assert_true(t_miss.b_valid);
   g_assert_cmpint(t_miss.i_size, ==, 4);
   texturecache_put_stamped(fx.p_cache, fx.p_file, fx.p_tex, &t_miss);
   g_assert_true(texturecache_get(fx.p_cache, fx.p_file) == fx.p_tex);

   GFile *p_synth = g_file_new_for_path("/nonexistent/ggaze/x.jpg");
   texturecache_put_stamped(fx.p_cache, p_synth, fx.p_tex, NULL);
   TextureStamp t_hit;
   g_assert_true(texturecache_lookup(fx.p_cache, p_synth, &t_hit) == fx.p_tex);
   g_assert_false(t_hit.b_valid);
   g_object_unref(p_synth);
   file_fx_close(&fx);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/texturecache/cap_and_evict", test_cap_and_evict);
   g_test_add_func("/texturecache/lru_order", test_lru_order);
   g_test_add_func("/texturecache/replace_and_miss", test_replace_and_miss);
   g_test_add_func("/texturecache/stale_entry_evicted",
                   test_stale_entry_evicted);
   g_test_add_func("/texturecache/same_second_same_size_rewrite_evicted",
                   test_same_second_same_size_rewrite_evicted);
   g_test_add_func("/texturecache/whole_second_stamp_falls_back",
                   test_whole_second_stamp_falls_back);
   g_test_add_func("/texturecache/replaced_inode_evicted",
                   test_replaced_inode_evicted);
   g_test_add_func("/texturecache/size_only_change_evicted",
                   test_size_only_change_evicted);
   g_test_add_func("/texturecache/prestamp_put_evicted_after_mid_decode_write",
                   test_prestamp_put_evicted_after_mid_decode_write);
   g_test_add_func("/texturecache/lookup_hands_out_stamp",
                   test_lookup_hands_out_stamp);
   return (g_test_run());
}
