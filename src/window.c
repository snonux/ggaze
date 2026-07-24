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
#include "clipboard.h"
#include "gridview.h"
#include "info.h"
#include "loader/loader.h"
#include "mover.h"
#include "navigator.h"
#include "opener.h"
#include "prefs.h"
#include "runner.h"
#include "settings.h"
#include "shortcuts.h"
#include "texturecache.h"
#include "thumbnail.h"
#include "trash.h"
#include "viewer.h"
#if GGAZE_HAVE_GEGL
#include "enhancer.h"
#endif

/* Which of trash/move most recently succeeded, so `u` (win.undo) knows which
 * engine's undo to prefer when both could theoretically still undo (decision
 * P: one unified undo, no explicit shared module — window.c already owns
 * both Trash and Mover, so it is the natural place to track the ordering). */
typedef enum {
   GGAZE_LAST_NONE = 0,
   GGAZE_LAST_TRASH,
   GGAZE_LAST_MOVE
} GgazeLastDestructive;

struct _GgazeWindow {
   GtkApplicationWindow parent_instance;
   Navigator           *p_nav; /* current folder listing (NULL until open) */
   Settings            *p_settings; /* GSettings wrapper (owned) */
   Mover               *p_mover;    /* configured move destinations */
   Opener              *p_opener;   /* configured external editors */
   Runner              *p_runner;   /* configured shell scripts */
   GCancellable        *p_cancel;   /* visible load; cancelled on each nav */
   GCancellable *p_prefetch_cancel; /* prefetch round; cancelled on new round */
   TextureCache *p_cache;           /* bounded LRU of decoded GdkTextures */
   Thumbnail    *p_thumb;           /* TMS thumbnail cache */
   Trash        *p_trash;           /* ./Trash bin for the current folder */
   GtkWidget    *p_stack;           /* GtkStack: grid / large (viewer) */
   GtkWidget *p_open_ext_pop;   /* `e` open-external popover (NULL when none) */
   GtkWidget *p_run_script_pop; /* `!` run-script popover (NULL when none) */
   GtkWidget *p_move_pop;       /* `m` move-to-destination popover (NULL when
                                 * none) */
   GgazeLastDestructive e_last_destructive; /* trash vs move, for win.undo */
   GtkWidget           *p_viewer;           /* GgazeViewer — the large view */
   GgazeGrid *p_grid;      /* the thumbnail grid (the "grid" stack child) */
   int        i_grid_size; /* current thumbnail size (64-512, decision T) */
   GtkWidget *p_overlay;   /* GtkOverlay wrapping the stack (for info label) */
   GtkWidget *p_info_lbl;  /* info overlay label (auto-hides) */
   guint      u_info_hide; /* info auto-hide timeout id (0=none) */
   guint      u_slideshow; /* slideshow timeout id (0=off) */
   gboolean   b_fullscreen;
   guint      u_hdr_hide; /* fullscreen header auto-hide timeout */
   gboolean   b_disposed; /* set in dispose; async callbacks check it */
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
static void     _dismiss_info_for_nav(GgazeWindow *p_win);
static void     _show_status(GgazeWindow *p_win, const char *c_msg);
static gboolean _slideshow_tick(gpointer p_data);
static void     _apply_viewer_prefs(GgazeWindow *p_win);
static void     _load_engine_lists(GgazeWindow *p_win);
static void     _open_ext_destroy(GgazeWindow *p_win);
static void     _run_script_destroy(GgazeWindow *p_win);
static void     _move_destroy(GgazeWindow *p_win);
static void _on_viewer_navigate(GgazeViewer *p_v, gint i_dir, gpointer p_data);
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
      navigator_mark_removed(p_win->p_nav, p_cur);  /* dim; emits changed */
      navigator_next(p_win->p_nav);                 /* advance; emits changed */
      p_win->e_last_destructive = GGAZE_LAST_TRASH; /* for unified win.undo */
   } else {
      g_warning("ggaze: trash failed: %s", p_err->message);
      g_clear_error(&p_err);
   }
}

/* --- bulk-delete safety: captured, immutable target context -------------
 *
 * The >1-mark delete opens an async GtkAlertDialog. While it is pending, a
 * single-instance open / drop can replace the folder (ggaze_window_open swaps
 * p_nav). The dialog callback must therefore NOT re-read the navigator's
 * marks; it deletes the targets captured here at prompt time, and only if the
 * folder is still the same one (ggaze_window_delete_targets_still_current).
 */
typedef struct {
   GgazeWindow *p_win; /* owned ref: outlives the async dialog */
   GFile       *p_dir; /* owning directory at prompt time (owned) */
   GList *p_files;     /* captured target GFile* list (owned, transfer full) */
} _DeleteCtx;

static void
_delete_ctx_free(_DeleteCtx *p_ctx) {
   if (p_ctx == NULL) {
      return;
   }
   g_clear_object(&p_ctx->p_dir);
   g_list_free_full(p_ctx->p_files, (GDestroyNotify)g_object_unref);
   g_clear_object(&p_ctx->p_win);
   g_free(p_ctx);
}

/* TRUE iff p_win still navigates p_dir (the folder the captured delete targets
 * came from is still open), so a pending confirm dialog may safely delete
 * them. FALSE if the folder was replaced (single-instance open / drop) while
 * the dialog was pending. See window.h. */
gboolean
ggaze_window_delete_targets_still_current(GgazeWindow *p_win, GFile *p_dir) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), FALSE);
   g_return_val_if_fail(G_IS_FILE(p_dir), FALSE);
   if (p_win->p_nav == NULL) {
      return (FALSE);
   }
   GFile *p_now = navigator_get_dir(p_win->p_nav);
   if (p_now == NULL) {
      return (FALSE);
   }
   return (g_file_equal(p_now, p_dir));
}

/* Permanently delete each file in p_files (the captured or current set). */
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
   navigator_next(p_win->p_nav); /* skip removed entries -> next live (or
                                  * park at -1 when none remain, which clears
                                  * the viewer via the "changed" reload) */
}

/* Process a confirmed bulk-delete against the captured targets p_files
 * (borrowed; not freed here). Deletes EXACTLY those files iff p_win still
 * navigates p_dir; otherwise the folder was replaced while the confirm dialog
 * was pending and the delete is refused (no files touched). Returns TRUE iff
 * the delete proceeded. See window.h. */
gboolean
ggaze_window_delete_captured(GgazeWindow *p_win, GFile *p_dir, GList *p_files) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), FALSE);
   g_return_val_if_fail(G_IS_FILE(p_dir), FALSE);
   if (!ggaze_window_delete_targets_still_current(p_win, p_dir)) {
      g_warning(
         "ggaze: bulk delete refused \u2014 the folder was replaced while "
         "the confirm dialog was pending; no files deleted.");
      return (FALSE);
   }
   _do_delete_files(p_win, p_files);
   return (TRUE);
}

static void
_delete_confirm_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   GtkAlertDialog *p_dlg = GTK_ALERT_DIALOG(p_src);
   _DeleteCtx     *p_ctx = (_DeleteCtx *)p_data;
   GError         *p_err = NULL;
   gboolean        b_ok  = gtk_alert_dialog_choose_finish(p_dlg, p_res, &p_err);
   if (b_ok) {
      /* Delete the captured targets (NOT a re-read of p_nav marks): if the
       * folder was replaced while this dialog was pending, the safety check
       * inside ggaze_window_delete_captured refuses and no files are touched.
       */
      ggaze_window_delete_captured(p_ctx->p_win, p_ctx->p_dir, p_ctx->p_files);
   }
   g_clear_error(&p_err);
   _delete_ctx_free(p_ctx);
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
      /* Confirm before deleting >1 marked image (decision #38). Capture an
       * immutable snapshot of the targets NOW so the async dialog callback
       * deletes exactly the files named by the prompt, even if the folder is
       * replaced (single-instance open / drop) while it is pending. */
      GFile *p_dir   = navigator_get_dir(p_win->p_nav);
      GList *p_marks = navigator_get_marks(p_win->p_nav); /* transfer full */
      if (p_dir == NULL || p_marks == NULL) {
         g_list_free_full(p_marks, (GDestroyNotify)g_object_unref);
         return;
      }
      GtkAlertDialog *p_dlg =
         gtk_alert_dialog_new("Permanently delete %u marked images?", u_marks);
      gtk_alert_dialog_set_buttons(p_dlg,
                                   (const char *[]){"Cancel", "Delete", NULL});
      _DeleteCtx *p_ctx = g_new(_DeleteCtx, 1);
      p_ctx->p_win      = (GgazeWindow *)g_object_ref(p_win);
      p_ctx->p_dir      = (GFile *)g_object_ref(p_dir);
      p_ctx->p_files    = p_marks; /* owned by the context now */
      gtk_alert_dialog_choose(p_dlg, GTK_WINDOW(p_win), NULL,
                              _delete_confirm_cb, p_ctx);
      g_object_unref(p_dlg);
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

/* Undo the last trash (restore from ./Trash to its original path). On
 * success the navigator is rescanned so the restored file is un-dimmed (its
 * path reappears on disk, so _relist un-removes it) and the header/grid
 * refresh via "changed". */
static void
_undo_trash(GgazeWindow *p_win) {
   GError *p_err = NULL;
   if (trash_restore_last(p_win->p_trash, &p_err)) {
      navigator_rescan(p_win->p_nav); /* re-list; restored file un-removed */
      _show_status(p_win, "Restored from Trash");
      p_win->e_last_destructive = GGAZE_LAST_NONE;
   } else {
      g_clear_error(&p_err);
   }
}

/* Undo the last move (move the recorded set back to their original paths).
 * Same navigator-rescan approach as _undo_trash: the files reappear at their
 * original path in the (possibly different) folder they came from, so a
 * rescan of the CURRENTLY open folder only visibly restores them if that is
 * where they were moved from; either way the move itself is undone on disk.
 */
static void
_undo_move(GgazeWindow *p_win) {
   GError *p_err = NULL;
   if (mover_undo_last(p_win->p_mover, &p_err)) {
      if (p_win->p_nav != NULL) {
         navigator_rescan(p_win->p_nav);
      }
      _show_status(p_win, "Move undone");
      p_win->e_last_destructive = GGAZE_LAST_NONE;
   } else {
      if (p_err != NULL) {
         g_warning("ggaze: move undo failed: %s", p_err->message);
      }
      g_clear_error(&p_err);
   }
}

/* `u`: undo the last destructive action, whichever of trash/move happened
 * most recently (decision P: one unified undo). Reopening a folder resets
 * BOTH engines' undo state and e_last_destructive together (ggaze_window_open
 * clears p_trash and calls mover_clear_last), so a stale record from a folder
 * the user is no longer looking at can never be the preferred branch below.
 * The fallback branches below instead serve the legitimate WITHIN-session
 * case: e.g. move a file, then trash a file (trash is now preferred), undo
 * once (undoes the trash, resets e_last_destructive to NONE) -- the move is
 * still undoable in the CURRENT folder, so a second `u` should undo that
 * too rather than silently do nothing. */
static void
_action_undo(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL) {
      return;
   }
   gboolean b_trash_ok =
      p_win->p_trash != NULL && trash_can_undo(p_win->p_trash);
   gboolean b_move_ok =
      p_win->p_mover != NULL && mover_can_undo(p_win->p_mover);
   if (p_win->e_last_destructive == GGAZE_LAST_MOVE && b_move_ok) {
      _undo_move(p_win);
   } else if (p_win->e_last_destructive == GGAZE_LAST_TRASH && b_trash_ok) {
      _undo_trash(p_win);
   } else if (b_move_ok) {
      _undo_move(p_win);
   } else if (b_trash_ok) {
      _undo_trash(p_win);
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

/* win.copy (Ctrl+c): put the current picture on the clipboard. With marks,
 * copy the marked files as text/uri-list (+ text/plain) so file managers and
 * file-aware apps can paste them. With no marks, copy the DISPLAYED image
 * pixels as image/png — the viewer's current texture, which is the enhanced
 * preview when an enhance preset is active, else the original (docs/ui-and-
 * interactions.md "Copy to clipboard"). The texture is already decoded, so the
 * PNG encode (gdk_texture_save_to_png_bytes) runs synchronously and is fast
 * enough not to block the UI on a re-decode. The viewer only ever holds the
 * texture for navigator.current (last-write-wins invariant), so copying it is
 * tied to the current load by construction. The decision is factored into
 * ggaze_window_get_copy_provider so it can be tested without driving the
 * (display-backend-dependent) system clipboard. */
static void
_action_copy(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow        *p_win  = GGAZE_WINDOW(p_data);
   GdkContentProvider *p_prov = ggaze_window_get_copy_provider(p_win);
   if (p_prov == NULL) {
      g_warning("ggaze: copy \u2014 nothing to copy");
      return;
   }
   GdkClipboard *p_clip = gtk_widget_get_clipboard(GTK_WIDGET(p_win));
   gdk_clipboard_set_content(p_clip, p_prov);
   g_object_unref(p_prov);
   guint u_marks =
      (p_win->p_nav != NULL) ? navigator_get_mark_count(p_win->p_nav) : 0;
   if (u_marks > 0) {
      char *c_msg =
         g_strdup_printf("Copied %u file%s", u_marks, u_marks == 1 ? "" : "s");
      _show_status(p_win, c_msg);
      g_free(c_msg);
   } else {
      _show_status(p_win, "Copied image");
   }
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
   "                <property name=\"accelerator\">e</property>\n"
   "                <property name=\"title\">Open in external "
   "program</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">exclam</property>\n"
   "                <property name=\"title\">Run a configured shell "
   "script</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">m</property>\n"
   "                <property name=\"title\">Move marked/current to a "
   "destination</property>\n"
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
   "                <property name=\"title\">Undo last trash or "
   "move</property>\n"
   "              </object>\n"
   "            </child>\n"
   "            <child>\n"
   "              <object class=\"GtkShortcutsShortcut\">\n"
   "                <property name=\"accelerator\">Primary+c</property>\n"
   "                <property name=\"title\">Copy image / marked "
   "files</property>\n"
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
      gdouble d_delay = 3.0;
      if (p_win->p_settings != NULL) {
         d_delay = settings_get_slideshow_delay(p_win->p_settings);
      }
      if (d_delay < 0.1) {
         d_delay = 0.1;
      }
      p_win->u_slideshow =
         g_timeout_add((guint)(d_delay * 1000.0), _slideshow_tick, p_win);
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
   /* Just fired: don't call g_source_remove on our own (already-firing)
    * source below, only zero the id so a future show/hide can tell no timer
    * is pending. */
   p_win->u_info_hide = 0;
   _hide_info(p_win);
   return (G_SOURCE_REMOVE);
}

/* Cancel the info-overlay auto-hide timer if one is pending. Every path that
 * changes the overlay's state (a fresh show, a status message reusing the
 * same label, or a navigation-triggered hide) must call this before touching
 * u_info_hide again, so a stale timer can never fire after the state has
 * already moved on and hide/re-hide something it no longer owns. Safe to
 * call when none is pending (u_info_hide == 0 guard). */
static void
_info_cancel_timer(GgazeWindow *p_win) {
   if (p_win->u_info_hide != 0) {
      g_source_remove(p_win->u_info_hide);
      p_win->u_info_hide = 0;
   }
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
   _info_cancel_timer(p_win);
   p_win->u_info_hide = g_timeout_add_seconds(5, _info_hide_tick, p_win);
}

static void
_hide_info(GgazeWindow *p_win) {
   gtk_widget_set_visible(p_win->p_info_lbl, FALSE);
}

/* Hide the info overlay in response to the current file changing (any
 * navigation: prev/next/first/last, slideshow auto-advance, grid selection,
 * trash/delete/move advancing past a target, rescan after undo, ...) -- see
 * nav_changed_cb, the single choke point every one of those paths funnels
 * through via Navigator's "changed" signal. The overlay must never keep
 * showing a PREVIOUS file's EXIF/dimensions after a new one is displayed, so
 * this cancels any pending auto-hide timer (there is nothing left for it to
 * hide) and hides the label unconditionally; a subsequent _show_status call
 * later in the same handler chain (e.g. move/undo's status line) re-shows it
 * with its own fresh timer, so this never fights a legitimate immediate
 * re-show. */
static void
_dismiss_info_for_nav(GgazeWindow *p_win) {
   _info_cancel_timer(p_win);
   _hide_info(p_win);
}

/* Show a brief transient status line in the info overlay label (the project
 * has no toast infrastructure yet; see docs/ui-and-interactions.md). Reuses
 * the info label + its auto-hide timer so a copy confirms visually without a
 * separate widget. The label is positioned over the stack and visible in both
 * large and grid views. */
static void
_show_status(GgazeWindow *p_win, const char *c_msg) {
   gtk_label_set_text(GTK_LABEL(p_win->p_info_lbl), c_msg);
   gtk_widget_set_visible(p_win->p_info_lbl, TRUE);
   _info_cancel_timer(p_win);
   p_win->u_info_hide = g_timeout_add_seconds(2, _info_hide_tick, p_win);
}

/* Free a temp (name, command) pair struct (OpenerProg / RunnerScript share
 * the same two-pointer layout). Used as a GPtrArray free func for the throw-
 * away arrays built while feeding settings into the engines. */
static void
_name_cmd_free(gpointer p) {
   OpenerProg *d = (OpenerProg *)p;
   if (d == NULL) {
      return;
   }
   g_free(d->c_name);
   g_free(d->c_command);
   g_free(d);
}

#if GGAZE_HAVE_GEGL
static void
_preset_tmp_free(gpointer p) {
   EnhancerPreset *d = (EnhancerPreset *)p;
   if (d == NULL) {
      return;
   }
   g_free(d->c_name);
   g_free(d->c_graph);
   g_free(d);
}
#endif

/* Apply the scalar viewer preferences (background, scroll behavior) from the
 * settings wrapper to the large-view widget. Called at init and after the
 * Preferences dialog commits a change. */
static void
_apply_viewer_prefs(GgazeWindow *p_win) {
   g_return_if_fail(p_win != NULL);
   if (p_win->p_settings == NULL || p_win->p_viewer == NULL) {
      return;
   }
   ggaze_viewer_set_background(GGAZE_VIEWER(p_win->p_viewer),
                               settings_get_background(p_win->p_settings));
   ggaze_viewer_set_scroll_behavior(
      GGAZE_VIEWER(p_win->p_viewer),
      settings_get_scroll_behavior(p_win->p_settings));
}

/* Feed the configured a(ss) lists into the mover/opener/runner engines (and,
 * when GEGL is built in, the user enhance presets into the enhancer). Called
 * at init so the engines are ready before any folder is opened. */
static void
_load_engine_lists(GgazeWindow *p_win) {
   g_return_if_fail(p_win != NULL);
   if (p_win->p_settings == NULL) {
      return;
   }
   if (p_win->p_mover != NULL) {
      GPtrArray *p = settings_get_destinations(p_win->p_settings);
      mover_set_dests(p_win->p_mover, p);
      g_ptr_array_unref(p);
   }
   if (p_win->p_opener != NULL) {
      GPtrArray *p  = settings_get_editors(p_win->p_settings);
      GPtrArray *pp = g_ptr_array_new_with_free_func(_name_cmd_free);
      for (guint i = 0; i < p->len; i++) {
         const SettingsPair *pr = g_ptr_array_index(p, i);
         OpenerProg         *np = g_new(OpenerProg, 1);
         np->c_name             = g_strdup(pr->c_name);
         np->c_command          = g_strdup(pr->c_value);
         g_ptr_array_add(pp, np);
      }
      opener_set_progs(p_win->p_opener, pp);
      g_ptr_array_unref(pp);
      g_ptr_array_unref(p);
   }
   if (p_win->p_runner != NULL) {
      GPtrArray *p  = settings_get_scripts(p_win->p_settings);
      GPtrArray *pp = g_ptr_array_new_with_free_func(_name_cmd_free);
      for (guint i = 0; i < p->len; i++) {
         const SettingsPair *pr = g_ptr_array_index(p, i);
         RunnerScript       *np = g_new(RunnerScript, 1);
         np->c_name             = g_strdup(pr->c_name);
         np->c_command          = g_strdup(pr->c_value);
         g_ptr_array_add(pp, np);
      }
      runner_set_scripts(p_win->p_runner, pp);
      g_ptr_array_unref(pp);
      g_ptr_array_unref(p);
   }
#if GGAZE_HAVE_GEGL
   if (p_win->p_enhancer != NULL) {
      /* Rebuild the enhancer preset list as: the existing built-in presets
       * (deep-copied) followed by the user-defined graph presets from
       * settings. enhancer_set_presets deep-copies again into its own array,
       * so the temp array here owns and frees every entry. User-graph
       * application is still TODO in enhancer_apply, but the list is plumbed
       * so the popup (M10) and the Preferences UI see the configured entries.
       */
      const GPtrArray *p_cur = enhancer_get_presets(p_win->p_enhancer);
      GPtrArray       *pp    = g_ptr_array_new_with_free_func(_preset_tmp_free);
      for (guint i = 0; p_cur != NULL && i < p_cur->len; i++) {
         const EnhancerPreset *pr = g_ptr_array_index((GPtrArray *)p_cur, i);
         EnhancerPreset       *np = g_new0(EnhancerPreset, 1);
         np->c_name               = g_strdup(pr->c_name);
         np->c_graph              = g_strdup(pr->c_graph);
         np->i_builtin            = pr->i_builtin;
         g_ptr_array_add(pp, np);
      }
      GPtrArray *p_user = settings_get_enhance_presets(p_win->p_settings);
      for (guint i = 0; i < p_user->len; i++) {
         const SettingsPair *pr = g_ptr_array_index(p_user, i);
         EnhancerPreset     *np = g_new0(EnhancerPreset, 1);
         np->c_name             = g_strdup(pr->c_name);
         np->c_graph            = g_strdup(pr->c_value);
         np->i_builtin          = 0;
         g_ptr_array_add(pp, np);
      }
      enhancer_set_presets(p_win->p_enhancer, pp);
      g_ptr_array_unref(pp);
      g_ptr_array_unref(p_user);
   }
#endif
}

/* Scroll-wheel navigate (GGAZE_SCROLL_NAVIGATE): advance the navigator. */
static void
_on_viewer_navigate(GgazeViewer *p_v, gint i_dir, gpointer p_data) {
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL) {
      return;
   }
   if (i_dir >= 0) {
      navigator_next(p_win->p_nav);
   } else {
      navigator_prev(p_win->p_nav);
   }
}

static void
_action_preferences(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_settings == NULL) {
      return;
   }
   prefs_show(p_win->p_settings, GTK_WIDGET(p_win));
}

/* --- M8: open in external program (`e`) --------------------------------
 *
 * `e` pops up a GtkPopover listing the configured editors, each with an
 * auto-assigned hotkey in list order: 1..9, then 0, then a..z (cap 36,
 * decision O). A hotkey or a row click launches opener_launch on the
 * ORIGINAL current file (navigator_get_current, not an enhanced preview);
 * Esc or an outside click cancels. The popover is its own GtkNative /
 * GtkShortcutManager, so while it is open the parent window's GLOBAL-scope
 * shortcuts (enhance-1..8, `a`, `s`, navigation, ...) do NOT fire for key
 * events on the popover's surface — the popover's own key controller sees
 * them exclusively (see gtkshortcutmanager.c: GtkPopover implements
 * GtkShortcutManager and global/managed scopes are limited to the same
 * native). opener_launch is detached (GSubprocess), so ggaze stays
 * responsive. Parse/launch failures are g_warning'd (the project has no
 * toast infra yet; see docs/ui-and-interactions.md "Opening in an external
 * program").
 */

/* Auto-assigned hotkey character for popup row index u_idx, in list order:
 * 1..9, then 0, then a..z. Returns 0 for an index beyond the 36-hotkey
 * range (such entries are shown without a hotkey and are click-only). Shared
 * by the open-external (`e`) and run-script (`!`) popovers (decision O). */
static char
_popup_hotkey_char(guint u_idx) {
   if (u_idx < 9) {
      return ((char)('1' + u_idx));
   }
   if (u_idx == 9) {
      return ('0');
   }
   if (u_idx < 36) {
      return ((char)('a' + (u_idx - 10)));
   }
   return (0);
}

/* Map a keyval to a popup row index (1-9 -> 0-8, 0 -> 9, a-z -> 10-35),
 * or -1 for any other key. Used by the open-external and run-script popover
 * key controllers. */
static gint
_popup_key_to_index(guint u_keyval) {
   if (u_keyval >= GDK_KEY_1 && u_keyval <= GDK_KEY_9) {
      return ((gint)(u_keyval - GDK_KEY_1));
   }
   if (u_keyval == GDK_KEY_0) {
      return (9);
   }
   if (u_keyval >= GDK_KEY_a && u_keyval <= GDK_KEY_z) {
      return ((gint)(10 + (u_keyval - GDK_KEY_a)));
   }
   return (-1);
}

/* Synchronously tear down the current open-external popover (unparent +
 * clear the field). Safe to call when none is open. The popover's "closed"
 * handler (autohide / outside-click dismissal) also routes here. */
static void
_open_ext_destroy(GgazeWindow *p_win) {
   if (p_win->p_open_ext_pop == NULL) {
      return;
   }
   GtkWidget *p_pop = p_win->p_open_ext_pop;
   p_win->p_open_ext_pop =
      NULL; /* first, so a re-entrant "closed" is a no-op */
   gtk_widget_unparent(p_pop);
}

/* "closed" (outside-click / autohide dismissal): tear down synchronously. */
static void
_open_ext_closed_cb(GtkPopover *p_pop, gpointer p_data) {
   (void)p_pop;
   _open_ext_destroy(GGAZE_WINDOW(p_data));
}

/* Row click (mouse): launch that editor on the original current file, then
 * close the popover. */
static void
_open_ext_row_clicked_cb(GtkButton *p_btn, gpointer p_data) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   guint u_idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(p_btn), "idx"));
   ggaze_window_open_external_index(p_win, u_idx);
   _open_ext_destroy(p_win);
}

/* Popover key controller: Esc cancels; a bare digit/letter hotkey launches
 * the matching editor on the original current file and closes the popover.
 * Modified keys (Ctrl+a, Shift+...) are propagated so they are not swallowed.
 */
static gboolean
_open_ext_key_pressed_cb(GtkEventControllerKey *p_c, guint u_keyval, guint u_kc,
                         GdkModifierType e_state, gpointer p_data) {
   (void)p_c;
   (void)u_kc;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (u_keyval == GDK_KEY_Escape) {
      _open_ext_destroy(p_win);
      return (GDK_EVENT_STOP);
   }
   if (e_state != 0) {
      return (GDK_EVENT_PROPAGATE);
   }
   gint i_idx = _popup_key_to_index(u_keyval);
   if (i_idx < 0) {
      return (GDK_EVENT_PROPAGATE);
   }
   const GPtrArray *p_progs = opener_get_progs(p_win->p_opener);
   if (p_progs == NULL || (guint)i_idx >= p_progs->len) {
      return (GDK_EVENT_PROPAGATE); /* no editor bound to that hotkey */
   }
   ggaze_window_open_external_index(p_win, (guint)i_idx);
   _open_ext_destroy(p_win);
   return (GDK_EVENT_STOP);
}

/* Build and pop up the open-external popover listing the configured editors.
 * If no editors are configured, shows a single message row pointing at
 * Preferences (`,`) instead. */
static void
_action_open_external(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL) {
      return; /* nothing open */
   }
   /* Toggle: a second `e` while the popover is up just closes it. */
   if (p_win->p_open_ext_pop != NULL) {
      _open_ext_destroy(p_win);
      return;
   }

   const GPtrArray *p_progs =
      p_win->p_opener != NULL ? opener_get_progs(p_win->p_opener) : NULL;
   GtkWidget *p_pop = gtk_popover_new();
   gtk_popover_set_position(GTK_POPOVER(p_pop), GTK_POS_TOP);
   gtk_popover_set_pointing_to(GTK_POPOVER(p_pop),
                               &(const GdkRectangle){0, 0, 1, 1});
   g_signal_connect(GTK_POPOVER(p_pop), "closed",
                    G_CALLBACK(_open_ext_closed_cb), p_win);
   /* Key controller on the popover (capture phase): the popover is its own
    * native / shortcut scope, so this sees the hotkeys without the parent
    * window's GLOBAL shortcuts intercepting them. */
   GtkEventController *p_kc = gtk_event_controller_key_new();
   gtk_event_controller_set_propagation_phase(p_kc, GTK_PHASE_CAPTURE);
   g_signal_connect(p_kc, "key-pressed", G_CALLBACK(_open_ext_key_pressed_cb),
                    p_win);
   gtk_widget_add_controller(p_pop, p_kc);

   GtkWidget *p_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
   gtk_widget_set_margin_start(p_box, 8);
   gtk_widget_set_margin_end(p_box, 8);
   gtk_widget_set_margin_top(p_box, 8);
   gtk_widget_set_margin_bottom(p_box, 8);
   gtk_popover_set_child(GTK_POPOVER(p_pop), p_box);

   if (p_progs == NULL || p_progs->len == 0) {
      GtkWidget *p_lbl =
         gtk_label_new("No editors configured. Press , to open Preferences.");
      gtk_widget_set_halign(p_lbl, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(p_box), p_lbl);
   } else {
      char *c_title = NULL;
      {
         GFile *p_cur = navigator_get_current(p_win->p_nav);
         if (p_cur != NULL) {
            char *c_name = g_file_get_basename(p_cur);
            c_title      = g_strdup_printf("Open %s in:", c_name);
            g_free(c_name);
         }
      }
      GtkWidget *p_lbl = gtk_label_new(c_title != NULL ? c_title : "Open in:");
      gtk_widget_set_halign(p_lbl, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(p_box), p_lbl);
      g_free(c_title);

      guint u_n = p_progs->len;
      if (u_n > 36) {
         u_n = 36; /* cap hotkeys at 1-9,0,a-z (decision O) */
      }
      for (guint i = 0; i < u_n; i++) {
         const OpenerProg *p_pr = g_ptr_array_index((GPtrArray *)p_progs, i);
         char              c_hk = _popup_hotkey_char(i);
         char             *c_lbl =
            g_strdup_printf("%c  %s", c_hk != 0 ? c_hk : ' ',
                            p_pr->c_name != NULL ? p_pr->c_name : "(unnamed)");
         GtkWidget *p_btn = gtk_button_new_with_label(c_lbl);
         gtk_widget_set_halign(p_btn, GTK_ALIGN_START);
         g_object_set_data(G_OBJECT(p_btn), "idx", GUINT_TO_POINTER(i));
         g_signal_connect(p_btn, "clicked",
                          G_CALLBACK(_open_ext_row_clicked_cb), p_win);
         gtk_box_append(GTK_BOX(p_box), p_btn);
         g_free(c_lbl);
         if (i == 0) {
            gtk_widget_grab_focus(p_btn); /* ensure the popover gets keys */
         }
      }
   }

   gtk_widget_set_parent(p_pop, p_win->p_stack);
   p_win->p_open_ext_pop = p_pop;
   gtk_popover_popup(GTK_POPOVER(p_pop));
}

/* Launch editor u_idx on the ORIGINAL current file. See window.h. */
gboolean
ggaze_window_open_external_index(GgazeWindow *p_win, guint u_idx) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), FALSE);
   if (p_win->p_nav == NULL || p_win->p_opener == NULL) {
      return (FALSE);
   }
   const GPtrArray *p_progs = opener_get_progs(p_win->p_opener);
   if (p_progs == NULL || u_idx >= p_progs->len) {
      return (FALSE);
   }
   GFile *p_cur = navigator_get_current(p_win->p_nav);
   if (p_cur == NULL) {
      return (FALSE);
   }
   const OpenerProg *p_prog = g_ptr_array_index((GPtrArray *)p_progs, u_idx);
   GError           *p_err  = NULL;
   gboolean b_ok = opener_launch(p_win->p_opener, p_cur, p_prog, &p_err);
   if (!b_ok) {
      g_warning("ggaze: open-external '%s' failed: %s",
                p_prog->c_name != NULL ? p_prog->c_name : "(unnamed)",
                p_err != NULL ? p_err->message : "(no detail)");
      g_clear_error(&p_err);
   }
   return (b_ok);
}

/* --- M8: run a configured shell script (`!`) ----------------------------
 *
 * `!` pops up a GtkPopover listing the configured scripts (settings `scripts`
 * a(ss)), each with an auto-assigned hotkey in list order: 1..9, then 0, then
 * a..z (cap 36, decision O). A hotkey or a row click runs runner_run on the
 * ORIGINAL current file (%f = navigator_get_current) and the current folder
 * (%d = navigator_get_dir) via /bin/sh -c, asynchronously (GSubprocess) so the
 * UI stays responsive. Esc or an outside click cancels. The popover is its own
 * GtkNative / GtkShortcutManager, so the parent window's GLOBAL-scope shortcuts
 * do not fire for key events on the popover's surface (same property the
 * open-external popover relies on). Empty-scripts case: a message row points
 * at Preferences (`,`), like the empty-editors handling.
 *
 * On completion the navigator is rescanned (scripts may add/remove files) and
 * a status line reports success or the exit status / error. The rescan is the
 * key correctness concern: the script ran against a SPECIFIC folder, captured
 * at launch time. While it ran, a single-instance open / drop can replace
 * p_nav with a different folder. The async completion callback must NOT rescan
 * the new folder (the script never touched it). It validates, eu0-style, that
 * p_win still navigates the captured folder before rescanning; otherwise it
 * only shows the completion status. The callback also holds an owned ref to
 * the window and checks b_disposed, because the window may have been closed
 * while the script ran (dispose destroys the child widgets, so the status
 * label would dangle without that guard).
 */

/* Captured at launch time for the async completion callback. Owns its refs so
 * it outlives the script even if the window's folder is replaced or the window
 * is closed. Freed in every path of _run_done_cb. */
typedef struct {
   GgazeWindow *p_win;  /* owned ref; outlives the script */
   GFile       *p_dir;  /* owned: folder the script ran against */
   char        *c_name; /* owned: script name, for the status message */
} _RunCtx;

static void
_run_ctx_free(_RunCtx *p_ctx) {
   if (p_ctx == NULL) {
      return;
   }
   g_clear_object(&p_ctx->p_win);
   g_clear_object(&p_ctx->p_dir);
   g_clear_pointer(&p_ctx->c_name, g_free);
   g_free(p_ctx);
}

/* Async completion: finish the subprocess, rescan ONLY if the window still
 * navigates the captured folder, and report success / exit status / error.
 * Safe if the folder was replaced (no rescan of the new folder) or the window
 * was closed (b_disposed: no widget touch). */
static void
_run_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   _RunCtx     *p_ctx  = (_RunCtx *)p_data;
   GgazeWindow *p_win  = p_ctx->p_win;
   GError      *p_err  = NULL;
   int          i_code = runner_run_finish(NULL, p_res, &p_err);

   /* Rescan only the folder the script ran against, and only if the window
    * is still alive and still navigates it. If the folder was replaced
    * (single-instance open / drop) the script's changes belong to the OLD
    * folder, not the new one, so do not rescan the new listing. */
   if (!p_win->b_disposed && p_win->p_nav != NULL) {
      GFile *p_now = navigator_get_dir(p_win->p_nav);
      if (p_now != NULL && g_file_equal(p_now, p_ctx->p_dir)) {
         navigator_rescan(p_win->p_nav); /* emits "changed" -> grid + reload */
      }
   }

   /* Feedback. Skip the status label if the window was closed (its child
    * widgets were destroyed in dispose). */
   if (!p_win->b_disposed) {
      if (i_code == 0) {
         char *c_msg = g_strdup_printf("Script '%s' done", p_ctx->c_name);
         _show_status(p_win, c_msg);
         g_free(c_msg);
      } else if (i_code > 0) {
         char *c_msg = g_strdup_printf("Script '%s' failed (exit %d)",
                                       p_ctx->c_name, i_code);
         _show_status(p_win, c_msg);
         g_free(c_msg);
      } else { /* -1: launch / wait error */
         char *c_msg =
            g_strdup_printf("Script '%s' failed: %s", p_ctx->c_name,
                            p_err != NULL ? p_err->message : "(no detail)");
         _show_status(p_win, c_msg);
         g_free(c_msg);
      }
   }
   g_clear_error(&p_err);
   _run_ctx_free(p_ctx);
}

/* Synchronously tear down the current run-script popover (unparent + clear the
 * field). Safe to call when none is open. The popover's "closed" handler
 * (autohide / outside-click dismissal) also routes here. */
static void
_run_script_destroy(GgazeWindow *p_win) {
   if (p_win->p_run_script_pop == NULL) {
      return;
   }
   GtkWidget *p_pop = p_win->p_run_script_pop;
   p_win->p_run_script_pop =
      NULL; /* first, so a re-entrant "closed" is a no-op */
   gtk_widget_unparent(p_pop);
}

static void
_run_script_closed_cb(GtkPopover *p_pop, gpointer p_data) {
   (void)p_pop;
   _run_script_destroy(GGAZE_WINDOW(p_data));
}

/* Row click (mouse): run that script on the original current file + folder,
 * then close the popover. */
static void
_run_script_row_clicked_cb(GtkButton *p_btn, gpointer p_data) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   guint u_idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(p_btn), "idx"));
   ggaze_window_run_script_index(p_win, u_idx);
   _run_script_destroy(p_win);
}

/* Popover key controller: Esc cancels; a bare digit/letter hotkey runs the
 * matching script and closes the popover. Modified keys are propagated. */
static gboolean
_run_script_key_pressed_cb(GtkEventControllerKey *p_c, guint u_keyval,
                           guint u_kc, GdkModifierType e_state,
                           gpointer p_data) {
   (void)p_c;
   (void)u_kc;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (u_keyval == GDK_KEY_Escape) {
      _run_script_destroy(p_win);
      return (GDK_EVENT_STOP);
   }
   if (e_state != 0) {
      return (GDK_EVENT_PROPAGATE);
   }
   gint i_idx = _popup_key_to_index(u_keyval);
   if (i_idx < 0) {
      return (GDK_EVENT_PROPAGATE);
   }
   const GPtrArray *p_scripts =
      p_win->p_runner != NULL ? runner_get_scripts(p_win->p_runner) : NULL;
   if (p_scripts == NULL || (guint)i_idx >= p_scripts->len) {
      return (GDK_EVENT_PROPAGATE); /* no script bound to that hotkey */
   }
   ggaze_window_run_script_index(p_win, (guint)i_idx);
   _run_script_destroy(p_win);
   return (GDK_EVENT_STOP);
}

/* Build and pop up the run-script popover listing the configured scripts. If
 * none are configured, shows a single message row pointing at Preferences
 * (`,`) instead. */
static void
_action_run_script(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL) {
      return; /* nothing open */
   }
   /* Toggle: a second `!` while the popover is up just closes it. */
   if (p_win->p_run_script_pop != NULL) {
      _run_script_destroy(p_win);
      return;
   }

   const GPtrArray *p_scripts =
      p_win->p_runner != NULL ? runner_get_scripts(p_win->p_runner) : NULL;
   GtkWidget *p_pop = gtk_popover_new();
   gtk_popover_set_position(GTK_POPOVER(p_pop), GTK_POS_TOP);
   gtk_popover_set_pointing_to(GTK_POPOVER(p_pop),
                               &(const GdkRectangle){0, 0, 1, 1});
   g_signal_connect(GTK_POPOVER(p_pop), "closed",
                    G_CALLBACK(_run_script_closed_cb), p_win);
   GtkEventController *p_kc = gtk_event_controller_key_new();
   gtk_event_controller_set_propagation_phase(p_kc, GTK_PHASE_CAPTURE);
   g_signal_connect(p_kc, "key-pressed", G_CALLBACK(_run_script_key_pressed_cb),
                    p_win);
   gtk_widget_add_controller(p_pop, p_kc);

   GtkWidget *p_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
   gtk_widget_set_margin_start(p_box, 8);
   gtk_widget_set_margin_end(p_box, 8);
   gtk_widget_set_margin_top(p_box, 8);
   gtk_widget_set_margin_bottom(p_box, 8);
   gtk_popover_set_child(GTK_POPOVER(p_pop), p_box);

   if (p_scripts == NULL || p_scripts->len == 0) {
      GtkWidget *p_lbl =
         gtk_label_new("No scripts configured. Press , to open Preferences.");
      gtk_widget_set_halign(p_lbl, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(p_box), p_lbl);
   } else {
      GtkWidget *p_lbl = gtk_label_new("Run script:");
      gtk_widget_set_halign(p_lbl, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(p_box), p_lbl);

      guint u_n = p_scripts->len;
      if (u_n > 36) {
         u_n = 36; /* cap hotkeys at 1-9,0,a-z (decision O) */
      }
      for (guint i = 0; i < u_n; i++) {
         const RunnerScript *p_sc =
            g_ptr_array_index((GPtrArray *)p_scripts, i);
         char  c_hk = _popup_hotkey_char(i);
         char *c_lbl =
            g_strdup_printf("%c  %s", c_hk != 0 ? c_hk : ' ',
                            p_sc->c_name != NULL ? p_sc->c_name : "(unnamed)");
         GtkWidget *p_btn = gtk_button_new_with_label(c_lbl);
         gtk_widget_set_halign(p_btn, GTK_ALIGN_START);
         g_object_set_data(G_OBJECT(p_btn), "idx", GUINT_TO_POINTER(i));
         g_signal_connect(p_btn, "clicked",
                          G_CALLBACK(_run_script_row_clicked_cb), p_win);
         gtk_box_append(GTK_BOX(p_box), p_btn);
         g_free(c_lbl);
         if (i == 0) {
            gtk_widget_grab_focus(p_btn); /* ensure the popover gets keys */
         }
      }
   }

   gtk_widget_set_parent(p_pop, p_win->p_stack);
   p_win->p_run_script_pop = p_pop;
   gtk_popover_popup(GTK_POPOVER(p_pop));
}

/* Run script u_idx (0-based, in the configured scripts list order) on the
 * ORIGINAL current file (%f) and the current folder (%d), asynchronously. The
 * completion callback (_run_done_cb) rescans the captured folder and reports
 * status. Returns TRUE iff the script was started; FALSE (with a g_warning +
 * status) on a launch error, an out-of-range index, or when nothing is open /
 * no scripts are configured. This is the testable run path the `!` popup (and
 * its hotkeys) invoke. See window.h. */
gboolean
ggaze_window_run_script_index(GgazeWindow *p_win, guint u_idx) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), FALSE);
   if (p_win->p_nav == NULL || p_win->p_runner == NULL) {
      return (FALSE);
   }
   const GPtrArray *p_scripts = runner_get_scripts(p_win->p_runner);
   if (p_scripts == NULL || u_idx >= p_scripts->len) {
      return (FALSE);
   }
   GFile *p_cur = navigator_get_current(p_win->p_nav);
   GFile *p_dir = navigator_get_dir(p_win->p_nav);
   if (p_dir == NULL) {
      return (FALSE);
   }
   const RunnerScript *p_sc  = g_ptr_array_index((GPtrArray *)p_scripts, u_idx);
   _RunCtx            *p_ctx = g_new(_RunCtx, 1);
   p_ctx->p_win              = (GgazeWindow *)g_object_ref(p_win);
   p_ctx->p_dir              = (GFile *)g_object_ref(p_dir);
   p_ctx->c_name  = g_strdup(p_sc->c_name != NULL ? p_sc->c_name : "(unnamed)");
   GError  *p_err = NULL;
   gboolean b_ok = runner_run(p_win->p_runner, p_cur, p_dir, p_sc, _run_done_cb,
                              p_ctx, &p_err);
   if (!b_ok) {
      g_warning("ggaze: run-script '%s' failed to start: %s", p_ctx->c_name,
                p_err != NULL ? p_err->message : "(no detail)");
      char *c_msg =
         g_strdup_printf("Script '%s' failed to start: %s", p_ctx->c_name,
                         p_err != NULL ? p_err->message : "(no detail)");
      if (!p_win->b_disposed) {
         _show_status(p_win, c_msg);
      }
      g_free(c_msg);
      g_clear_error(&p_err);
      _run_ctx_free(p_ctx);
      return (FALSE);
   }
   char *c_msg = g_strdup_printf("Running '%s'…", p_ctx->c_name);
   if (!p_win->b_disposed) {
      _show_status(p_win, c_msg);
   }
   g_free(c_msg);
   return (TRUE);
}

/* --- M8: move-to-destination (marks-or-current, collision-aware, undoable)
 * -------------------------------------------------------------------------
 */

/* The files a move should act on: every marked file if any are marked, else
 * just the current file (docs/ui-and-interactions.md "Selection & moving").
 * Transfer full: caller frees with g_list_free_full(..., g_object_unref). */
static GList *
_move_targets(GgazeWindow *p_win) {
   if (p_win->p_nav == NULL) {
      return (NULL);
   }
   if (navigator_get_mark_count(p_win->p_nav) > 0) {
      return (navigator_get_marks(p_win->p_nav)); /* transfer full */
   }
   GFile *p_cur = navigator_get_current(p_win->p_nav);
   if (p_cur == NULL) {
      return (NULL);
   }
   return (g_list_prepend(NULL, g_object_ref(p_cur)));
}

/* After mover_move() returns (success or partial failure), mark every target
 * that actually left its original path as removed (dimmed; mirrors trash) so
 * the navigator/grid never show a file that is no longer where they think it
 * is. Checking disk state rather than trusting the overall return value
 * handles mover_move's partial-failure case (some files moved before an
 * error hit a later one in the list) correctly. Returns the count removed. */
static guint
_move_mark_removed(GgazeWindow *p_win, GList *p_files) {
   guint u_removed = 0;
   for (GList *p_it = p_files; p_it != NULL; p_it = p_it->next) {
      GFile *p_f = G_FILE(p_it->data);
      if (!g_file_query_exists(p_f, NULL)) {
         navigator_mark_removed(p_win->p_nav, p_f); /* dim; emits changed */
         u_removed++;
      }
   }
   return (u_removed);
}

/* Report the outcome of a mover_move() call via _show_status (+ g_warning on
 * any failure): a clean success ("Moved N files to X"), a clean failure
 * ("Move to X failed: ..."), or — mover_move's partial-failure case — a
 * count of how many of the N requested actually moved before the error hit
 * a later one in the list ("Moved M of N files to X; then failed: ..."). */
static void
_move_report(GgazeWindow *p_win, const MoverDest *p_dest, gboolean b_ok,
             guint u_moved, guint u_n, GError *p_err) {
   if (b_ok) {
      char *c_msg = g_strdup_printf("Moved %u file%s to %s", u_n,
                                    u_n == 1 ? "" : "s", p_dest->c_name);
      _show_status(p_win, c_msg);
      g_free(c_msg);
      return;
   }
   g_warning("ggaze: move to '%s' failed: %s", p_dest->c_name,
             p_err != NULL ? p_err->message : "(no detail)");
   char *c_msg =
      u_moved > 0
         ? g_strdup_printf("Moved %u of %u files to %s; then failed: %s",
                           u_moved, u_n, p_dest->c_name,
                           p_err != NULL ? p_err->message : "(no detail)")
         : g_strdup_printf("Move to %s failed: %s", p_dest->c_name,
                           p_err != NULL ? p_err->message : "(no detail)");
   _show_status(p_win, c_msg);
   g_free(c_msg);
}

/* Move u_idx (0-based, in the configured destinations list order) — see
 * window.h. */
gboolean
ggaze_window_move_index(GgazeWindow *p_win, guint u_idx) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), FALSE);
   if (p_win->p_nav == NULL || p_win->p_mover == NULL) {
      return (FALSE);
   }
   const GPtrArray *p_dests = mover_get_dests(p_win->p_mover);
   if (p_dests == NULL || u_idx >= p_dests->len) {
      return (FALSE);
   }
   GList *p_files = _move_targets(p_win);
   if (p_files == NULL) {
      return (FALSE);
   }
   const MoverDest *p_dest = g_ptr_array_index((GPtrArray *)p_dests, u_idx);
   guint            u_n    = g_list_length(p_files);
   GError          *p_err  = NULL;
   gboolean         b_ok = mover_move(p_win->p_mover, p_files, p_dest, &p_err);
   guint            u_moved = _move_mark_removed(p_win, p_files);
   if (u_moved > 0) {
      p_win->e_last_destructive = GGAZE_LAST_MOVE;
      navigator_next(p_win->p_nav); /* advance past the moved set */
   }
   _move_report(p_win, p_dest, b_ok, u_moved, u_n, p_err);
   g_clear_error(&p_err);
   g_list_free_full(p_files, (GDestroyNotify)g_object_unref);
   return (b_ok);
}

/* Synchronously tear down the current move popover (unparent + clear the
 * field). Safe to call when none is open. */
static void
_move_destroy(GgazeWindow *p_win) {
   if (p_win->p_move_pop == NULL) {
      return;
   }
   GtkWidget *p_pop  = p_win->p_move_pop;
   p_win->p_move_pop = NULL; /* first, so a re-entrant "closed" is a no-op */
   gtk_widget_unparent(p_pop);
}

static void
_move_closed_cb(GtkPopover *p_pop, gpointer p_data) {
   (void)p_pop;
   _move_destroy(GGAZE_WINDOW(p_data));
}

/* Row click (mouse): move to that destination, then close the popover. */
static void
_move_row_clicked_cb(GtkButton *p_btn, gpointer p_data) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   guint u_idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(p_btn), "idx"));
   ggaze_window_move_index(p_win, u_idx);
   _move_destroy(p_win);
}

/* Popover key controller: Esc cancels; a bare digit/letter hotkey moves to
 * the matching destination and closes the popover. Modified keys are
 * propagated so they are not swallowed. */
static gboolean
_move_key_pressed_cb(GtkEventControllerKey *p_c, guint u_keyval, guint u_kc,
                     GdkModifierType e_state, gpointer p_data) {
   (void)p_c;
   (void)u_kc;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (u_keyval == GDK_KEY_Escape) {
      _move_destroy(p_win);
      return (GDK_EVENT_STOP);
   }
   if (e_state != 0) {
      return (GDK_EVENT_PROPAGATE);
   }
   gint i_idx = _popup_key_to_index(u_keyval);
   if (i_idx < 0) {
      return (GDK_EVENT_PROPAGATE);
   }
   const GPtrArray *p_dests = mover_get_dests(p_win->p_mover);
   if (p_dests == NULL || (guint)i_idx >= p_dests->len) {
      return (GDK_EVENT_PROPAGATE); /* no destination bound to that hotkey */
   }
   ggaze_window_move_index(p_win, (guint)i_idx);
   _move_destroy(p_win);
   return (GDK_EVENT_STOP);
}

/* Build the popover's content box: a title label followed by one row per
 * destination (hotkey + name), or a single message row when none are
 * configured. Split out of _action_move to keep that under ~30 lines. */
static GtkWidget *
_move_build_box(GgazeWindow *p_win, const GPtrArray *p_dests, guint u_count) {
   GtkWidget *p_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
   gtk_widget_set_margin_start(p_box, 8);
   gtk_widget_set_margin_end(p_box, 8);
   gtk_widget_set_margin_top(p_box, 8);
   gtk_widget_set_margin_bottom(p_box, 8);

   if (p_dests == NULL || p_dests->len == 0) {
      GtkWidget *p_lbl = gtk_label_new(
         "No destinations configured. Press , to open Preferences.");
      gtk_widget_set_halign(p_lbl, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(p_box), p_lbl);
      return (p_box);
   }

   char *c_title =
      g_strdup_printf("Move %u image%s to:", u_count, u_count == 1 ? "" : "s");
   GtkWidget *p_lbl = gtk_label_new(c_title);
   gtk_widget_set_halign(p_lbl, GTK_ALIGN_START);
   gtk_box_append(GTK_BOX(p_box), p_lbl);
   g_free(c_title);

   guint u_n = p_dests->len;
   if (u_n > 36) {
      u_n = 36; /* cap hotkeys at 1-9,0,a-z (decision O) */
   }
   for (guint i = 0; i < u_n; i++) {
      const MoverDest *p_d  = g_ptr_array_index((GPtrArray *)p_dests, i);
      char             c_hk = _popup_hotkey_char(i);
      char            *c_lbl =
         g_strdup_printf("%c  %s", c_hk != 0 ? c_hk : ' ',
                         p_d->c_name != NULL ? p_d->c_name : "(unnamed)");
      GtkWidget *p_btn = gtk_button_new_with_label(c_lbl);
      gtk_widget_set_halign(p_btn, GTK_ALIGN_START);
      g_object_set_data(G_OBJECT(p_btn), "idx", GUINT_TO_POINTER(i));
      g_signal_connect(p_btn, "clicked", G_CALLBACK(_move_row_clicked_cb),
                       p_win);
      gtk_box_append(GTK_BOX(p_box), p_btn);
      g_free(c_lbl);
      if (i == 0) {
         gtk_widget_grab_focus(p_btn); /* ensure the popover gets keys */
      }
   }
   return (p_box);
}

/* Build and pop up the move popover listing the configured destinations
 * (`m`). Toggles closed on a second press, same as `e`/`!`. */
static void
_action_move(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL || p_win->p_mover == NULL) {
      return;
   }
   if (p_win->p_move_pop != NULL) {
      _move_destroy(p_win);
      return;
   }
   GList *p_targets = _move_targets(p_win);
   guint  u_count   = g_list_length(p_targets);
   g_list_free_full(p_targets, (GDestroyNotify)g_object_unref);
   if (u_count == 0) {
      _show_status(p_win, "Nothing to move");
      return;
   }

   const GPtrArray *p_dests = mover_get_dests(p_win->p_mover);
   GtkWidget       *p_pop   = gtk_popover_new();
   gtk_popover_set_position(GTK_POPOVER(p_pop), GTK_POS_TOP);
   gtk_popover_set_pointing_to(GTK_POPOVER(p_pop),
                               &(const GdkRectangle){0, 0, 1, 1});
   g_signal_connect(GTK_POPOVER(p_pop), "closed", G_CALLBACK(_move_closed_cb),
                    p_win);
   GtkEventController *p_kc = gtk_event_controller_key_new();
   gtk_event_controller_set_propagation_phase(p_kc, GTK_PHASE_CAPTURE);
   g_signal_connect(p_kc, "key-pressed", G_CALLBACK(_move_key_pressed_cb),
                    p_win);
   gtk_widget_add_controller(p_pop, p_kc);

   gtk_popover_set_child(GTK_POPOVER(p_pop),
                         _move_build_box(p_win, p_dests, u_count));
   gtk_widget_set_parent(p_pop, p_win->p_stack);
   p_win->p_move_pop = p_pop;
   gtk_popover_popup(GTK_POPOVER(p_pop));
}

static const GActionEntry ACTIONS[] = {
   {.name = "prev", .activate = _action_prev},
   {.name = "next", .activate = _action_next},
   {.name = "first", .activate = _action_first},
   {.name = "last", .activate = _action_last},
   {.name = "open", .activate = _action_open},
   {.name = "open-external", .activate = _action_open_external},
   {.name = "run-script", .activate = _action_run_script},
   {.name = "move", .activate = _action_move},
   {.name = "quit", .activate = _action_quit},
   {.name = "trash", .activate = _action_trash},
   {.name = "delete", .activate = _action_delete},
   {.name = "undo", .activate = _action_undo},
   {.name = "toggle-view", .activate = _action_toggle_view},
   {.name = "mark", .activate = _action_mark},
   {.name = "mark-all", .activate = _action_mark_all},
   {.name = "mark-range", .activate = _action_mark_range},
   {.name = "copy", .activate = _action_copy},
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
   {.name = "preferences", .activate = _action_preferences},
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
   /* This signal is the single choke point every navigation path funnels
    * through (prev/next/first/last, slideshow auto-advance, grid selection,
    * trash/delete/move advancing past a target, rescan after undo, ...). The
    * info overlay (`i`) shows the PREVIOUS current file's EXIF/dimensions
    * until its 5s timer expires unless dismissed here, so a caller could
    * navigate away and see stale metadata for whatever is newly displayed.
    * Hiding unconditionally (rather than refreshing to the new file) is the
    * simplest contract that can never show wrong-file data; any status
    * message a caller shows immediately after this (e.g. move/undo) still
    * wins because it re-shows the label with its own fresh timer afterward. */
   _dismiss_info_for_nav(p_win);
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

/* One LoadCtx per loader_load_async call. It carries the source GFile
 * identity so the main-thread progress/finish callbacks can enforce
 * last-write-wins: a result whose file no longer equals navigator.current
 * is dropped instead of overwriting the viewer. The GTask always invokes
 * _load_finish_cb (even on cancellation), which is the sole owner that frees
 * the ctx, so the refs are balanced regardless of whether progress fired. */
typedef struct {
   GgazeWindow *p_win;  /* ref'd; outlives the load */
   GFile       *p_file; /* ref'd; the file being loaded */
} LoadCtx;

typedef struct {
   GgazeWindow *p_win;
   GFile       *p_file;
   GdkTexture  *p_tex;
} ProgressInvoke;

static void
_load_ctx_free(LoadCtx *p_ctx) {
   if (p_ctx == NULL) {
      return;
   }
   g_object_unref(p_ctx->p_win);
   g_object_unref(p_ctx->p_file);
   g_free(p_ctx);
}

static gboolean
_on_progress_main(gpointer p_data) {
   ProgressInvoke *p_pi = (ProgressInvoke *)p_data;
   /* Last-write-wins: show the partial only if its source file is still the
    * current one. A stale partial from a superseded (cancelled) load is
    * dropped here so it cannot overwrite the viewer while another image is
    * current; the full result replaces it in _load_finish_cb. */
   GFile *p_cur = navigator_get_current(p_pi->p_win->p_nav);
   if (p_cur != NULL && g_file_equal(p_cur, p_pi->p_file)) {
      ggaze_viewer_set_texture(GGAZE_VIEWER(p_pi->p_win->p_viewer),
                               p_pi->p_tex);
   }
   g_object_unref(p_pi->p_tex);
   g_object_unref(p_pi->p_file);
   g_object_unref(p_pi->p_win);
   g_free(p_pi);
   return (G_SOURCE_REMOVE);
}

static void
_load_progress_cb(GdkTexture *p_partial, gpointer p_data) {
   LoadCtx        *p_ctx = (LoadCtx *)p_data;
   ProgressInvoke *p_pi  = g_new(ProgressInvoke, 1);
   p_pi->p_win           = (GgazeWindow *)g_object_ref(p_ctx->p_win);
   p_pi->p_file          = (GFile *)g_object_ref(p_ctx->p_file);
   p_pi->p_tex           = (GdkTexture *)g_object_ref(p_partial);
   g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, _on_progress_main, p_pi,
                              NULL);
}

/* Visible-load callback: show only if this is still the current file
 * (last-write-wins), then cache it and prefetch neighbours. */
static void
_load_finish_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   LoadCtx     *p_ctx = (LoadCtx *)p_data;
   GgazeWindow *p_win = p_ctx->p_win;
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
      _load_ctx_free(p_ctx);
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
   _load_ctx_free(p_ctx);
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
    * Last-write-wins is enforced in _load_progress_cb (partial) and
    * _load_finish_cb (full result), both via the LoadCtx's source GFile. The
    * single LoadCtx is shared by both callbacks and freed in _load_finish_cb,
    * which the GTask always invokes. */
   g_cancellable_cancel(p_win->p_cancel);
   g_clear_object(&p_win->p_cancel);
   p_win->p_cancel = g_cancellable_new();
   LoadCtx *p_ctx  = g_new(LoadCtx, 1);
   p_ctx->p_win    = (GgazeWindow *)g_object_ref(p_win);
   p_ctx->p_file   = (GFile *)g_object_ref(p_cur);
   loader_load_async(p_cur, p_win->p_cancel, _load_progress_cb, p_ctx,
                     _load_finish_cb, p_ctx);
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
   p_win->b_disposed = TRUE; /* async callbacks check this before touching UI */
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
   _open_ext_destroy(p_win);
   _run_script_destroy(p_win);
   _move_destroy(p_win);
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
   g_clear_pointer(&p_win->p_runner, runner_delete);
   g_clear_pointer(&p_win->p_opener, opener_delete);
   g_clear_pointer(&p_win->p_mover, mover_delete);
   g_clear_pointer(&p_win->p_settings, settings_delete);
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
   p_win->p_settings        = settings_new();
   p_win->p_mover           = mover_new();
   p_win->p_opener          = opener_new();
   p_win->p_runner          = runner_new();
   if (p_win->p_settings != NULL) {
      p_win->i_grid_size =
         CLAMP(settings_get_thumbnail_size(p_win->p_settings), 64, 512);
   }

#if GGAZE_HAVE_GEGL
   p_win->u_enhance_mask = 0; /* start on the original */
   p_win->p_enhancer     = enhancer_new();
#endif
   /* Feed the configured a(ss) lists into the engines now that all of them
    * (incl. the GEGL enhancer) exist. */
   _load_engine_lists(p_win);

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
   g_signal_connect(p_win->p_viewer, "navigate",
                    G_CALLBACK(_on_viewer_navigate), p_win);
   _apply_viewer_prefs(p_win);

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

   /* Apply the sort/wrap/hide-raw preferences from settings (defaults if the
    * wrapper is absent). */
   GgazeSort e_sort         = GGAZE_SORT_NAME;
   gboolean  b_wrap         = TRUE;
   gboolean  b_hide_raw     = TRUE;
   gboolean  b_hide_trashed = FALSE;
   if (p_win->p_settings != NULL) {
      e_sort         = settings_get_sort(p_win->p_settings);
      b_wrap         = settings_get_wrap(p_win->p_settings);
      b_hide_raw     = settings_get_hide_raw(p_win->p_settings);
      b_hide_trashed = settings_get_hide_trashed(p_win->p_settings);
   }
   p_win->p_nav = navigator_new(p_dir, e_sort, b_wrap, b_hide_raw);
   g_clear_pointer(&p_win->p_trash, trash_delete);
   /* A move recorded against the folder just left must not be undoable once
    * we are looking at a different folder (it would silently move a file
    * back into a folder no longer on screen) -- drop mover's undo state the
    * same way p_trash gets a fresh one below, and clear the unified-undo
    * preference so a stale enum value can't point at either. */
   mover_clear_last(p_win->p_mover);
   p_win->e_last_destructive = GGAZE_LAST_NONE;
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
   p_win->p_grid   = GGAZE_GRID(ggaze_grid_new(
      p_win->p_nav, p_win->p_thumb, p_win->i_grid_size, b_hide_trashed));
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

GtkWidget *
ggaze_window_get_info_label(GgazeWindow *p_win) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), NULL);
   return (p_win->p_info_lbl);
}

/* The content provider win.copy would set on the clipboard, without touching
 * the clipboard itself (so the decision is testable independently of the
 * display-backend-dependent system clipboard). Marks -> text/uri-list (+
 * text/plain); no marks -> the DISPLAYED texture as image/png (the enhanced
 * preview when a preset is active, else the original). NULL when nothing is
 * open / no marks and no texture displayed. See window.h. */
GdkContentProvider *
ggaze_window_get_copy_provider(GgazeWindow *p_win) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), NULL);
   if (p_win->p_nav == NULL) {
      return (NULL);
   }
   guint u_marks = navigator_get_mark_count(p_win->p_nav);
   if (u_marks > 0) {
      GList *p_marks = navigator_get_marks(p_win->p_nav); /* transfer full */
      GdkContentProvider *p_prov = clipboard_build_uri_provider(p_marks);
      g_list_free_full(p_marks, (GDestroyNotify)g_object_unref);
      return (p_prov);
   }
   GdkTexture *p_tex = ggaze_viewer_get_texture(GGAZE_VIEWER(p_win->p_viewer));
   if (p_tex == NULL) {
      return (NULL);
   }
   return (clipboard_build_texture_provider(p_tex));
}
