/*:*
 * ggaze — main window
 *
 * GgazeWindow : GtkApplicationWindow owns an AdwHeaderBar + a GtkStack with two
 * children ("grid" placeholder until M7, "large" = the GgazeViewer). M2 adds a
 * Navigator over the current folder, a single GCancellable (last-write-wins),
 * keybinding->action shortcuts, and a file/folder drop target. The header
 * title carries "filename · n/total". See docs/architecture.md.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "window.h"

#include <adwaita.h>
#include <glib.h>
#include <gtk/gtk.h>

#include "gridview.h"
#include "info.h"
#include "loader/loader.h"
#include "navigator.h"
#include "shortcuts.h"
#include "texturecache.h"
#include "thumbnail.h"
#include "trash.h"
#include "viewer.h"

struct _GgazeWindow {
   GtkApplicationWindow parent_instance;
   Navigator           *p_nav;    /* current folder listing (NULL until open) */
   GCancellable        *p_cancel; /* visible load; cancelled on each nav */
   GCancellable *p_prefetch_cancel; /* prefetch round; cancelled on new round */
   TextureCache *p_cache;           /* bounded LRU of decoded GdkTextures */
   Thumbnail    *p_thumb;           /* TMS thumbnail cache */
   Trash        *p_trash;           /* ./Trash bin for the current folder */
   GtkWidget    *p_stack;           /* GtkStack: grid / large (viewer) */
   GtkWidget    *p_viewer;          /* GgazeViewer — the large view */
   GgazeGrid    *p_grid;      /* the thumbnail grid (the "grid" stack child) */
   int           i_grid_size; /* current thumbnail size (64-512, decision T) */
   GtkWidget    *p_overlay; /* GtkOverlay wrapping the stack (for info label) */
   GtkWidget    *p_info_lbl;  /* info overlay label (auto-hides) */
   guint         u_info_hide; /* info auto-hide timeout id (0=none) */
   guint         u_slideshow; /* slideshow timeout id (0=off) */
   gboolean      b_fullscreen;
   guint         u_hdr_hide; /* fullscreen header auto-hide timeout */
};

G_DEFINE_TYPE(GgazeWindow, ggaze_window, GTK_TYPE_APPLICATION_WINDOW)

/* --- forward decls ------------------------------------------------------- */
static void     _load_current(GgazeWindow *p_win);
static void     _prefetch(GgazeWindow *p_win);
static void     _update_header(GgazeWindow *p_win);
static void     _on_grid_activate(GgazeGrid *p_grid, gpointer p_data);
static void     _show_info(GgazeWindow *p_win);
static void     _hide_info(GgazeWindow *p_win);
static gboolean _slideshow_tick(gpointer p_data);

/* --- actions ------------------------------------------------------------- */

static void
_action_prev(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   ggaze_window_prev(GGAZE_WINDOW(p_data));
}

static void
_action_next(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   ggaze_window_next(GGAZE_WINDOW(p_data));
}

static void
_action_first(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   ggaze_window_first(GGAZE_WINDOW(p_data));
}

static void
_action_last(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   ggaze_window_last(GGAZE_WINDOW(p_data));
}

static void
_action_quit(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   gtk_window_close(GTK_WINDOW(p_data));
}

static void
_open_dialog_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   GtkFileDialog *p_dlg  = GTK_FILE_DIALOG(p_src);
   GError        *p_err  = NULL;
   GFile         *p_file = gtk_file_dialog_open_finish(p_dlg, p_res, &p_err);
   if (p_file != NULL) {
      ggaze_window_open(GGAZE_WINDOW(p_data), p_file);
      g_object_unref(p_file);
   } else if (p_err != NULL) {
      if (!g_error_matches(p_err, GTK_DIALOG_ERROR,
                           GTK_DIALOG_ERROR_DISMISSED)) {
         g_warning("ggaze: open dialog failed: %s", p_err->message);
      }
      g_error_free(p_err);
   }
   g_object_unref(p_data);
}

static void
_action_open(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow   *p_win = GGAZE_WINDOW(p_data);
   GtkFileDialog *p_dlg = gtk_file_dialog_new();
   gtk_file_dialog_set_title(p_dlg, "Open image");
   gtk_file_dialog_open(p_dlg, GTK_WINDOW(p_win), NULL, _open_dialog_cb,
                        g_object_ref(p_win));
   g_object_unref(p_dlg);
}

/* --- M7: trash / delete / undo / view toggle / resize ------------------- */

static void
_action_trash(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL || p_win->p_trash == NULL) {
      return;
   }
   GFile *p_cur = navigator_get_current(p_win->p_nav);
   if (p_cur == NULL) {
      return;
   }
   GError *p_err = NULL;
   if (trash_bin(p_win->p_trash, p_cur, &p_err)) {
      navigator_mark_removed(p_win->p_nav, p_cur); /* dim; emits changed */
      navigator_next(p_win->p_nav);                /* advance; emits changed */
   } else {
      g_warning("ggaze: trash failed: %s", p_err->message);
      g_clear_error(&p_err);
   }
}

/* Permanently delete each file in p_files (the current or the marked set). */
static void
_do_delete_files(GgazeWindow *p_win, GList *p_files) {
   for (GList *p_it = p_files; p_it != NULL; p_it = p_it->next) {
      GFile  *p_f   = G_FILE(p_it->data);
      GError *p_err = NULL;
      if (trash_permanently_delete(p_win->p_trash, p_f, &p_err)) {
         navigator_mark_removed(p_win->p_nav, p_f);
      } else {
         g_warning("ggaze: delete failed: %s", p_err->message);
         g_clear_error(&p_err);
      }
   }
   navigator_next(p_win->p_nav); /* advance off the last deleted */
}

static void
_delete_confirm_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   GtkAlertDialog *p_dlg = GTK_ALERT_DIALOG(p_src);
   GgazeWindow    *p_win = GGAZE_WINDOW(p_data);
   GError         *p_err = NULL;
   gboolean        b_ok  = gtk_alert_dialog_choose_finish(p_dlg, p_res, &p_err);
   if (b_ok && p_win->p_nav != NULL) {
      GList *p_marks = navigator_get_marks(p_win->p_nav);
      _do_delete_files(p_win, p_marks);
      g_list_free_full(p_marks, (GDestroyNotify)g_object_unref);
   } else {
      g_clear_error(&p_err);
   }
   g_object_unref(p_data);
}

static void
_action_delete(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL || p_win->p_trash == NULL) {
      return;
   }
   guint u_marks = navigator_get_mark_count(p_win->p_nav);
   if (u_marks > 1) {
      /* Confirm before deleting >1 marked image (decision #38). */
      char *c_msg =
         g_strdup_printf("Permanently delete %u marked images?", u_marks);
      GtkAlertDialog *p_dlg =
         gtk_alert_dialog_new("Permanently delete %u marked images?", u_marks);
      gtk_alert_dialog_set_buttons(p_dlg,
                                   (const char *[]){"Cancel", "Delete", NULL});
      gtk_alert_dialog_choose(p_dlg, GTK_WINDOW(p_win), NULL,
                              _delete_confirm_cb, g_object_ref(p_win));
      g_object_unref(p_dlg);
      g_free(c_msg);
      return;
   }
   GList *p_files = NULL;
   if (u_marks == 1) {
      p_files = navigator_get_marks(p_win->p_nav);
   } else {
      GFile *p_cur = navigator_get_current(p_win->p_nav);
      if (p_cur != NULL) {
         p_files = g_list_prepend(NULL, g_object_ref(p_cur));
      }
   }
   _do_delete_files(p_win, p_files);
   g_list_free_full(p_files, (GDestroyNotify)g_object_unref);
}

static void
_action_undo(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_trash == NULL) {
      return;
   }
   GError *p_err = NULL;
   if (trash_restore_last(p_win->p_trash, &p_err)) {
      navigator_rescan(p_win->p_nav); /* re-list; restored file un-removed */
   } else {
      g_clear_error(&p_err);
   }
}

static void
_action_toggle_view(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   const char  *c_cur =
      gtk_stack_get_visible_child_name(GTK_STACK(p_win->p_stack));
   if (g_strcmp0(c_cur, "large") == 0) {
      gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack), "grid");
   } else {
      gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack), "large");
   }
}

static void
_action_zoom_in(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   const char  *c_cur =
      gtk_stack_get_visible_child_name(GTK_STACK(p_win->p_stack));
   if (g_strcmp0(c_cur, "large") == 0) {
      ggaze_viewer_zoom_in(GGAZE_VIEWER(p_win->p_viewer));
   } else if (p_win->p_grid != NULL) {
      int i_sz           = CLAMP(p_win->i_grid_size + 32, 64, 512);
      p_win->i_grid_size = i_sz;
      ggaze_grid_set_thumbnail_size(p_win->p_grid, i_sz);
   }
}

static void
_action_zoom_out(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   const char  *c_cur =
      gtk_stack_get_visible_child_name(GTK_STACK(p_win->p_stack));
   if (g_strcmp0(c_cur, "large") == 0) {
      ggaze_viewer_zoom_out(GGAZE_VIEWER(p_win->p_viewer));
   } else if (p_win->p_grid != NULL) {
      int i_sz           = CLAMP(p_win->i_grid_size - 32, 64, 512);
      p_win->i_grid_size = i_sz;
      ggaze_grid_set_thumbnail_size(p_win->p_grid, i_sz);
   }
}

/* --- M4: fullscreen / slideshow / info / back --------------------------- */

static void
_action_fullscreen(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->b_fullscreen) {
      gtk_window_unfullscreen(GTK_WINDOW(p_win));
      p_win->b_fullscreen = FALSE;
   } else {
      gtk_window_fullscreen(GTK_WINDOW(p_win));
      p_win->b_fullscreen = TRUE;
   }
}

static void
_action_slideshow(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->u_slideshow != 0) {
      g_source_remove(p_win->u_slideshow);
      p_win->u_slideshow = 0;
   } else {
      /* 3-second default; GSettings slideshow-delay wired in M10 */
      p_win->u_slideshow = g_timeout_add_seconds(3, _slideshow_tick, p_win);
   }
}

static void
_action_info(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _show_info(GGAZE_WINDOW(p_data));
}

static void
_action_back(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->b_fullscreen) {
      gtk_window_unfullscreen(GTK_WINDOW(p_win));
      p_win->b_fullscreen = FALSE;
   } else {
      const char *c_cur =
         gtk_stack_get_visible_child_name(GTK_STACK(p_win->p_stack));
      if (g_strcmp0(c_cur, "large") == 0) {
         gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack), "grid");
      } else {
         gtk_window_close(GTK_WINDOW(p_win));
      }
   }
}

static gboolean
_slideshow_tick(gpointer p_data) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav != NULL) {
      navigator_next(p_win->p_nav);
   }
   return (G_SOURCE_CONTINUE);
}

static gboolean
_info_hide_tick(gpointer p_data) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   _hide_info(p_win);
   p_win->u_info_hide = 0;
   return (G_SOURCE_REMOVE);
}

static void
_show_info(GgazeWindow *p_win) {
   if (p_win->p_nav == NULL) {
      return;
   }
   GFile *p_cur = navigator_get_current(p_win->p_nav);
   if (p_cur == NULL) {
      return;
   }
   GgazeInfo *p_info = info_new(p_cur);
   if (p_info == NULL) {
      return;
   }
   char *c_text = info_format(p_info);
   gtk_label_set_text(GTK_LABEL(p_win->p_info_lbl), c_text);
   g_free(c_text);
   info_delete(p_info);
   gtk_widget_set_visible(p_win->p_info_lbl, TRUE);
   if (p_win->u_info_hide != 0) {
      g_source_remove(p_win->u_info_hide);
   }
   p_win->u_info_hide = g_timeout_add_seconds(5, _info_hide_tick, p_win);
}

static void
_hide_info(GgazeWindow *p_win) {
   gtk_widget_set_visible(p_win->p_info_lbl, FALSE);
}

static const GActionEntry ACTIONS[] = {
   {.name = "prev", .activate = _action_prev},
   {.name = "next", .activate = _action_next},
   {.name = "first", .activate = _action_first},
   {.name = "last", .activate = _action_last},
   {.name = "open", .activate = _action_open},
   {.name = "quit", .activate = _action_quit},
   {.name = "trash", .activate = _action_trash},
   {.name = "delete", .activate = _action_delete},
   {.name = "undo", .activate = _action_undo},
   {.name = "toggle-view", .activate = _action_toggle_view},
   {.name = "zoom-in", .activate = _action_zoom_in},
   {.name = "zoom-out", .activate = _action_zoom_out},
   {.name = "fullscreen", .activate = _action_fullscreen},
   {.name = "slideshow", .activate = _action_slideshow},
   {.name = "info", .activate = _action_info},
   {.name = "back", .activate = _action_back},
};

/* --- drop target --------------------------------------------------------- */

static gboolean
drop_cb(GtkDropTarget *p_t, const GValue *p_val, gdouble d_x, gdouble d_y,
        gpointer p_data) {
   (void)p_t;
   (void)d_x;
   (void)d_y;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (!G_VALUE_HOLDS(p_val, GDK_TYPE_FILE_LIST)) {
      return (FALSE);
   }
   GdkFileList *p_fl    = (GdkFileList *)g_value_get_boxed(p_val);
   GSList      *p_files = gdk_file_list_get_files(p_fl);
   /* Decision Z: many files -> first file's folder with the first current. */
   if (p_files != NULL) {
      ggaze_window_open(p_win, G_FILE(p_files->data));
      return (TRUE);
   }
   return (FALSE);
}

/* --- navigator changed -> reload ----------------------------------------- */

static void
nav_changed_cb(Navigator *p_nav, gpointer p_data) {
   (void)p_nav;
   _load_current(GGAZE_WINDOW(p_data));
}

static void
_on_grid_activate(GgazeGrid *p_grid, gpointer p_data) {
   (void)p_grid;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack), "large");
   _load_current(p_win);
}

/* --- load current into the viewer ---------------------------------------- */

static void
_show_texture(GgazeWindow *p_win, GdkTexture *p_tex) {
   ggaze_viewer_set_texture(GGAZE_VIEWER(p_win->p_viewer), p_tex);
   gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack), "large");
}

/* Prefetch callback: just cache the result (never touches the viewer). p_data
 * is a ref on the window (released here) so the window outlives the load. */
static void
_prefetch_finish_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   GError      *p_err = NULL;
   GdkTexture  *p_tex = loader_load_finish(p_res, &p_err);
   if (p_tex != NULL) {
      GFile *p_file = (GFile *)g_task_get_source_object((GTask *)p_res);
      texturecache_put(p_win->p_cache, p_file, p_tex);
      g_object_unref(p_tex);
   } else {
      g_clear_error(&p_err);
   }
   g_object_unref(p_win);
}

/* Visible-load callback: show only if this is still the current file
 * (last-write-wins), then cache it and prefetch neighbours. */
static void
_load_finish_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   GError      *p_err = NULL;
   GdkTexture  *p_tex = loader_load_finish(p_res, &p_err);
   if (p_tex == NULL) {
      if (p_err != NULL &&
          !g_error_matches(p_err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
         char *c_name = g_file_get_basename(
            (GFile *)g_task_get_source_object((GTask *)p_res));
         g_warning("ggaze: failed to load %s: %s", c_name, p_err->message);
         g_free(c_name);
      }
      g_clear_error(&p_err);
      g_object_unref(p_win);
      return;
   }
   GFile *p_loaded = (GFile *)g_task_get_source_object((GTask *)p_res);
   GFile *p_cur    = navigator_get_current(p_win->p_nav);
   if (p_cur != NULL && g_file_equal(p_cur, p_loaded)) {
      _show_texture(p_win, p_tex);
      texturecache_put(p_win->p_cache, p_loaded, p_tex);
      _prefetch(p_win);
   }
   g_object_unref(p_tex);
   g_object_unref(p_win);
}

/* Prefetch the next/previous images into the cache (not shown). Cancels the
 * previous prefetch round so at most two prefetch loads are in flight. */
static void
_prefetch(GgazeWindow *p_win) {
   if (p_win->p_nav == NULL) {
      return;
   }
   g_cancellable_cancel(p_win->p_prefetch_cancel);
   g_clear_object(&p_win->p_prefetch_cancel);
   p_win->p_prefetch_cancel = g_cancellable_new();

   gint  i_idx = navigator_get_current_index(p_win->p_nav);
   guint u_n   = navigator_get_count(p_win->p_nav);
   if (u_n == 0) {
      return;
   }
   for (gint i_delta = -1; i_delta <= 1; i_delta += 2) {
      gint i_j = i_idx + i_delta;
      if (i_j < 0 || i_j >= (gint)u_n) {
         continue;
      }
      GFile *p_file = navigator_get_file(p_win->p_nav, (guint)i_j);
      if (p_file != NULL && texturecache_get(p_win->p_cache, p_file) == NULL) {
         loader_load_async(p_file, p_win->p_prefetch_cancel,
                           _prefetch_finish_cb, g_object_ref(p_win));
      }
   }
}

static void
_load_current(GgazeWindow *p_win) {
   if (p_win->p_nav == NULL) {
      return;
   }
   GFile *p_cur = navigator_get_current(p_win->p_nav);
   if (p_cur == NULL) {
      ggaze_viewer_set_texture(GGAZE_VIEWER(p_win->p_viewer), NULL);
      _update_header(p_win);
      return;
   }

   /* Cache hit: show immediately, no async load. */
   GdkTexture *p_cached = texturecache_get(p_win->p_cache, p_cur);
   if (p_cached != NULL) {
      /* Cancel any in-flight visible load for a now-stale path. */
      g_cancellable_cancel(p_win->p_cancel);
      g_clear_object(&p_win->p_cancel);
      p_win->p_cancel = g_cancellable_new();
      _show_texture(p_win, p_cached);
      _update_header(p_win);
      _prefetch(p_win);
      return;
   }

   /* Cache miss: cancel the previous visible load, start a new async load.
    * Last-write-wins is enforced in _load_finish_cb. */
   g_cancellable_cancel(p_win->p_cancel);
   g_clear_object(&p_win->p_cancel);
   p_win->p_cancel = g_cancellable_new();
   loader_load_async(p_cur, p_win->p_cancel, _load_finish_cb,
                     g_object_ref(p_win));
   _update_header(p_win);
}

static void
_update_header(GgazeWindow *p_win) {
   gchar *c_title = NULL;
   if (p_win->p_nav != NULL) {
      GFile *p_cur       = navigator_get_current(p_win->p_nav);
      guint  u_remaining = navigator_get_remaining(p_win->p_nav);
      guint  u_total     = navigator_get_count(p_win->p_nav);
      gint   i_idx       = navigator_get_current_index(p_win->p_nav);
      if (p_cur != NULL) {
         char *c_name = g_file_get_basename(p_cur);
         if (u_total > 0 && i_idx >= 0) {
            c_title = g_strdup_printf("%s  \u00b7  %d/%u", c_name, i_idx + 1,
                                      u_remaining);
         } else {
            c_title = g_strdup(c_name);
         }
         g_free(c_name);
      }
   }
   if (c_title == NULL) {
      c_title = g_strdup("ggaze");
   }
   gtk_window_set_title(GTK_WINDOW(p_win), c_title);
   g_free(c_title);
}

/* --- GObject ------------------------------------------------------------- */

static void
ggaze_window_dispose(GObject *p_obj) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_obj);
   if (p_win->p_nav != NULL) {
      g_signal_handlers_disconnect_by_data(p_win->p_nav, p_win);
      if (p_win->p_grid != NULL) {
         ggaze_grid_detach(p_win->p_grid); /* before the nav is freed */
      }
      g_clear_object(&p_win->p_nav);
   }
   g_cancellable_cancel(p_win->p_prefetch_cancel);
   g_clear_object(&p_win->p_prefetch_cancel);
   g_cancellable_cancel(p_win->p_cancel);
   g_clear_object(&p_win->p_cancel);
   if (p_win->u_slideshow != 0) {
      g_source_remove(p_win->u_slideshow);
      p_win->u_slideshow = 0;
   }
   if (p_win->u_info_hide != 0) {
      g_source_remove(p_win->u_info_hide);
      p_win->u_info_hide = 0;
   }
   if (p_win->u_hdr_hide != 0) {
      g_source_remove(p_win->u_hdr_hide);
      p_win->u_hdr_hide = 0;
   }
   g_clear_pointer(&p_win->p_cache, texturecache_delete);
   g_clear_pointer(&p_win->p_trash, trash_delete);
   g_clear_pointer(&p_win->p_thumb, thumbnail_delete);
   /* p_stack/p_viewer/p_grid are GtkWidgets parented to the window; GTK
    * releases them. */
   G_OBJECT_CLASS(ggaze_window_parent_class)->dispose(p_obj);
}

static void
ggaze_window_class_init(GgazeWindowClass *p_klass) {
   GObjectClass *p_obj_class = G_OBJECT_CLASS(p_klass);
   p_obj_class->dispose      = ggaze_window_dispose;
}

static void
ggaze_window_init(GgazeWindow *p_win) {
   p_win->p_cancel          = g_cancellable_new();
   p_win->p_prefetch_cancel = g_cancellable_new();
   p_win->p_cache           = texturecache_new(4);
   p_win->p_thumb           = thumbnail_new();
   p_win->p_trash           = NULL; /* created on open */
   p_win->p_grid            = NULL; /* created on open */
   p_win->i_grid_size       = 128;

   /* Header bar (libadwaita, decision #29). */
   GtkWidget *p_header = adw_header_bar_new();
   gtk_window_set_titlebar(GTK_WINDOW(p_win), p_header);

   /* Two-view stack: "grid" is created on open; placeholder until then. */
   p_win->p_stack = gtk_stack_new();
   gtk_stack_set_transition_type(GTK_STACK(p_win->p_stack),
                                 GTK_STACK_TRANSITION_TYPE_CROSSFADE);

   /* Wrap the stack in a GtkOverlay so the info label can float on top. */
   p_win->p_overlay = gtk_overlay_new();
   gtk_overlay_set_child(GTK_OVERLAY(p_win->p_overlay), p_win->p_stack);
   p_win->p_info_lbl = gtk_label_new("");
   gtk_widget_add_css_class(p_win->p_info_lbl, "ggaze-info");
   gtk_widget_set_margin_start(p_win->p_info_lbl, 12);
   gtk_widget_set_margin_top(p_win->p_info_lbl, 12);
   gtk_widget_set_visible(p_win->p_info_lbl, FALSE);
   gtk_overlay_add_overlay(GTK_OVERLAY(p_win->p_overlay), p_win->p_info_lbl);
   gtk_window_set_child(GTK_WINDOW(p_win), p_win->p_overlay);

   GtkWidget *p_grid = gtk_label_new("grid");
   gtk_widget_add_css_class(p_grid, "dim-label");
   gtk_stack_add_named(GTK_STACK(p_win->p_stack), p_grid, "grid");

   p_win->p_viewer = ggaze_viewer_new();
   gtk_widget_set_hexpand(p_win->p_viewer, TRUE);
   gtk_widget_set_vexpand(p_win->p_viewer, TRUE);
   gtk_stack_add_named(GTK_STACK(p_win->p_stack), p_win->p_viewer, "large");

   gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack), "grid");

   /* Actions + keybindings (decision #10/#12). */
   g_action_map_add_action_entries(G_ACTION_MAP(p_win), ACTIONS,
                                   G_N_ELEMENTS(ACTIONS), p_win);
   shortcuts_install(GTK_WIDGET(p_win));

   /* File/folder drag-and-drop (decision #27). */
   GtkDropTarget *p_drop =
      gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
   g_signal_connect(p_drop, "drop", G_CALLBACK(drop_cb), p_win);
   gtk_widget_add_controller(GTK_WIDGET(p_win), GTK_EVENT_CONTROLLER(p_drop));
}

/* --- public -------------------------------------------------------------- */

GgazeWindow *
ggaze_window_new(GgazeApp *p_app) {
   return (GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, "application", p_app,
                                     "default-width", 800, "default-height",
                                     600, NULL)));
}

void
ggaze_window_open(GgazeWindow *p_win, GFile *p_arg) {
   g_return_if_fail(GGAZE_IS_WINDOW(p_win));
   g_return_if_fail(G_IS_FILE(p_arg));

   if (p_win->p_nav != NULL) {
      g_signal_handlers_disconnect_by_data(p_win->p_nav, p_win);
      if (p_win->p_grid != NULL) {
         ggaze_grid_detach(p_win->p_grid);
      }
      g_clear_object(&p_win->p_nav);
   }

   GFile    *p_dir   = NULL;
   GFile    *p_start = NULL;
   GFileType e_type =
      g_file_query_file_type(p_arg, G_FILE_QUERY_INFO_NONE, NULL);
   if (e_type == G_FILE_TYPE_DIRECTORY) {
      p_dir = (GFile *)g_object_ref(p_arg);
   } else {
      p_dir   = g_file_get_parent(p_arg);
      p_start = (GFile *)g_object_ref(p_arg);
   }
   if (p_dir == NULL) {
      g_clear_object(&p_start);
      return;
   }

   p_win->p_nav = navigator_new(p_dir, GGAZE_SORT_NAME, TRUE, TRUE);
   g_clear_pointer(&p_win->p_trash, trash_delete);
   g_clear_object(&p_dir);
   g_signal_connect(p_win->p_nav, "changed", G_CALLBACK(nav_changed_cb), p_win);
   if (p_start != NULL) {
      navigator_set_current_file(p_win->p_nav, p_start);
      g_clear_object(&p_start);
   }

   /* Build the grid (replaces the "grid" placeholder or the old grid). */
   {
      GtkWidget *p_old =
         gtk_stack_get_child_by_name(GTK_STACK(p_win->p_stack), "grid");
      if (p_old != NULL) {
         if (GGAZE_IS_GRID(p_old)) {
            ggaze_grid_detach(GGAZE_GRID(p_old));
         }
         gtk_stack_remove(GTK_STACK(p_win->p_stack), p_old);
      }
   }
   GFile *p_navdir = navigator_get_dir(p_win->p_nav);
   p_win->p_trash  = trash_new(p_navdir);
   p_win->p_grid   = GGAZE_GRID(
      ggaze_grid_new(p_win->p_nav, p_win->p_thumb, p_win->i_grid_size, FALSE));
   g_signal_connect(p_win->p_grid, "activate", G_CALLBACK(_on_grid_activate),
                    p_win);
   gtk_stack_add_named(GTK_STACK(p_win->p_stack), GTK_WIDGET(p_win->p_grid),
                       "grid");

   gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack), "large");
   _load_current(p_win);
}

void
ggaze_window_prev(GgazeWindow *p_win) {
   g_return_if_fail(GGAZE_IS_WINDOW(p_win));
   if (p_win->p_nav != NULL) {
      navigator_prev(p_win->p_nav); /* emits "changed" -> _load_current */
   }
}

void
ggaze_window_next(GgazeWindow *p_win) {
   g_return_if_fail(GGAZE_IS_WINDOW(p_win));
   if (p_win->p_nav != NULL) {
      navigator_next(p_win->p_nav);
   }
}

void
ggaze_window_first(GgazeWindow *p_win) {
   g_return_if_fail(GGAZE_IS_WINDOW(p_win));
   if (p_win->p_nav != NULL) {
      navigator_first(p_win->p_nav);
   }
}

void
ggaze_window_last(GgazeWindow *p_win) {
   g_return_if_fail(GGAZE_IS_WINDOW(p_win));
   if (p_win->p_nav != NULL) {
      navigator_last(p_win->p_nav);
   }
}

GtkStack *
ggaze_window_get_stack(GgazeWindow *p_win) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), NULL);
   return (GTK_STACK(p_win->p_stack));
}
