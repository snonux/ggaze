/*:*
 * ggaze — info overlay + transient status line
 *
 * See info-overlay.h. The label, the auto-hide timer and the async info
 * request (info.c's info_new in a GTask worker) live here.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "info-overlay.h"

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

#include "info.h"

struct InfoOverlay {
   gatomicrefcount u_refs;
   GtkWidget      *p_label;     /* the overlay child (borrowed from GTK) */
   guint           u_hide;      /* auto-hide timeout id (0 = none) */
   GCancellable   *p_cancel;    /* outstanding async info_new() request */
   GFile          *p_info_file; /* file the visible card describes, or NULL */
   gboolean        b_disposed;  /* no widget touch after this */
};

static InfoOverlay *
_ref(InfoOverlay *p_io) {
   g_atomic_ref_count_inc(&p_io->u_refs);
   return (p_io);
}

static void
_unref(InfoOverlay *p_io) {
   if (g_atomic_ref_count_dec(&p_io->u_refs)) {
      g_clear_object(&p_io->p_cancel);
      g_clear_object(&p_io->p_info_file);
      g_free(p_io);
   }
}

InfoOverlay *
info_overlay_new(GtkOverlay *p_overlay) {
   g_return_val_if_fail(GTK_IS_OVERLAY(p_overlay), NULL);
   InfoOverlay *p_io = g_new0(InfoOverlay, 1);
   g_atomic_ref_count_init(&p_io->u_refs);
   p_io->p_label = gtk_label_new("");
   gtk_widget_add_css_class(p_io->p_label, "ggaze-info");
   gtk_label_set_wrap(GTK_LABEL(p_io->p_label), TRUE);
   gtk_label_set_max_width_chars(GTK_LABEL(p_io->p_label), 60);
   gtk_widget_set_halign(p_io->p_label, GTK_ALIGN_START);
   gtk_widget_set_valign(p_io->p_label, GTK_ALIGN_START);
   gtk_widget_set_margin_start(p_io->p_label, 12);
   gtk_widget_set_margin_top(p_io->p_label, 12);
   gtk_widget_set_visible(p_io->p_label, FALSE);
   gtk_overlay_add_overlay(p_overlay, p_io->p_label);
   return (p_io);
}

/* Cancel the auto-hide timer if one is pending. Every state change (a fresh
 * show, a status reusing the label, a navigation hide) calls this first so a
 * stale timer can never hide something it no longer owns. */
static void
_cancel_timer(InfoOverlay *p_io) {
   if (p_io->u_hide != 0) {
      g_source_remove(p_io->u_hide);
      p_io->u_hide = 0;
   }
}

/* Cancel any in-flight async info decode (navigation / dispose / a new `i`).
 * The cancelled task's completion is a no-op. */
static void
_cancel_async(InfoOverlay *p_io) {
   if (p_io->p_cancel != NULL) {
      g_cancellable_cancel(p_io->p_cancel);
      g_clear_object(&p_io->p_cancel);
   }
}

static void
_hide(InfoOverlay *p_io) {
   g_clear_object(&p_io->p_info_file);
   if (!p_io->b_disposed) {
      gtk_widget_set_visible(p_io->p_label, FALSE);
   }
}

static gboolean
_hide_tick(gpointer p_data) {
   InfoOverlay *p_io = (InfoOverlay *)p_data;
   /* Just fired: only zero the id (never g_source_remove our own source). */
   p_io->u_hide = 0;
   _hide(p_io);
   return (G_SOURCE_REMOVE);
}

/* Show c_text and arm the auto-hide for u_secs. */
static void
_show_text(InfoOverlay *p_io, const char *c_text, guint u_secs) {
   if (p_io->b_disposed) {
      return;
   }
   gtk_label_set_text(GTK_LABEL(p_io->p_label), c_text);
   gtk_widget_set_visible(p_io->p_label, TRUE);
   _cancel_timer(p_io);
   p_io->u_hide = g_timeout_add_seconds(u_secs, _hide_tick, p_io);
}

void
info_overlay_show_status(InfoOverlay *p_io, const char *c_msg) {
   g_return_if_fail(p_io != NULL);
   g_return_if_fail(c_msg != NULL);
   /* A short confirmation vanishes quickly; a long failure text gets time to
    * be read: 2 s + 1 s per 40 characters, capped at 8 s. */
   guint u_secs = (guint)CLAMP(2 + strlen(c_msg) / 40, 2, 8);
   g_clear_object(&p_io->p_info_file); /* a status is not a file card */
   _show_text(p_io, c_msg, u_secs);
}

/* GTask worker (off the main thread): build the GgazeInfo for the captured
 * file. Touches no GtkWidget. */
static void
_info_thread(GTask *p_task, gpointer p_src, gpointer p_task_data,
             GCancellable *p_cancel) {
   (void)p_src;
   GgazeInfo *p_info = info_new((GFile *)p_task_data);
   if (p_info == NULL) {
      g_task_return_new_error(p_task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "info: gather failed");
      return;
   }
   /* Superseded mid-decode: free the result here, because the completion is
    * a no-op on cancel and may never run before the loop drains. */
   if (g_cancellable_is_cancelled(p_cancel)) {
      info_delete(p_info);
      g_task_return_new_error(p_task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "info: cancelled");
      return;
   }
   g_task_return_pointer(p_task, p_info, (GDestroyNotify)info_delete);
}

/* GTask completion (main thread): apply the result iff not superseded and
 * the overlay is still live. p_data is the ref taken in show_for_file. */
static void
_info_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   InfoOverlay *p_io   = (InfoOverlay *)p_data;
   GError      *p_err  = NULL;
   GgazeInfo   *p_info = g_task_propagate_pointer(G_TASK(p_res), &p_err);
   g_clear_error(&p_err);
   if (p_info != NULL && !p_io->b_disposed) {
      char *c_text = info_format(p_info);
      _show_text(p_io, c_text, 5);
      g_free(c_text);
      g_set_object(&p_io->p_info_file,
                   (GFile *)g_task_get_task_data(G_TASK(p_res)));
   }
   g_clear_pointer(&p_info, info_delete);
   _unref(p_io);
}

void
info_overlay_show_for_file(InfoOverlay *p_io, GFile *p_file) {
   g_return_if_fail(p_io != NULL);
   g_return_if_fail(G_IS_FILE(p_file));
   if (p_io->b_disposed) {
      return;
   }
   _cancel_async(p_io);
   p_io->p_cancel = g_cancellable_new();
   /* The overlay is not a GObject: the task's source object is NULL and the
    * ref that keeps the struct alive for the callback is the plain refcount.
    */
   GTask *p_task = g_task_new(NULL, p_io->p_cancel, _info_done_cb, _ref(p_io));
   g_task_set_task_data(p_task, g_object_ref(p_file), g_object_unref);
   g_task_run_in_thread(p_task, _info_thread);
   g_object_unref(p_task);
}

void
info_overlay_toggle_for_file(InfoOverlay *p_io, GFile *p_file) {
   g_return_if_fail(p_io != NULL);
   g_return_if_fail(G_IS_FILE(p_file));
   if (p_io->p_info_file != NULL && g_file_equal(p_io->p_info_file, p_file) &&
       !p_io->b_disposed && gtk_widget_get_visible(p_io->p_label)) {
      info_overlay_dismiss(p_io);
      return;
   }
   info_overlay_show_for_file(p_io, p_file);
}

void
info_overlay_dismiss(InfoOverlay *p_io) {
   g_return_if_fail(p_io != NULL);
   _cancel_async(p_io);
   _cancel_timer(p_io);
   _hide(p_io);
}

GtkWidget *
info_overlay_get_label(InfoOverlay *p_io) {
   g_return_val_if_fail(p_io != NULL, NULL);
   return (p_io->p_label);
}

void
info_overlay_dispose(InfoOverlay *p_io) {
   if (p_io == NULL) {
      return;
   }
   _cancel_timer(p_io);
   _cancel_async(p_io);
   p_io->b_disposed = TRUE; /* GTK frees the label with its parent */
   p_io->p_label    = NULL;
}

void
info_overlay_delete(InfoOverlay *p_io) {
   if (p_io == NULL) {
      return;
   }
   info_overlay_dispose(p_io);
   _unref(p_io);
}
