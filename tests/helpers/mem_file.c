/*:*
 * ggaze — a GFile served from memory (see mem_file.h)
 *
 * Two small GObject types: GgtestMemStream, a GFileInputStream over a
 * GBytes whose read_fn copies out of the buffer and may cancel a
 * GCancellable at EOF; and GgtestMemFile, a GFile whose read_fn hands out
 * one of those per open and counts the opens. Only the GFile vfuncs the
 * loader (and GIO on its behalf) actually calls are implemented.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "mem_file.h"

#include <string.h>

/* --- the stream ---------------------------------------------------------- */

typedef struct {
   GFileInputStream parent_instance;
   GBytes          *p_bytes;
   gsize            u_pos;
   GCancellable    *p_cancel_at_eof; /* NULL: a plain EOF */
} GgtestMemStream;

typedef struct {
   GFileInputStreamClass parent_class;
} GgtestMemStreamClass;

GType ggtest_mem_stream_get_type(void);
G_DEFINE_TYPE(GgtestMemStream, ggtest_mem_stream, G_TYPE_FILE_INPUT_STREAM)

/* Serve the next min(u_count, remaining) bytes. At EOF, cancel first and
 * report 0 second: GInputStream checked the cancellable BEFORE calling
 * this, so the caller's read succeeds with EOF and only a check of its
 * own after the read can notice the cancel -- which is the branch the
 * helper exists to reach. */
static gssize
_mem_stream_read(GInputStream *p_stream, void *p_buf, gsize u_count,
                 GCancellable *p_cancel, GError **p_err) {
   (void)p_cancel;
   (void)p_err;
   GgtestMemStream *p_s    = (GgtestMemStream *)p_stream;
   gsize            u_size = 0;
   const guint8    *p_data = g_bytes_get_data(p_s->p_bytes, &u_size);
   gsize            u_left = u_size - p_s->u_pos;
   if (u_left == 0) {
      if (p_s->p_cancel_at_eof != NULL) {
         g_cancellable_cancel(p_s->p_cancel_at_eof);
      }
      return (0);
   }
   gsize u_n = MIN(u_count, u_left);
   memcpy(p_buf, p_data + p_s->u_pos, u_n);
   p_s->u_pos += u_n;
   return ((gssize)u_n);
}

static void
_mem_stream_finalize(GObject *p_obj) {
   GgtestMemStream *p_s = (GgtestMemStream *)p_obj;
   g_clear_pointer(&p_s->p_bytes, g_bytes_unref);
   g_clear_object(&p_s->p_cancel_at_eof);
   G_OBJECT_CLASS(ggtest_mem_stream_parent_class)->finalize(p_obj);
}

static void
ggtest_mem_stream_class_init(GgtestMemStreamClass *p_class) {
   G_OBJECT_CLASS(p_class)->finalize      = _mem_stream_finalize;
   G_INPUT_STREAM_CLASS(p_class)->read_fn = _mem_stream_read;
   /* No close_fn: GInputStream treats that as a close that succeeds. */
}

static void
ggtest_mem_stream_init(GgtestMemStream *p_s) {
   (void)p_s;
}

/* --- the file ------------------------------------------------------------ */

typedef struct {
   GObject       parent_instance;
   GBytes       *p_bytes;
   GCancellable *p_cancel;     /* owned; what the armed stream cancels */
   guint         u_cancel_nth; /* which open's EOF cancels; 0: none */
   guint         u_opens;
} GgtestMemFile;

typedef struct {
   GObjectClass parent_class;
} GgtestMemFileClass;

static void _mem_file_iface_init(GFileIface *p_iface);

GType ggtest_mem_file_get_type(void);
G_DEFINE_TYPE_WITH_CODE(GgtestMemFile, ggtest_mem_file, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_FILE,
                                              _mem_file_iface_init))

static GFile *
_mem_file_new_from_bytes(GBytes *p_bytes) {
   GgtestMemFile *p_f = g_object_new(ggtest_mem_file_get_type(), NULL);
   p_f->p_bytes       = g_bytes_ref(p_bytes);
   return (G_FILE(p_f));
}

/* One stream per open, the u_cancel_nth-th one armed. */
static GFileInputStream *
_mem_file_read(GFile *p_file, GCancellable *p_cancel, GError **p_err) {
   (void)p_cancel;
   (void)p_err;
   GgtestMemFile   *p_f = (GgtestMemFile *)p_file;
   GgtestMemStream *p_s = g_object_new(ggtest_mem_stream_get_type(), NULL);
   p_s->p_bytes         = g_bytes_ref(p_f->p_bytes);
   p_f->u_opens++;
   if (p_f->u_cancel_nth != 0 && p_f->u_opens == p_f->u_cancel_nth) {
      p_s->p_cancel_at_eof = g_object_ref(p_f->p_cancel);
   }
   return (G_FILE_INPUT_STREAM(p_s));
}

static GFile *
_mem_file_dup(GFile *p_file) {
   return (_mem_file_new_from_bytes(((GgtestMemFile *)p_file)->p_bytes));
}

static guint
_mem_file_hash(GFile *p_file) {
   return (g_direct_hash(p_file));
}

static gboolean
_mem_file_equal(GFile *p_a, GFile *p_b) {
   return (p_a == p_b);
}

static gboolean
_mem_file_is_native(GFile *p_file) {
   (void)p_file;
   return (FALSE);
}

static char *
_mem_file_get_uri(GFile *p_file) {
   (void)p_file;
   return (g_strdup("ggtest-mem:///image"));
}

static char *
_mem_file_get_basename(GFile *p_file) {
   (void)p_file;
   return (g_strdup("image"));
}

/* No path: the loader's path-taking entry points must say "non-local". */
static char *
_mem_file_get_path(GFile *p_file) {
   (void)p_file;
   return (NULL);
}

static void
_mem_file_iface_init(GFileIface *p_iface) {
   p_iface->dup          = _mem_file_dup;
   p_iface->hash         = _mem_file_hash;
   p_iface->equal        = _mem_file_equal;
   p_iface->is_native    = _mem_file_is_native;
   p_iface->get_uri      = _mem_file_get_uri;
   p_iface->get_basename = _mem_file_get_basename;
   p_iface->get_path     = _mem_file_get_path;
   p_iface->read_fn      = _mem_file_read;
}

static void
_mem_file_finalize(GObject *p_obj) {
   GgtestMemFile *p_f = (GgtestMemFile *)p_obj;
   g_clear_pointer(&p_f->p_bytes, g_bytes_unref);
   g_clear_object(&p_f->p_cancel);
   G_OBJECT_CLASS(ggtest_mem_file_parent_class)->finalize(p_obj);
}

static void
ggtest_mem_file_class_init(GgtestMemFileClass *p_class) {
   G_OBJECT_CLASS(p_class)->finalize = _mem_file_finalize;
}

static void
ggtest_mem_file_init(GgtestMemFile *p_f) {
   (void)p_f;
}

/* --- public -------------------------------------------------------------- */

GFile *
ggtest_mem_file_new(const guint8 *p_bytes, gsize u_len) {
   GBytes *p_b    = g_bytes_new(p_bytes, u_len);
   GFile  *p_file = _mem_file_new_from_bytes(p_b);
   g_bytes_unref(p_b);
   return (p_file);
}

void
ggtest_mem_file_cancel_at_eof(GFile *p_file, GCancellable *p_cancel,
                              guint u_nth) {
   GgtestMemFile *p_f = (GgtestMemFile *)p_file;
   g_assert_true(
      G_TYPE_CHECK_INSTANCE_TYPE(p_file, ggtest_mem_file_get_type()));
   g_clear_object(&p_f->p_cancel);
   p_f->p_cancel     = (p_cancel != NULL) ? g_object_ref(p_cancel) : NULL;
   p_f->u_cancel_nth = u_nth;
}

guint
ggtest_mem_file_opens(GFile *p_file) {
   g_assert_true(
      G_TYPE_CHECK_INSTANCE_TYPE(p_file, ggtest_mem_file_get_type()));
   return (((GgtestMemFile *)p_file)->u_opens);
}
