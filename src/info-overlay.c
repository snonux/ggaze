/*:*
 * ggaze — info overlay + transient status line
 *
 * See info-overlay.h. The card box (label + histogram plot), the auto-hide
 * timer and the async gather (info.c's info_new plus histogram.c's texture
 * binning, both in one GTask worker) live here.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "info-overlay.h"

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

#include "ggaze-config.h"
#include "histogram-view.h"
#include "histogram.h"
#include "info.h"
#if GGAZE_HAVE_GEGL
#include "enhancer-gegl.h"
#endif

struct InfoOverlay {
   gatomicrefcount u_refs;
   GtkWidget      *p_box;       /* the card: overlay child (borrowed) */
   GtkWidget      *p_label;     /* text lines inside the card (borrowed) */
   GtkWidget      *p_hist;      /* GgazeHistogramView inside the card */
   guint           u_hide;      /* auto-hide timeout id (0 = none) */
   GCancellable   *p_cancel;    /* outstanding async gather */
   GFile          *p_info_file; /* file the visible card describes, or NULL */
   /* File of the in-flight FULL gather (text + plot), or NULL: a texture
    * change while it runs restarts it rather than racing it. */
   GFile *p_pending;
   /* Texture the plot shows or is being binned from, or NULL. A ref: the
    * pointer must stay unique for the identity check in texture_changed. */
   GdkTexture *p_plot_tex;
   gboolean    b_disposed; /* no widget touch after this */
};

/* One request: what the worker reads (file + the texture on screen for it,
 * may be NULL) and what it produces. b_hist_only is the texture-changed
 * follow-up: the card's text stays, only the plot is rebuilt. Task data, so
 * the task frees it whether the completion runs or the loop drains first. */
typedef struct {
   GFile      *p_file;
   GdkTexture *p_tex;       /* nullable */
   gboolean    b_hist_only; /* skip info_new: plot refresh for a live card */
   GgazeInfo  *p_info;      /* result, NULL until the worker ran / hist-only */
   Histogram  *p_hist;      /* result, NULL without a texture */
} InfoJob;

static InfoJob *
_infojob_new(GFile *p_file, GdkTexture *p_tex, gboolean b_hist_only) {
   InfoJob *p_job     = g_new0(InfoJob, 1);
   p_job->p_file      = g_object_ref(p_file);
   p_job->p_tex       = p_tex != NULL ? g_object_ref(p_tex) : NULL;
   p_job->b_hist_only = b_hist_only;
   return (p_job);
}

static void
_infojob_delete(InfoJob *p_job) {
   g_clear_object(&p_job->p_file);
   g_clear_object(&p_job->p_tex);
   g_clear_pointer(&p_job->p_info, info_delete);
   g_clear_pointer(&p_job->p_hist, histogram_delete);
   g_free(p_job);
}

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
      g_clear_object(&p_io->p_pending);
      g_clear_object(&p_io->p_plot_tex);
      g_free(p_io);
   }
}

/* The card is a vertical box carrying the "ggaze-info" backdrop, with the
 * text label above the histogram plot. The label wraps like the status line
 * always did; the plot is hidden until a histogram lands. */
static void
_build_card(InfoOverlay *p_io) {
   p_io->p_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_add_css_class(p_io->p_box, "ggaze-info");
   gtk_widget_set_halign(p_io->p_box, GTK_ALIGN_START);
   gtk_widget_set_valign(p_io->p_box, GTK_ALIGN_START);
   gtk_widget_set_margin_start(p_io->p_box, 12);
   gtk_widget_set_margin_top(p_io->p_box, 12);
   gtk_widget_set_visible(p_io->p_box, FALSE);

   p_io->p_label = gtk_label_new("");
   gtk_label_set_wrap(GTK_LABEL(p_io->p_label), TRUE);
   gtk_label_set_max_width_chars(GTK_LABEL(p_io->p_label), 60);
   gtk_label_set_xalign(GTK_LABEL(p_io->p_label), 0.0f);
   gtk_widget_set_visible(p_io->p_label, FALSE);
   gtk_box_append(GTK_BOX(p_io->p_box), p_io->p_label);

   p_io->p_hist = ggaze_histogram_view_new();
   gtk_widget_set_halign(p_io->p_hist, GTK_ALIGN_START);
   gtk_widget_set_visible(p_io->p_hist, FALSE);
   gtk_box_append(GTK_BOX(p_io->p_box), p_io->p_hist);
}

InfoOverlay *
info_overlay_new(GtkOverlay *p_overlay) {
   g_return_val_if_fail(GTK_IS_OVERLAY(p_overlay), NULL);
   InfoOverlay *p_io = g_new0(InfoOverlay, 1);
   g_atomic_ref_count_init(&p_io->u_refs);
   _build_card(p_io);
   gtk_overlay_add_overlay(p_overlay, p_io->p_box);
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

/* Cancel any in-flight async gather (navigation / dispose / a new request).
 * The cancelled task's completion is a no-op, and nothing is pending any
 * more: a texture change after a dismissed `i` must not resurrect the
 * dismissed card by restarting its gather. */
static void
_cancel_async(InfoOverlay *p_io) {
   if (p_io->p_cancel != NULL) {
      g_cancellable_cancel(p_io->p_cancel);
      g_clear_object(&p_io->p_cancel);
   }
   g_clear_object(&p_io->p_pending);
}

/* Hand p_hist (transfer full, may be NULL) to the plot and show it iff there
 * is one. A status line and a card for a file with no displayed texture both
 * pass NULL, so a stale plot can never sit under unrelated text. */
static void
_set_histogram(InfoOverlay *p_io, Histogram *p_hist) {
   if (p_io->b_disposed) {
      histogram_delete(p_hist);
      return;
   }
   ggaze_histogram_view_set_histogram(GGAZE_HISTOGRAM_VIEW(p_io->p_hist),
                                      p_hist);
   gtk_widget_set_visible(p_io->p_hist, p_hist != NULL);
}

/* Hide the card. The label's own visible flag is kept in step with the box
 * so callers (and the existing tests) asking the label "are you up?" keep
 * getting the card's answer, as they did when the label was the card. */
static void
_hide(InfoOverlay *p_io) {
   g_clear_object(&p_io->p_info_file);
   g_clear_object(&p_io->p_plot_tex); /* the next card starts plot-free */
   if (!p_io->b_disposed) {
      gtk_widget_set_visible(p_io->p_box, FALSE);
      gtk_widget_set_visible(p_io->p_label, FALSE);
      _set_histogram(p_io, NULL);
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
   gtk_widget_set_visible(p_io->p_box, TRUE);
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
   g_clear_object(&p_io->p_plot_tex);  /* ... carries no plot ... */
   _set_histogram(p_io, NULL);         /* ... and texture changes skip it */
   _show_text(p_io, c_msg, u_secs);
}

/* GTask worker (off the main thread): fill the job's results -- the
 * GgazeInfo for the file (unless the job only refreshes the plot) and, when
 * a texture came along, its histogram. Touches no GtkWidget; the texture is
 * immutable so reading it here is safe. */
static void
_info_thread(GTask *p_task, gpointer p_src, gpointer p_task_data,
             GCancellable *p_cancel) {
   (void)p_src;
   InfoJob *p_job = (InfoJob *)p_task_data;
   if (!p_job->b_hist_only) {
      p_job->p_info = info_new(p_job->p_file);
      if (p_job->p_info == NULL) {
         g_task_return_new_error(p_task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "info: gather failed");
         return;
      }
#if GGAZE_HAVE_GEGL
      /* The card's "managed on enhance/export" note, only when true: the
       * enhancer's own gates, asked header-deep (info.h b_icc_managed). */
      p_job->p_info->b_icc_managed =
         p_job->p_info->e_icc == GGAZE_ICC_EMBEDDED &&
         enhancer_would_manage(p_job->p_file);
#endif
   }
   /* Skip the binning if superseded while the metadata was gathering: the
    * completion is a no-op on cancel and the job dies with the task. */
   if (g_cancellable_is_cancelled(p_cancel)) {
      g_task_return_new_error(p_task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "info: cancelled");
      return;
   }
   if (p_job->p_tex != NULL) {
      p_job->p_hist = histogram_new_from_texture(p_job->p_tex);
   }
   g_task_return_boolean(p_task, TRUE);
}

/* Apply a landed job to the card. A full gather shows its text (fresh
 * auto-hide timer) and marks the card as p_file's; a plot-only refresh
 * changes nothing but the plot, and only if the card still IS that file's
 * -- the auto-hide timer may have taken the card down while the binning
 * ran, and a plot must not come up on a hidden card. */
static void
_apply_job(InfoOverlay *p_io, InfoJob *p_job) {
   if (!p_job->b_hist_only) {
      char *c_text = info_format(p_job->p_info);
      _show_text(p_io, c_text, 5);
      g_free(c_text);
      g_set_object(&p_io->p_info_file, p_job->p_file);
   } else if (p_io->p_info_file == NULL ||
              !g_file_equal(p_io->p_info_file, p_job->p_file)) {
      return;
   }
   _set_histogram(p_io, g_steal_pointer(&p_job->p_hist));
}

/* GTask completion (main thread): apply the result iff not superseded and
 * the overlay is still live. p_data is the ref taken in _start_job. The
 * histogram moves out of the job into the plot widget; the rest stays with
 * the job and is freed with the task. The live request (its cancellable is
 * the current one) is over either way, success or failure: a full gather
 * that is done is no longer pending, so a later texture change refreshes
 * the plot instead of restarting the text gather. A superseded request
 * (cancelled: b_ok FALSE, another cancellable current) changes nothing. */
static void
_info_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   InfoOverlay *p_io  = (InfoOverlay *)p_data;
   GError      *p_err = NULL;
   gboolean     b_ok  = g_task_propagate_boolean(G_TASK(p_res), &p_err);
   g_clear_error(&p_err);
   if (g_task_get_cancellable(G_TASK(p_res)) == p_io->p_cancel) {
      g_clear_object(&p_io->p_pending);
   }
   if (b_ok && !p_io->b_disposed) {
      _apply_job(p_io, (InfoJob *)g_task_get_task_data(G_TASK(p_res)));
   }
   _unref(p_io);
}

/* Start one async request (full gather or plot-only), superseding any in
 * flight: the previous cancellable is cancelled, so its completion is a
 * no-op and the last request issued is the one that lands (last-write-
 * wins). p_tex becomes the texture the plot is for, whether or not it is
 * NULL, so a repeat texture change for the same texture is recognised. A
 * full gather is pending until its completion runs; a plot refresh never
 * is (the card is already up). p_file is ref'd before the cancel, which
 * drops the previous pending file it may well be. */
static void
_start_job(InfoOverlay *p_io, GFile *p_file, GdkTexture *p_tex,
           gboolean b_hist_only) {
   g_object_ref(p_file);
   _cancel_async(p_io);
   p_io->p_cancel = g_cancellable_new();
   g_set_object(&p_io->p_plot_tex, p_tex);
   if (!b_hist_only) {
      g_set_object(&p_io->p_pending, p_file);
   }
   /* The overlay is not a GObject: the task's source object is NULL and the
    * ref that keeps the struct alive for the callback is the plain refcount.
    */
   GTask *p_task = g_task_new(NULL, p_io->p_cancel, _info_done_cb, _ref(p_io));
   g_task_set_task_data(p_task, _infojob_new(p_file, p_tex, b_hist_only),
                        (GDestroyNotify)_infojob_delete);
   g_task_run_in_thread(p_task, _info_thread);
   g_object_unref(p_task);
   g_object_unref(p_file);
}

void
info_overlay_show_for_file(InfoOverlay *p_io, GFile *p_file,
                           GdkTexture *p_tex) {
   g_return_if_fail(p_io != NULL);
   g_return_if_fail(G_IS_FILE(p_file));
   g_return_if_fail(p_tex == NULL || GDK_IS_TEXTURE(p_tex));
   if (p_io->b_disposed) {
      return;
   }
   _start_job(p_io, p_file, p_tex, FALSE);
}

void
info_overlay_texture_changed(InfoOverlay *p_io, GdkTexture *p_tex) {
   g_return_if_fail(p_io != NULL);
   g_return_if_fail(p_tex == NULL || GDK_IS_TEXTURE(p_tex));
   if (p_io->b_disposed || p_tex == p_io->p_plot_tex) {
      return; /* nothing to follow, or already plotting this very texture */
   }
   if (p_io->p_pending != NULL) {
      /* The card's own gather is still running with the previous texture
       * (or none): restart it with what is on screen now, so the card
       * comes up plotting the picture it sits over. */
      _start_job(p_io, p_io->p_pending, p_tex, FALSE);
      return;
   }
   if (p_io->p_info_file == NULL) {
      return; /* a status line or a hidden card has no plot to follow */
   }
   /* A live file card: drop the plot of the picture that is gone at once
    * (never a stale plot under the new picture, not even for the binning's
    * duration) and re-bin the new one off the main thread. The card's text
    * and its auto-hide timer are untouched: the file is the same. */
   _set_histogram(p_io, NULL);
   if (p_tex == NULL) {
      _cancel_async(p_io); /* an older binning must not land either */
      g_clear_object(&p_io->p_plot_tex);
      return;
   }
   _start_job(p_io, p_io->p_info_file, p_tex, TRUE);
}

void
info_overlay_toggle_for_file(InfoOverlay *p_io, GFile *p_file,
                             GdkTexture *p_tex) {
   g_return_if_fail(p_io != NULL);
   g_return_if_fail(G_IS_FILE(p_file));
   if (p_io->p_info_file != NULL && g_file_equal(p_io->p_info_file, p_file) &&
       !p_io->b_disposed && gtk_widget_get_visible(p_io->p_box)) {
      info_overlay_dismiss(p_io);
      return;
   }
   info_overlay_show_for_file(p_io, p_file, p_tex);
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

GtkWidget *
info_overlay_get_histogram(InfoOverlay *p_io) {
   g_return_val_if_fail(p_io != NULL, NULL);
   return (p_io->p_hist);
}

void
info_overlay_dispose(InfoOverlay *p_io) {
   if (p_io == NULL) {
      return;
   }
   _cancel_timer(p_io);
   _cancel_async(p_io);
   p_io->b_disposed = TRUE; /* GTK frees the card with its parent */
   p_io->p_box      = NULL;
   p_io->p_label    = NULL;
   p_io->p_hist     = NULL;
}

void
info_overlay_delete(InfoOverlay *p_io) {
   if (p_io == NULL) {
      return;
   }
   info_overlay_dispose(p_io);
   _unref(p_io);
}
