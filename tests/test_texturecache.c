/*:*
 * ggaze — texture cache unit test
 *
 * Exercises the bounded LRU: capacity cap + eviction, MRU ordering on get,
 * replace, and miss; then the stamp that evicts an entry whose file changed
 * on disk -- size, whole seconds, the sub-second part of the mtime and the
 * inode each on their own, and the whole-second-filesystem fallback. Uses
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

/* c_path's mtime as the cache reads it: whole seconds and the sub-second
 * part in nanoseconds; and its inode. */
static void
read_stamp(const char *c_path, guint64 *p_sec, guint32 *p_nsec,
           guint64 *p_inode) {
   GFile     *p_f    = g_file_new_for_path(c_path);
   GFileInfo *p_info = g_file_query_info(p_f,
                                         G_FILE_ATTRIBUTE_TIME_MODIFIED
                                         "," G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC
                                         "," G_FILE_ATTRIBUTE_UNIX_INODE,
                                         G_FILE_QUERY_INFO_NONE, NULL, NULL);
   g_assert_nonnull(p_info);
   *p_sec =
      g_file_info_get_attribute_uint64(p_info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
   *p_nsec = g_file_info_get_attribute_uint32(
      p_info, G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC);
   *p_inode =
      g_file_info_get_attribute_uint64(p_info, G_FILE_ATTRIBUTE_UNIX_INODE);
   g_object_unref(p_info);
   g_object_unref(p_f);
}

/* Set c_path's mtime to u_sec + u_nsec, both parts in one call (setting the
 * seconds alone zeroes the sub-second part), and check it took: a
 * filesystem that keeps whole seconds only would make the sub-second
 * subtests below vacuous, so they say so instead of passing. */
static void
set_mtime(const char *c_path, guint64 u_sec, guint32 u_nsec) {
   GFile     *p_f    = g_file_new_for_path(c_path);
   GFileInfo *p_info = g_file_info_new();
   g_file_info_set_attribute_uint64(p_info, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                    u_sec);
   g_file_info_set_attribute_uint32(p_info, G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC,
                                    u_nsec);
   g_assert_true(g_file_set_attributes_from_info(
      p_f, p_info, G_FILE_QUERY_INFO_NONE, NULL, NULL));
   g_object_unref(p_info);
   g_object_unref(p_f);
   guint64 u_sec_now, u_inode;
   guint32 u_nsec_now;
   read_stamp(c_path, &u_sec_now, &u_nsec_now, &u_inode);
   g_assert_cmpuint(u_sec_now, ==, u_sec);
   g_assert_cmpuint(u_nsec_now, ==, u_nsec);
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

/* An entry for a real file is evicted once the file changes on disk (size
 * here), so in-place edits are never served stale; a synthetic path that
 * cannot be stat'ed is trusted as before. */
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

/* The fd2 case: a rewrite within the same second to the same byte count,
 * in place. Whole seconds + size read that as unchanged and the viewer
 * kept the stale decode; the sub-second part of the mtime tells it. The
 * new sub-second value is the old one moved by half a second, so it
 * differs whatever the clock did. */
static void
test_same_second_same_size_rewrite_evicted(void) {
   FileFx fx;
   file_fx_open(&fx, "one");
   guint64 u_sec, u_inode;
   guint32 u_nsec;
   read_stamp(fx.c_path, &u_sec, &u_nsec, &u_inode);
   write_in_place(fx.c_path, "two"); /* the same 3 bytes */
   set_mtime(fx.c_path, u_sec, (u_nsec + 500000000u) % 1000000000u);
   g_assert_null(texturecache_get(fx.p_cache, fx.p_file)); /* stale */
   g_assert_cmpuint(texturecache_get_size(fx.p_cache), ==, 0);
   file_fx_close(&fx);
}

/* A filesystem that records whole seconds only reads 0 below the second at
 * put and get alike (simulated here by setting the mtime to .000000000 on
 * both sides), so the seconds + size check stands on its own as it did:
 * the same stamp is a hit -- the one rewrite the cache cannot tell -- and
 * another second is a miss. */
static void
test_whole_second_stamp_falls_back(void) {
   FileFx fx;
   file_fx_open(&fx, "one");
   guint64 u_sec, u_inode;
   guint32 u_nsec;
   read_stamp(fx.c_path, &u_sec, &u_nsec, &u_inode);
   set_mtime(fx.c_path, u_sec, 0);
   g_assert_null(texturecache_get(fx.p_cache, fx.p_file)); /* moved */
   texturecache_put(fx.p_cache, fx.p_file, fx.p_tex);      /* stamp: .0 */
   write_in_place(fx.c_path, "two");
   set_mtime(fx.c_path, u_sec, 0);
   g_assert_true(texturecache_get(fx.p_cache, fx.p_file) == fx.p_tex);
   set_mtime(fx.c_path, u_sec - 1, 0);
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
   guint64 u_sec, u_inode, u_sec_now, u_inode_now;
   guint32 u_nsec, u_nsec_now;
   read_stamp(fx.c_path, &u_sec, &u_nsec, &u_inode);
   g_assert_true(g_file_set_contents(fx.c_path, "one", -1, NULL));
   set_mtime(fx.c_path, u_sec, u_nsec);
   read_stamp(fx.c_path, &u_sec_now, &u_nsec_now, &u_inode_now);
   g_assert_cmpuint(u_inode_now, !=, u_inode);             /* the premise */
   g_assert_null(texturecache_get(fx.p_cache, fx.p_file)); /* stale */
   g_assert_cmpuint(texturecache_get_size(fx.p_cache), ==, 0);
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
   return (g_test_run());
}
