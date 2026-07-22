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
#include <glib/gstdio.h>
#include <string.h>
#include <gtk/gtk.h>

#include "ggaze-config.h"
#include "gridview.h"
#include "info.h"
#include "loader/loader.h"
#include "navigator.h"
#include "shortcuts.h"
#include "texturecache.h"
#include "thumbnail.h"
#include "trash.h"
#include "viewer.h"
#if GGAZE_HAVE_GEGL
#include "enhancer.h"
#endif

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
#if GGAZE_HAVE_GEGL
   guint8     u_enhance_mask;    /* bit i -> preset i enabled (layered) */
   Enhancer  *p_enhancer;        /* GEGL preset engine (NULL w/o GEGL) */
   GtkWidget *p_enhance_panel;   /* GtkRevealer side panel (NULL w/o GEGL) */
   GtkWidget *p_enhance_btns[8]; /* preset row buttons (for highlighting) */
#endif
   GtkWidget *p_content; /* horizontal box: [enhance panel] + image area */
};

G_DEFINE_TYPE(GgazeWindow, ggaze_window, GTK_TYPE_APPLICATION_WINDOW)

/* --- forward decls ------------------------------------------------------- */
static void     _load_current(GgazeWindow *p_win);
static void     _prefetch(GgazeWindow *p_win);
static void     _show_texture(GgazeWindow *p_win, GdkTexture *p_tex);
static void     _update_header(GgazeWindow *p_win);
static void     _on_grid_activate(GgazeGrid *p_grid, gpointer p_data);
static void     _show_info(GgazeWindow *p_win);
static void     _hide_info(GgazeWindow *p_win);
static gboolean _slideshow_tick(gpointer p_data);
#if GGAZE_HAVE_GEGL
static void     _enhance_update_highlights(GgazeWindow *p_win);
static void     _enhance_panel_reparent(GgazeWindow *p_win, gboolean b_overlay);
static gboolean _enhance_do_save(GgazeWindow *p_win);
#endif

/* Navigation continuations used by _maybe_save_then after the (GEGL) save
 * /discard dialog resolves. They are trivial wrappers over the public
 * navigation API and do not depend on GEGL, so they are always compiled
 * (the _action_prev/next/first/last handlers reference them regardless of
 * the GEGL build configuration). */
static gboolean
_proceed_prev(gpointer d) {
   ggaze_window_prev(GGAZE_WINDOW(d));
   return (G_SOURCE_REMOVE);
}
static gboolean
_proceed_next(gpointer d) {
   ggaze_window_next(GGAZE_WINDOW(d));
   return (G_SOURCE_REMOVE);
}
static gboolean
_proceed_first(gpointer d) {
   ggaze_window_first(GGAZE_WINDOW(d));
   return (G_SOURCE_REMOVE);
}
static gboolean
_proceed_last(gpointer d) {
   ggaze_window_last(GGAZE_WINDOW(d));
   return (G_SOURCE_REMOVE);
}
#if GGAZE_HAVE_GEGL
/* Export the current image with the enabled-preset chain to
 * <stem>-enhanced.<ext>. Returns TRUE on success; prints the saved name. */
static gboolean
_enhance_do_save(GgazeWindow *p_win) {
   if (p_win->p_nav == NULL || p_win->p_enhancer == NULL ||
       p_win->u_enhance_mask == 0) {
      return (FALSE);
   }
   GFile *p_file = navigator_get_current(p_win->p_nav);
   if (p_file == NULL) {
      return (FALSE);
   }
   char       *c_base = g_file_get_basename(p_file);
   char       *c_dot  = strrchr(c_base, '.');
   const char *c_ext  = ".jpg";
   if (c_dot != NULL && (g_ascii_strcasecmp(c_dot, ".jpg") == 0 ||
                         g_ascii_strcasecmp(c_dot, ".jpeg") == 0 ||
                         g_ascii_strcasecmp(c_dot, ".png") == 0 ||
                         g_ascii_strcasecmp(c_dot, ".webp") == 0)) {
      c_ext = c_dot;
   }
   char *c_stem;
   if (c_dot != NULL && c_ext == c_dot) {
      c_stem = g_strndup(c_base, (gsize)(c_dot - c_base));
   } else {
      c_stem = g_strdup(c_base);
   }
   GFile *p_dir     = g_file_get_parent(p_file);
   char  *c_outname = g_strdup_printf("%s-enhanced%s", c_stem, c_ext);
   GFile *p_out     = g_file_get_child(p_dir, c_outname);
   g_free(c_outname);
   g_free(c_stem);
   g_free(c_base);
   g_object_unref(p_dir);

   GError     *p_err = NULL;
   GeglBuffer *p_buf = enhancer_load(p_file, &p_err);
   gboolean    b_ok  = FALSE;
   if (p_buf != NULL) {
      const GPtrArray *p_presets = enhancer_get_presets(p_win->p_enhancer);
      b_ok = enhancer_export_chain(p_win->p_enhancer, p_buf, p_presets,
                                   p_win->u_enhance_mask, p_out, &p_err);
      g_object_unref(p_buf);
   }
   char *c_saved = b_ok ? g_file_get_basename(p_out) : NULL;
   g_object_unref(p_out);
   if (b_ok) {
      g_printerr("ggaze: saved %s\n", c_saved);
   } else {
      g_warning("ggaze: enhance-save failed: %s",
                p_err != NULL ? p_err->message : "(no detail)");
   }
   g_free(c_saved);
   g_clear_error(&p_err);
   return (b_ok);
}

typedef struct {
   GgazeWindow *p_win;
   GSourceFunc  fn;
   gpointer     data;
} _SaveCtx;

/* Alert-dialog response: 0=Cancel, 1=Discard, 2=Save. */
static void
_save_dialog_cb(GObject *p_dlg, GAsyncResult *p_res, gpointer p_data) {
   _SaveCtx    *p_ctx = (_SaveCtx *)p_data;
   GgazeWindow *p_win = p_ctx->p_win;
   GError      *p_err = NULL;
   gint         i_btn =
      gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(p_dlg), p_res, &p_err);
   g_object_unref(GTK_ALERT_DIALOG(p_dlg));
   if (p_err != NULL) { /* dismissed / error -> treat as Cancel */
      g_clear_error(&p_err);
      g_free(p_ctx);
      return;
   }
   if (i_btn == 2) { /* Save */
      _enhance_do_save(p_win);
   }
   if (i_btn == 1 || i_btn == 2) { /* Discard or Save: clear + proceed */
      p_win->u_enhance_mask = 0;
      _enhance_update_highlights(p_win);
      _update_header(p_win);
      if (p_ctx->fn != NULL) {
         p_ctx->fn(p_ctx->data);
      }
   } /* Cancel: keep the preview, do not navigate. */
   g_free(p_ctx);
}

/* If an enhance preview is active (unsaved), ask Save / Discard / Cancel
 * before proceeding with fn. If no preview, just run fn. */
static void
_maybe_save_then(GgazeWindow *p_win, GSourceFunc fn, gpointer data) {
   if (p_win->u_enhance_mask == 0 || p_win->p_enhancer == NULL) {
      if (fn != NULL) {
         fn(data);
      }
      return;
   }
   GtkAlertDialog *p_dlg =
      gtk_alert_dialog_new("Save the enhanced copy before leaving this image?");
   static const char *const c_btns[] = {"Cancel", "Discard", "Save", NULL};
   gtk_alert_dialog_set_buttons(p_dlg, c_btns);
   gtk_alert_dialog_set_default_button(p_dlg, 2);
   gtk_alert_dialog_set_cancel_button(p_dlg, 0);
   gtk_alert_dialog_set_modal(p_dlg, TRUE);
   _SaveCtx *p_ctx = g_new(_SaveCtx, 1);
   p_ctx->p_win    = p_win;
   p_ctx->fn       = fn;
   p_ctx->data     = data;
   gtk_alert_dialog_choose(p_dlg, GTK_WINDOW(p_win), NULL, _save_dialog_cb,
                           p_ctx);
}

#else /* !GGAZE_HAVE_GEGL */
static void
_maybe_save_then(GgazeWindow *p_win, GSourceFunc fn, gpointer data) {
   (void)p_win;
   if (fn != NULL) {
      fn(data);
   }
}
#endif

/* --- actions ------------------------------------------------------------- */

static void
_action_prev(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _maybe_save_then(GGAZE_WINDOW(p_data), _proceed_prev, p_data);
}

static void
_action_next(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _maybe_save_then(GGAZE_WINDOW(p_data), _proceed_next, p_data);
}

static void
_action_first(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _maybe_save_then(GGAZE_WINDOW(p_data), _proceed_first, p_data);
}

static void
_action_last(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _maybe_save_then(GGAZE_WINDOW(p_data), _proceed_last, p_data);
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
      /* Leaving the grid: sync navigator.current to the highlighted cell so
       * the large view opens the selected image, then load it. */
      if (p_win->p_grid != NULL) {
         ggaze_grid_sync_current(p_win->p_grid);
      }
      gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack), "large");
      _load_current(p_win);
   }
}

/* Toggle a mark on the highlighted grid cell (grid view) or the current image
 * (large view). Marks are ggaze's multi-selection: D / Ctrl+c / m act on the
 * marked set. Toggle does not emit navigator "changed", so the grid cell's
 * badge is updated in place (no reflow/re-decode). */
static void
_action_mark(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL) {
      return;
   }
   GFile      *p_target = NULL;
   const char *c_cur =
      gtk_stack_get_visible_child_name(GTK_STACK(p_win->p_stack));
   if (g_strcmp0(c_cur, "grid") == 0 && p_win->p_grid != NULL) {
      p_target = ggaze_grid_get_selected_file(p_win->p_grid);
   }
   if (p_target == NULL) {
      p_target = navigator_get_current(p_win->p_nav);
   }
   if (p_target == NULL) {
      return;
   }
   navigator_toggle_mark(p_win->p_nav, p_target);
   if (p_win->p_grid != NULL) {
      ggaze_grid_update_mark_badge(p_win->p_grid, p_target);
   }
   _update_header(p_win);
}

static void
_action_mark_all(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL) {
      return;
   }
   navigator_mark_all(p_win->p_nav); /* emits "changed" -> grid refresh */
   _update_header(p_win);
}

/* `V` range-mark: mark every file from the last `v`-toggled mark (the anchor)
 * to the highlighted grid cell / current large-view image, inclusive. No-op
 * if no mark has been toggled on yet (no anchor). Emits "changed" so grid
 * badges and the header mark count refresh. */
static void
_action_mark_range(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL) {
      return;
   }
   GFile *p_anchor = navigator_get_last_mark(p_win->p_nav);
   if (p_anchor == NULL) {
      return;
   }
   GFile      *p_target = NULL;
   const char *c_cur =
      gtk_stack_get_visible_child_name(GTK_STACK(p_win->p_stack));
   if (g_strcmp0(c_cur, "grid") == 0 && p_win->p_grid != NULL) {
      p_target = ggaze_grid_get_selected_file(p_win->p_grid);
   }
   if (p_target == NULL) {
      p_target = navigator_get_current(p_win->p_nav);
   }
   if (p_target == NULL) {
      return;
   }
   navigator_mark_range(p_win->p_nav, p_anchor, p_target);
   _update_header(p_win);
}

/* GtkBuilder UI for the shortcuts overlay (?). Accel strings use gtk
 * accelerator syntax: "h Left" means h OR Left triggers it. */
static const char *SHORTCUTS_UI =
   "<interface>\n"
   "  <object class=\"GtkShortcutsWindow\" id=\"shortcuts\">\n"
   "    <property name=\"modal\">True</property>\n"
   "    <property name=\"section-name\">shortcuts</property>\n"
   "    <child>\n"
   "      <object class=\"GtkShortcutsSection\" id=\"sec\">\n"
   "        <property name=\"section-name\">shortcuts</property>\n"
   "        <property name=\"title\">ggaze</property>\n"
   "        <child>\n"
   "          <object class=\"GtkShortcutsGroup\">\n"
   "            <property name=\"title\">Navigation</property>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">h Left</property>\n"
   "                <property name=\"title\">Previous image</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">l Right</property>\n"
   "                <property name=\"title\">Next image</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">j</property>\n"
   "                <property name=\"title\">Cursor down one row (grid)\n"
   "                </property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">k</property>\n"
   "                <property name=\"title\">Cursor up one row (grid)\n"
   "                </property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">g</property>\n"
   "                <property name=\"title\">First image</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">Shift+G</property>\n"
   "                <property name=\"title\">Last image</property>\n"
   "              </object>\n"
   "            </child>\n"
   "          </object>\n"
   "        </child>\n"
   "        <child>\n"
   "          <object class=\"GtkShortcutsGroup\">\n"
   "            <property name=\"title\">View</property>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">t</property>\n"
   "                <property name=\"title\">Toggle large / grid</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">f</property>\n"
   "                <property name=\"title\">Fullscreen</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">Shift+S</property>\n"
   "                <property name=\"title\">Slideshow</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">i</property>\n"
   "                <property name=\"title\">Info overlay</property>\n"
   "              </object>\n"
   "            </child>\n"
   "          </object>\n"
   "        </child>\n"
   "        <child>\n"
   "          <object class=\"GtkShortcutsGroup\">\n"
   "            <property name=\"title\">Selection (marks)</property>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">v</property>\n"
   "                <property name=\"title\">Toggle mark on "
   "highlighted</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">Shift+V</property>\n"
   "                <property name=\"title\">Range-mark from last mark "
   "to current</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">Ctrl+a</property>\n"
   "                <property name=\"title\">Mark all</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">Escape</property>\n"
   "                <property name=\"title\">Clear marks / back</property>\n"
   "              </object>\n"
   "            </child>\n"
   "          </object>\n"
   "        </child>\n"
   "        <child>\n"
   "          <object class=\"GtkShortcutsGroup\">\n"
   "            <property name=\"title\">Files</property>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">o</property>\n"
   "                <property name=\"title\">Open</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">d</property>\n"
   "                <property name=\"title\">Trash</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">Shift+D</property>\n"
   "                <property name=\"title\">Delete permanently</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">u</property>\n"
   "                <property name=\"title\">Undo</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">q</property>\n"
   "                <property name=\"title\">Quit</property>\n"
   "              </object>\n"
   "            </child>\n"
   "          </object>\n"
   "        </child>\n"
   "        <child>\n"
   "          <object class=\"GtkShortcutsGroup\">\n"
   "            <property name=\"title\">Enhance</property>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">a</property>\n"
   "                <property name=\"title\">Toggle the enhance side "
   "panel</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">1 2 3 4 5 6 7 8</property>\n"
   "                <property name=\"title\">Toggle enhance preset 1-8 "
   "(layered); 0 = Original</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">s</property>\n"
   "                <property name=\"title\">Save enhanced copy</property>\n"
   "              </object>\n"
   "            </child>\n"
   "          </object>\n"
   "        </child>\n"
   "        <child>\n"
   "          <object class=\"GtkShortcutsGroup\">\n"
   "            <property name=\"title\">Zoom</property>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">plus equal</property>\n"
   "                <property name=\"title\">Zoom in</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">minus "
   "underscore</property>\n"
   "                <property name=\"title\">Zoom out</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">question</property>\n"
   "                <property name=\"title\">Show this help</property>\n"
   "              </object>\n"
   "            </child>\n"
   "          </object>\n"
   "        </child>\n"
   "      </object>\n"
   "    </child>\n"
   "  </object>\n"
   "</interface>\n";

static void
_action_shortcuts(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow        *p_win = GGAZE_WINDOW(p_data);
   GtkBuilder         *p_b   = gtk_builder_new_from_string(SHORTCUTS_UI, -1);
   GtkShortcutsWindow *p_w =
      GTK_SHORTCUTS_WINDOW(gtk_builder_get_object(p_b, "shortcuts"));
   if (p_w == NULL) {
      g_object_unref(p_b);
      return;
   }
   gtk_window_set_transient_for(GTK_WINDOW(p_w), GTK_WINDOW(p_win));
   gtk_window_set_title(GTK_WINDOW(p_w), "ggaze — keyboard shortcuts");
   /* Keep the builder alive for the window's lifetime, drop it on close. */
   g_signal_connect_swapped(p_w, "destroy", G_CALLBACK(g_object_unref), p_b);
   gtk_window_present(GTK_WINDOW(p_w));
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
#if GGAZE_HAVE_GEGL
      _enhance_panel_reparent(p_win, FALSE); /* back to sidebar next to image */
#endif
   } else {
      gtk_window_fullscreen(GTK_WINDOW(p_win));
      p_win->b_fullscreen = TRUE;
#if GGAZE_HAVE_GEGL
      _enhance_panel_reparent(p_win, TRUE); /* overlay over the image */
#endif
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
   } else if (p_win->p_nav != NULL &&
              navigator_get_mark_count(p_win->p_nav) > 0) {
      /* Contextual Esc: clear marks before backing out (docs/ui-and-
       * interactions.md marks). Emits "changed" -> grid refreshes badges. */
      navigator_clear_marks(p_win->p_nav);
      _update_header(p_win);
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

#if GGAZE_HAVE_GEGL
/* Update each preset button's "ggaze-enhance-on" highlight from the mask. */
static void
_enhance_update_highlights(GgazeWindow *p_win) {
   for (guint i = 0; i < G_N_ELEMENTS(p_win->p_enhance_btns); i++) {
      GtkWidget *p_btn = p_win->p_enhance_btns[i];
      if (p_btn == NULL) {
         continue;
      }
      if ((p_win->u_enhance_mask & (guint8)(1u << i)) != 0) {
         gtk_widget_add_css_class(p_btn, "ggaze-enhance-on");
      } else {
         gtk_widget_remove_css_class(p_btn, "ggaze-enhance-on");
      }
   }
}

/* Apply the enabled-preset chain (u_enhance_mask) to the current image as a
 * live preview. An empty mask restores the original. Switches to large view
 * so the result shows. Synchronous - may briefly block on large images. */
static void
_apply_enhance_mask(GgazeWindow *p_win) {
   if (p_win->p_nav == NULL || p_win->p_enhancer == NULL) {
      return;
   }
   const char *c_cur =
      gtk_stack_get_visible_child_name(GTK_STACK(p_win->p_stack));
   if (g_strcmp0(c_cur, "large") != 0) {
      gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack), "large");
   }
   if (p_win->u_enhance_mask == 0) {
      _load_current(p_win); /* restore original (texturecache is fast) */
      _update_header(p_win);
      return;
   }
   GFile *p_file = navigator_get_current(p_win->p_nav);
   if (p_file == NULL) {
      p_win->u_enhance_mask = 0;
      _enhance_update_highlights(p_win);
      _update_header(p_win);
      return;
   }
   const GPtrArray *p_presets = enhancer_get_presets(p_win->p_enhancer);
   GError          *p_err     = NULL;
   GeglBuffer      *p_buf     = enhancer_load(p_file, &p_err);
   GdkTexture      *p_tex     = NULL;
   if (p_buf != NULL) {
      GeglBuffer *p_enh = enhancer_apply_chain(
         p_win->p_enhancer, p_buf, p_presets, p_win->u_enhance_mask, &p_err);
      if (p_enh != NULL) {
         p_tex = enhancer_buffer_to_texture(p_enh, &p_err);
         g_object_unref(p_enh);
      }
      g_object_unref(p_buf);
   }
   if (p_tex == NULL) {
      g_warning("ggaze: enhance failed: %s",
                p_err != NULL ? p_err->message : "(no detail)");
      g_clear_error(&p_err);
      p_win->u_enhance_mask = 0;
      _enhance_update_highlights(p_win);
      _load_current(p_win);
   } else {
      _show_texture(p_win, p_tex);
      g_object_unref(p_tex);
   }
   _update_header(p_win);
}

/* Clicked row: idx 0..7 toggles that preset's bit; idx -1 (Original) clears
 * the mask. Then refresh highlights + re-apply the (possibly empty) chain. */
static void
_enhance_row_toggle(GgazeWindow *p_win, GtkWidget *p_btn) {
   gint i_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(p_btn), "idx"));
   if (i_idx < 0) {
      p_win->u_enhance_mask = 0; /* Original */
   } else if (i_idx < (gint)G_N_ELEMENTS(p_win->p_enhance_btns)) {
      p_win->u_enhance_mask ^= (guint8)(1u << i_idx);
   }
   _enhance_update_highlights(p_win);
   _apply_enhance_mask(p_win);
}

/* Build the enhance side panel: a GtkRevealer (slide right) holding one
 * button per preset plus an "Original" row, placed in the content box next to
 * the image (reparented to the overlay in fullscreen). Hidden until 'a'. */
static void
_build_enhance_panel(GgazeWindow *p_win) {
   if (p_win->p_enhancer == NULL || p_win->p_content == NULL) {
      return;
   }
   const GPtrArray *p_presets = enhancer_get_presets(p_win->p_enhancer);
   GtkWidget       *p_box     = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
   gtk_widget_set_margin_start(p_box, 8);
   gtk_widget_set_margin_end(p_box, 8);
   gtk_widget_set_margin_top(p_box, 8);
   gtk_widget_set_margin_bottom(p_box, 8);
   for (guint i = 0; i < G_N_ELEMENTS(p_win->p_enhance_btns); i++) {
      const EnhancerPreset *p_pr =
         (p_presets != NULL && i < p_presets->len)
            ? g_ptr_array_index((GPtrArray *)p_presets, i)
            : NULL;
      char *c_lbl =
         g_strdup_printf("%u  %s", i + 1, p_pr != NULL ? p_pr->c_name : "-");
      GtkWidget *p_btn = gtk_button_new_with_label(c_lbl);
      gtk_widget_set_size_request(p_btn, 160, -1);
      gtk_widget_set_halign(p_btn, GTK_ALIGN_START);
      g_object_set_data(G_OBJECT(p_btn), "idx", GINT_TO_POINTER((gint)i));
      g_signal_connect_swapped(p_btn, "clicked",
                               G_CALLBACK(_enhance_row_toggle), p_win);
      gtk_box_append(GTK_BOX(p_box), p_btn);
      p_win->p_enhance_btns[i] = p_btn;
      g_free(c_lbl);
   }
   GtkWidget *p_btn0 = gtk_button_new_with_label("0  Original");
   gtk_widget_set_size_request(p_btn0, 160, -1);
   gtk_widget_set_halign(p_btn0, GTK_ALIGN_START);
   g_object_set_data(G_OBJECT(p_btn0), "idx", GINT_TO_POINTER(-1));
   g_signal_connect_swapped(p_btn0, "clicked", G_CALLBACK(_enhance_row_toggle),
                            p_win);
   gtk_box_append(GTK_BOX(p_box), p_btn0);

   /* A dim hint that 's' saves the enhanced copy. */
   GtkWidget *p_hint = gtk_label_new("s  Save enhanced copy");
   gtk_widget_set_halign(p_hint, GTK_ALIGN_START);
   gtk_widget_set_margin_top(p_hint, 8);
   gtk_widget_add_css_class(p_hint, "dim-label");
   gtk_box_append(GTK_BOX(p_box), p_hint);

   GtkWidget *p_rev = gtk_revealer_new();
   gtk_revealer_set_transition_type(GTK_REVEALER(p_rev),
                                    GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
   gtk_revealer_set_child(GTK_REVEALER(p_rev), p_box);
   gtk_revealer_set_reveal_child(GTK_REVEALER(p_rev), FALSE);
   /* Sidebar (normal mode): sits in the content box next to the image. */
   gtk_widget_set_hexpand(p_rev, FALSE);
   gtk_widget_set_vexpand(p_rev, TRUE);
   gtk_box_prepend(GTK_BOX(p_win->p_content), p_rev);
   p_win->p_enhance_panel = p_rev;
}

/* Move the enhance panel between the sidebar (next to the image) and an
 * overlay over the image (used in fullscreen, where there's no side room). */
static void
_enhance_panel_reparent(GgazeWindow *p_win, gboolean b_overlay) {
   GtkWidget *p_rev = p_win->p_enhance_panel;
   if (p_rev == NULL) {
      return;
   }
   GtkWidget *p_parent = gtk_widget_get_parent(p_rev);
   if (b_overlay && p_parent != p_win->p_overlay) {
      g_object_ref(p_rev);
      if (p_parent == p_win->p_content) {
         gtk_box_remove(GTK_BOX(p_win->p_content), p_rev);
      }
      gtk_overlay_add_overlay(GTK_OVERLAY(p_win->p_overlay), p_rev);
      gtk_widget_set_halign(p_rev, GTK_ALIGN_START);
      gtk_widget_set_valign(p_rev, GTK_ALIGN_START);
      gtk_widget_set_margin_top(p_rev, 48);
      g_object_unref(p_rev);
   } else if (!b_overlay && p_parent != p_win->p_content) {
      g_object_ref(p_rev);
      if (p_parent == p_win->p_overlay) {
         gtk_overlay_remove_overlay(GTK_OVERLAY(p_win->p_overlay), p_rev);
      }
      gtk_widget_set_halign(p_rev, GTK_ALIGN_FILL);
      gtk_widget_set_valign(p_rev, GTK_ALIGN_FILL);
      gtk_widget_set_margin_top(p_rev, 0);
      gtk_box_prepend(GTK_BOX(p_win->p_content), p_rev);
      g_object_unref(p_rev);
   }
}

/* win.enhance (key 'a'): toggle the enhance side panel on/off. */
static void
_action_enhance(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_enhance_panel == NULL) {
      return;
   }
   gboolean b_vis =
      gtk_revealer_get_reveal_child(GTK_REVEALER(p_win->p_enhance_panel));
   gtk_revealer_set_reveal_child(GTK_REVEALER(p_win->p_enhance_panel), !b_vis);
}

/* win.enhance-N (keys 1-8): toggle preset N on/off (layered), then re-apply. */
static void
_action_enhance_n(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_v;
   GgazeWindow *p_win  = GGAZE_WINDOW(p_data);
   const char  *c_name = g_action_get_name(G_ACTION(p_a));
   if (!g_str_has_prefix(c_name, "enhance-")) {
      return;
   }
   gint i_idx =
      (gint)g_ascii_strtoll(c_name + strlen("enhance-"), NULL, 10) - 1;
   if (p_win->p_enhancer == NULL || i_idx < 0 ||
       i_idx >= (gint)G_N_ELEMENTS(p_win->p_enhance_btns)) {
      return;
   }
   p_win->u_enhance_mask ^= (guint8)(1u << i_idx);
   _enhance_update_highlights(p_win);
   _apply_enhance_mask(p_win);
}

/* win.enhance-save (key 's'): export the current image with the enabled-preset
 * chain to <stem>-enhanced.<ext>. Never overwrites the original. No-op (with
 * a warning) when no preset is enabled. */
static void
_action_enhance_save(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL || p_win->p_enhancer == NULL) {
      return;
   }
   if (p_win->u_enhance_mask == 0) {
      g_warning("ggaze: nothing to save (no enhance preset enabled)");
      return;
   }
   _enhance_do_save(p_win);
}
#else  /* !GGAZE_HAVE_GEGL */
static void
_action_enhance(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   (void)p_data;
   g_warning("ggaze: GEGL not built in");
}
static void
_action_enhance_save(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   (void)p_data;
   g_warning("ggaze: GEGL not built in");
}
static void
_action_enhance_n(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   (void)p_data;
}
#endif /* GGAZE_HAVE_GEGL */

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
   {.name = "mark", .activate = _action_mark},
   {.name = "mark-all", .activate = _action_mark_all},
   {.name = "mark-range", .activate = _action_mark_range},
   {.name = "shortcuts", .activate = _action_shortcuts},
   {.name = "enhance-1", .activate = _action_enhance_n},
   {.name = "enhance-2", .activate = _action_enhance_n},
   {.name = "enhance-3", .activate = _action_enhance_n},
   {.name = "enhance-4", .activate = _action_enhance_n},
   {.name = "enhance-5", .activate = _action_enhance_n},
   {.name = "enhance-6", .activate = _action_enhance_n},
   {.name = "enhance-7", .activate = _action_enhance_n},
   {.name = "enhance-8", .activate = _action_enhance_n},
   {.name = "zoom-in", .activate = _action_zoom_in},
   {.name = "zoom-out", .activate = _action_zoom_out},
   {.name = "fullscreen", .activate = _action_fullscreen},
   {.name = "slideshow", .activate = _action_slideshow},
   {.name = "info", .activate = _action_info},
   {.name = "back", .activate = _action_back},
   {.name = "enhance", .activate = _action_enhance},
   {.name = "enhance-save", .activate = _action_enhance_save},
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
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
#if GGAZE_HAVE_GEGL
   /* New image starts fresh: drop any enhanced preview so we don't show a
    * stale enhanced texture for a different file, and clear the highlights. */
   p_win->u_enhance_mask = 0;
   _enhance_update_highlights(p_win);
#endif
   _load_current(p_win);
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
   /* Only update the viewer's texture here; do NOT force the stack to "large".
    * The stack is owned by the caller: file-open / toggle / grid-activate set
    * "large" themselves before loading, and directory-open sets "grid".
    * Forcing large here would yank a just-opened folder back out of the grid
    * view the moment its first image finishes loading. */
   ggaze_viewer_set_texture(GGAZE_VIEWER(p_win->p_viewer), p_tex);
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

/* --- M6: progressive low-res preview ------------------------------------ */

typedef struct {
   GgazeWindow *p_win;
   GdkTexture  *p_tex;
} ProgressInvoke;

static gboolean
_on_progress_main(gpointer p_data) {
   ProgressInvoke *p_pi = (ProgressInvoke *)p_data;
   /* Show the partial; the full result replaces it in _load_finish_cb. */
   ggaze_viewer_set_texture(GGAZE_VIEWER(p_pi->p_win->p_viewer), p_pi->p_tex);
   g_object_unref(p_pi->p_tex);
   g_object_unref(p_pi->p_win);
   g_free(p_pi);
   return (G_SOURCE_REMOVE);
}

static void
_load_progress_cb(GdkTexture *p_partial, gpointer p_data) {
   GgazeWindow    *p_win = GGAZE_WINDOW(p_data);
   ProgressInvoke *p_pi  = g_new(ProgressInvoke, 1);
   p_pi->p_win           = (GgazeWindow *)g_object_ref(p_win);
   p_pi->p_tex           = (GdkTexture *)g_object_ref(p_partial);
   g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, _on_progress_main, p_pi,
                              NULL);
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
         loader_load_async(p_file, p_win->p_prefetch_cancel, NULL, NULL,
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
   loader_load_async(p_cur, p_win->p_cancel, _load_progress_cb,
                     g_object_ref(p_win), _load_finish_cb, g_object_ref(p_win));
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
      /* Append the marked count so multi-selection is visible in the title. */
      guint u_marks = navigator_get_mark_count(p_win->p_nav);
      if (u_marks > 0 && c_title != NULL) {
         char *c_tmp =
            g_strdup_printf("%s  \u00b7  %u marked", c_title, u_marks);
         g_free(c_title);
         c_title = c_tmp;
      }
   }
#if GGAZE_HAVE_GEGL
   /* Append the enabled enhance preset names (comma-joined) when layered. */
   if (p_win->u_enhance_mask != 0 && p_win->p_enhancer != NULL &&
       c_title != NULL) {
      const GPtrArray *p_presets = enhancer_get_presets(p_win->p_enhancer);
      if (p_presets != NULL) {
         GString *p_str = g_string_new(NULL);
         for (guint i = 0; i < p_presets->len && i < 8; i++) {
            if ((p_win->u_enhance_mask & (guint8)(1u << i)) == 0) {
               continue;
            }
            const EnhancerPreset *p_pr =
               g_ptr_array_index((GPtrArray *)p_presets, i);
            if (p_str->len > 0) {
               g_string_append_c(p_str, ',');
            }
            g_string_append(p_str, p_pr->c_name);
         }
         if (p_str->len > 0) {
            char *c_tmp =
               g_strdup_printf("%s  \u00b7  %s", c_title, p_str->str);
            g_free(c_title);
            c_title = c_tmp;
         }
         g_string_free(p_str, TRUE);
      }
   }
#endif
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
#if GGAZE_HAVE_GEGL
   g_clear_pointer(&p_win->p_enhancer, enhancer_delete);
#endif
   /* p_stack/p_viewer/p_grid are GtkWidgets parented to the window; GTK
    * releases them. */
   G_OBJECT_CLASS(ggaze_window_parent_class)->dispose(p_obj);
}

static void
ggaze_window_class_init(GgazeWindowClass *p_klass) {
   GObjectClass *p_obj_class = G_OBJECT_CLASS(p_klass);
   p_obj_class->dispose      = ggaze_window_dispose;
}

/* Load the small ggaze stylesheet once (mark badge styling — the navigator's
 * mark API has no visual representation without it). */
static void
_ensure_css(void) {
   static gboolean b_done = FALSE;
   if (b_done) {
      return;
   }
   b_done                = TRUE;
   GtkCssProvider *p_css = gtk_css_provider_new();
   gtk_css_provider_load_from_string(
      p_css, "/* marked-thumbnail badge (multi-selection). */\n"
             ".ggaze-marked {\n"
             "  border: 2px solid #3584e4;\n"
             "  border-radius: 4px;\n"
             "  background-color: rgba(53, 132, 228, 0.15);\n"
             "}\n"
             "/* enabled enhance preset row highlight. */\n"
             ".ggaze-enhance-on {\n"
             "  background-color: #3584e4;\n"
             "  color: #ffffff;\n"
             "  font-weight: bold;\n"
             "}\n");
   GdkDisplay *p_disp = gdk_display_get_default();
   if (p_disp != NULL) {
      gtk_style_context_add_provider_for_display(
         p_disp, GTK_STYLE_PROVIDER(p_css),
         GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
   }
   g_object_unref(p_css);
}

static void
ggaze_window_init(GgazeWindow *p_win) {
   _ensure_css();
   p_win->p_cancel          = g_cancellable_new();
   p_win->p_prefetch_cancel = g_cancellable_new();
   p_win->p_cache           = texturecache_new(4);
   p_win->p_thumb           = thumbnail_new();
   p_win->p_trash           = NULL; /* created on open */
   p_win->p_grid            = NULL; /* created on open */
   p_win->i_grid_size       = 128;
#if GGAZE_HAVE_GEGL
   p_win->u_enhance_mask = 0; /* start on the original */
   p_win->p_enhancer     = enhancer_new();
#endif

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
   /* Layout: a horizontal box with the image area (overlay) taking the space,
    * and the enhance panel slotted in next to it (or overlaid in fullscreen).
    */
   p_win->p_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
   gtk_widget_set_hexpand(p_win->p_overlay, TRUE);
   gtk_widget_set_vexpand(p_win->p_overlay, TRUE);
   gtk_box_append(GTK_BOX(p_win->p_content), p_win->p_overlay);
   gtk_window_set_child(GTK_WINDOW(p_win), p_win->p_content);
#if GGAZE_HAVE_GEGL
   _build_enhance_panel(p_win);
#endif

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
   gboolean b_is_dir = (e_type == G_FILE_TYPE_DIRECTORY);
   if (b_is_dir) {
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

   /* Folder arg → start in the thumbnail grid (folder-to-grid behavior,
    * docs/ui-and-interactions.md 33-47); file arg → large view on that image.
    * _load_current is run either way so the large view is ready when toggled.
    */
   gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack),
                                    b_is_dir ? "grid" : "large");
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
