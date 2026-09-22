/*:*
 * ggaze — main window
 *
 * GgazeWindow : GtkApplicationWindow owns the layout (header bar with
 * navigation buttons + main menu; a GtkStack with the grid, the large
 * GgazeViewer and an empty-state page; the info overlay) and routes every
 * win.* action to the helper that owns the logic: viewload (large-view
 * loading), info-overlay (status/EXIF card), save-gate and delete-confirm
 * (modal prompts), enhance-ctrl (GEGL), and the plain-C engines (navigator,
 * trash, mover, opener, runner, undo). The header title carries
 * "filename · n/total" over the live images. See docs/architecture.md.
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
#include "delete-confirm.h"
#include "fileops.h"
#include "info-overlay.h"
#include "mover.h"
#include "navigator.h"
#include "opener.h"
#include "pathutil.h"
#include "popup_list.h"
#include "prefs.h"
#include "runner.h"
#include "save-gate.h"
#include "settings.h"
#include "shortcuts.h"
#include "thumbnail.h"
#include "trash.h"
#include "undo.h"
#include "viewer.h"
#include "viewload.h"
#include "enhance-ctrl.h"
#if GGAZE_HAVE_GEGL
#include "enhance-ui.h"
#include "enhancer.h"
#include "enhancer-gegl.h"
#endif

/* The two views the stack can show, plus the empty-state page. Owning the
 * stack child names here (and only here) keeps the "which view am I in"
 * question out of the action handlers. */
typedef enum {
   GGAZE_VIEW_GRID,
   GGAZE_VIEW_LARGE,
   GGAZE_VIEW_EMPTY, /* no folder open / no images to show */
} GgazeViewMode;

static const char *VIEW_NAMES[] = {"grid", "large", "empty"};

/* How long a second Esc in the grid counts as "yes, quit" (ms). */
#define GGAZE_ESC_QUIT_WINDOW_MS 2000

struct _GgazeWindow {
   GtkApplicationWindow parent_instance;
   Navigator           *p_nav; /* current folder listing (NULL until open) */
   Settings            *p_settings; /* GSettings wrapper (owned) */
   Mover               *p_mover;    /* configured move destinations */
   Opener              *p_opener;   /* configured external editors */
   Runner              *p_runner;   /* configured shell scripts */
   ViewLoad            *p_viewload; /* large-view load pipeline: texture LRU,
                                     * one-active-load cancel, prefetch,
                                     * last-write-wins (viewload.h) */
   Thumbnail *p_thumb;              /* TMS thumbnail cache */
   Trash     *p_trash;              /* ./Trash bin for the current folder */
   GtkWidget *p_stack;              /* GtkStack: grid / large (viewer) */
   PopupList *p_open_ext_pop;   /* `e` open-external popover (NULL when none) */
   PopupList *p_run_script_pop; /* `!` run-script popover (NULL when none) */
   PopupList *p_move_pop;       /* `m` move-to-destination popover (NULL when
                                 * none) */
   Undo      *p_undo;           /* unified-undo coordinator (Trash vs Mover) */
   FileOps   *p_fileops;        /* trash/delete/move/undo policy (fileops.h) */
   GtkWidget *p_viewer;         /* GgazeViewer — the large view */
   GgazeGrid *p_grid;      /* the thumbnail grid (the "grid" stack child) */
   int        i_grid_size; /* current thumbnail size (64-512, decision T) */
   GtkWidget *p_overlay;   /* GtkOverlay wrapping the stack (for info label) */
   GtkWidget *p_side_slot; /* GtkBox beside the overlay that the enhance
                            * side panel is appended to while open; shown
                            * only in the large view (_set_view) */
   InfoOverlay *p_info;    /* info card + status line over the stack */
   guint        u_slideshow;     /* slideshow timeout id (0=off) */
   gint64       i_esc_at;        /* monotonic us of the last grid Esc (two-step
                                  * quit), 0 = none */
   GtkWidget     *p_status_page; /* AdwStatusPage: the "empty" stack child */
   GtkWidget     *p_menu_btn;    /* header-bar main menu (F10) */
   gboolean       b_disposed;    /* set in dispose; async callbacks check it */
   DeleteConfirm *p_delete_confirm; /* bulk-delete confirm flow
                                     * (captured targets + outstanding
                                     * dialog + folder-identity re-check). */
   SaveGate *p_save_gate;           /* Save/Discard/Cancel prompt gate: every
                                     * discard/overwrite continuation funnels through
                                     * save_gate_maybe_save_then(). Owns the prompt
                                     * state machine (cancellable, outstanding-flag,
                                     * parked-request slot); the dirty/save/discard
                                     * decisions stay here and reach it through the
                                     * host ops. Exists in every build (a no-op when
                                     * GEGL is not built in: is_dirty is always FALSE). */
   EnhanceCtrl
      *p_enhance_ctrl; /* GEGL enhance feature controller (SRP): owns the
                        * preset mask, the in-flight apply/preview
                        * cancellables + generation counters, the cached
                        * enhanced texture, the hold-Space flag, the saved
                        * flag, the side panel + its cards/pictures, and the
                        * Enhancer engine. window.c forwards only the
                        * a/s/digit/Space/0/Esc actions and a few choke-point
                        * queries (is_dirty, override_texture,
                        * nav_changed). NULL when GEGL is not built in. */
};

G_DEFINE_TYPE(GgazeWindow, ggaze_window, GTK_TYPE_APPLICATION_WINDOW)

/* --- forward decls ------------------------------------------------------- */
static void     _load_current(GgazeWindow *p_win);
static void     _show_texture(GgazeWindow *p_win, GdkTexture *p_tex);
static void     _update_header(GgazeWindow *p_win);
static void     _on_grid_activate(GgazeGrid *p_grid, gpointer p_data);
static void     _show_info(GgazeWindow *p_win);
static void     _dismiss_info_for_nav(GgazeWindow *p_win);
static void     _show_status(GgazeWindow *p_win, const char *c_msg);
static void     _sync_info_plot(GgazeWindow *p_win);
static gboolean _slideshow_tick(gpointer p_data);
static void     _apply_viewer_prefs(GgazeWindow *p_win);
static void     _load_engine_lists(GgazeWindow *p_win);
static void     _set_grid_size(GgazeWindow *p_win, int i_size);
static void     _update_empty_state(GgazeWindow *p_win);
static void     _action_enter_large(GSimpleAction *p_a, GVariant *p_v,
                                    gpointer p_data);
static void _action_menu(GSimpleAction *p_a, GVariant *p_v, gpointer p_data);
static void _action_empty_trash(GSimpleAction *p_a, GVariant *p_v,
                                gpointer p_data);
static void _on_viewer_navigate(GgazeViewer *p_v, gint i_dir, gpointer p_data);

/* POPOVER KEYBOARD FOCUS -- who focuses the first row (dw0).
 *
 * Nothing here does. All four builders below hand the finished popover to
 * gtk_popover_popup(), and GtkPopover focuses the first focusable child
 * itself: gtk_popover_show() ends with
 *
 *    if (priv->autohide)
 *      if (!gtk_widget_get_focus_child (widget))
 *        gtk_widget_child_focus (widget, GTK_DIR_TAB_FORWARD);
 *
 * (gtk/gtkpopover.c:1164-1168, gtk 4.22.4). autohide defaults to TRUE
 * (gtkpopover.c:1019) and ggaze never unsets it, so that arm always applies.
 * The first focusable child is the first row button -- the "Open in:" /
 * "Run script:" / "Move N images to:" title label above it is a GtkLabel and
 * takes no focus.
 *
 * Each builder used to call gtk_widget_grab_focus() on its i == 0 row with
 * the comment "ensure the popover gets keys". That call was a guaranteed
 * no-op in all four: it ran inside the row loop, i.e. BEFORE
 * gtk_widget_set_parent(p_pop, p_win->p_stack), so the button had no GtkRoot
 * yet and gtk_widget_grab_focus() returned FALSE at its
 * `widget->priv->root == NULL` guard (gtk/gtkwidget.c:5158-5161).
 *
 * Measured on gtk 4.22.4 with a probe replicating this exact build order,
 * comparing the grab where it was ("asis"), moved after set_parent
 * ("moved"), and removed ("none"), reading gtk_root_get_focus() after popup:
 *
 *   X11 (Xvfb), toplevel presented or not : all three -> first row button.
 *   Wayland, toplevel presented           : all three -> first row button.
 *   Wayland, toplevel NEVER presented     : asis/none -> no focus, and the
 *                                           popup surface never maps either;
 *                                           moved -> first row button focused
 *                                           inside an UNMAPPED popover.
 *
 * So the grab was observationally identical to having no grab in every
 * condition, and moving it after set_parent would have been strictly worse
 * in the one condition where they differ: it would put keyboard focus into a
 * popover the compositor never showed (gdk_popup_present() refuses a popup
 * whose parent surface has no xdg_surface, so gtk_popover_show() returns at
 * its `if (!present_popup (popover)) return;`, gtkpopover.c:1159-1160, before
 * both the map and the focus arm). The grabs were therefore deleted rather
 * than moved. Do not reintroduce one: if a popover ever does need a specific
 * row focused, grab it after set_parent AND only once the popup has mapped. */
#if GGAZE_HAVE_GEGL
static gboolean _space_pressed_cb(GtkEventControllerKey *p_c, guint u_keyval,
                                  guint u_kc, GdkModifierType e_state,
                                  gpointer p_data);
static gboolean _space_released_cb(GtkEventControllerKey *p_c, guint u_keyval,
                                   guint u_kc, GdkModifierType e_state,
                                   gpointer p_data);
static void     _init_enhance_state(GgazeWindow *p_win);
#endif

/* Navigation continuations handed to save_gate_maybe_save_then and run once
 * the (GEGL) save/discard dialog resolves. They are trivial wrappers over the
 * public navigation API and do not depend on GEGL, so they are always compiled
 * (the _action_prev/next/first/last handlers reference them regardless of the
 * GEGL build configuration). Their data is the GgazeWindow itself (not owned
 * by the continuation -- the SaveGate's _SaveCtx holds its own ref for the
 * dialog's lifetime), so they are registered with a NULL fn_free_data. */
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
/* The quit continuation shared by win.quit and the native "close-request"
 * handler. It lives up here with its navigation siblings because the SaveGate
 * has to recognise it by identity: a prompt whose own continuation closes the
 * window may not retry a queued request afterwards (round 4, finding r -- see
 * save-gate.c _save_prompt_show/_save_prompt_flush). The gate receives this
 * function as SaveGateHostOps.quit_continuation, so it is compiled either way
 * and compared by pointer in every build. */
static gboolean
_proceed_quit(gpointer p_data) {
   gtk_window_close(GTK_WINDOW(p_data));
   return (G_SOURCE_REMOVE);
}

/* --- capture-at-request-time contexts ------------------------------------
 *
 * A window + ONE GFile captured when the user asked for something, which is
 * the shape every deferred continuation that acts on a single specific file
 * needs. The rule it exists to enforce is the one _DeleteCtx (further down)
 * already documents for the bulk-delete confirm dialog: a callback that runs
 * after an async dialog must act on the target the prompt was RAISED FOR, and
 * must never re-read navigator.current at answer time.
 *
 * That matters because GTK4 modality is INPUT-only. The slideshow timer is a
 * plain g_timeout_add and the folder's GFileMonitor is a plain GSource, so
 * both keep firing behind a modal prompt and can move navigator.current out
 * from under it. Re-reading the navigator in the continuation then acted on a
 * file the user never selected -- for `d`/`D` that meant trashing/deleting the
 * wrong image (tu0 review round 3, finding h).
 *
 * The ctx is freed by _maybe_save_then via _file_ctx_free, never by the
 * continuation itself, so the paths that never reach the continuation
 * (Cancel/dismiss/failed Save/a dropped parked request) release it too. */
typedef struct {
   GgazeWindow *p_win;  /* owned ref */
   GFile       *p_file; /* owned ref, NULL when nothing was current */
} _FileCtx;

static _FileCtx *
_file_ctx_new(GgazeWindow *p_win, GFile *p_file) {
   _FileCtx *p_ctx = g_new(_FileCtx, 1);
   p_ctx->p_win    = (GgazeWindow *)g_object_ref(p_win);
   p_ctx->p_file   = p_file != NULL ? (GFile *)g_object_ref(p_file) : NULL;
   return (p_ctx);
}

static void
_file_ctx_free(gpointer p_data) {
   _FileCtx *p_ctx = (_FileCtx *)p_data;
   g_object_unref(p_ctx->p_win);
   g_clear_object(&p_ctx->p_file);
   g_free(p_ctx);
}

/* Continuation for _grid_select_gate (below): applies the deferred
 * navigator.current change once Save/Discard/Cancel resolves. */
static gboolean
_proceed_grid_select(gpointer p_data) {
   _FileCtx *p_ctx = (_FileCtx *)p_data;
   if (p_ctx->p_win->p_nav != NULL && p_ctx->p_file != NULL) {
      navigator_set_current_file(p_ctx->p_win->p_nav, p_ctx->p_file);
   }
   return (G_SOURCE_REMOVE);
}

/* The files a marks-or-current action (`D` delete, `m` move) acts on,
 * captured ONCE at key-press time -- see fileops_capture_targets for why the
 * decision itself is perishable. Transfer full. */
static GList *
_capture_targets(GgazeWindow *p_win) {
   return (fileops_capture_targets(p_win->p_fileops));
}

/* A window + a captured target set, the multi-file counterpart of _FileCtx.
 * Embedded rather than allocated by the contexts that need extra fields of
 * their own (_MoveIdxCtx), so both share one init/clear pair. */
typedef struct {
   GgazeWindow *p_win;   /* owned ref */
   GList       *p_files; /* captured targets (owned, transfer full) */
} _FilesCtx;

/* Takes ownership of p_files (which may legitimately be NULL: `D` on an empty
 * listing captures nothing, and the continuation then does nothing). */
static void
_files_ctx_init(_FilesCtx *p_ctx, GgazeWindow *p_win, GList *p_files) {
   p_ctx->p_win   = (GgazeWindow *)g_object_ref(p_win);
   p_ctx->p_files = p_files;
}

static void
_files_ctx_clear(_FilesCtx *p_ctx) {
   g_list_free_full(p_ctx->p_files, (GDestroyNotify)g_object_unref);
   p_ctx->p_files = NULL;
   g_clear_object(&p_ctx->p_win);
}

static _FilesCtx *
_files_ctx_new(GgazeWindow *p_win, GList *p_files) {
   _FilesCtx *p_ctx = g_new(_FilesCtx, 1);
   _files_ctx_init(p_ctx, p_win, p_files);
   return (p_ctx);
}

/* _maybe_save_then owns the ctx and frees it through this on EVERY exit path
 * (including Cancel/dismiss/failed Save/a dropped parked request), so the
 * window ref it holds is never leaked. */
static void
_files_ctx_free(gpointer p_data) {
   _FilesCtx *p_ctx = (_FilesCtx *)p_data;
   _files_ctx_clear(p_ctx);
   g_free(p_ctx);
}

/* TRUE while the `D` >1-target delete-confirm dialog is outstanding.
 *
 * The cancellable slot IS the state -- there is no separate flag to drift out
 * of step with it. It is set in _delete_confirm_ask immediately before
 * gtk_alert_dialog_choose(), and cleared in _delete_confirm_cb, which that
 * choose() always reaches: a button press, a dismissal and a cancel all
 * complete the dialog's GTask, and nothing else can leave it unfinished (see
 * _delete_confirm_ask). So "non-NULL" means "a dialog really is on screen and
 * really is answerable", which is what _on_close_request needs it to mean --
 * a slot that could stick non-NULL with no dialog up would be a window the
 * user cannot close. */
static gboolean
_delete_confirm_outstanding(GgazeWindow *p_win) {
   return (delete_confirm_outstanding(p_win->p_delete_confirm));
}

/* TRUE while ANY modal dialog this window owns is outstanding.
 *
 * Task 2w0 gated _on_close_request on the Save-prompt flag -- a GEGL-only
 * field, which is half of why reaching for it from here was wrong -- and that
 * answers a narrower question anyway ("is the Save/Discard/Cancel prompt
 * up?"), so it left the delete confirm, raised AFTER that prompt has
 * resolved, unguarded (task aw0). This is the question the close gate actually
 * wants, so it is asked once, here, through the two accessors that exist in
 * EVERY build, and every future modal dialog belongs in it rather than in a
 * third flag. */
static gboolean
_modal_dialog_outstanding(GgazeWindow *p_win) {
   return (save_gate_outstanding(p_win->p_save_gate) ||
           _delete_confirm_outstanding(p_win));
}

/* Select gate installed on every grid (ggaze_grid_set_select_func, wired in
 * _open_rebuild_grid): gridview.c calls this instead of
 * navigator_set_current_file() directly for every grid/thumbnail selection
 * path (double-click/Enter, middle-click mark, j/k cursor move, toggle-to-
 * large sync), so an active unsaved GEGL enhance preview gets the same
 * Save/Discard/Cancel prompt as h/l/g/G/scroll/quit/d/D/m instead of being
 * silently discarded by _nav_changed_cb before the window ever sees the click
 * (tu0 review round 2, issue 1). Mirrors navigator_set_current_file's own
 * contract (TRUE iff current changed synchronously); returns FALSE both for
 * a true no-op and when the change is deferred behind the dialog -- it
 * still applies once the user resolves it (Save or Discard), exactly like
 * _move_go/ggaze_window_open's own deferred continuations. Written without
 * any #if GGAZE_HAVE_GEGL guard: ggaze_window_enhance_is_dirty and
 * _maybe_save_then are both defined (as no-ops) in the GEGL-disabled build
 * too, so this gate works unmodified in either configuration. */
static gboolean
_grid_select_gate(GgazeGrid *p_grid, GFile *p_file, gpointer p_data) {
   (void)p_grid;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL || p_file == NULL) {
      return (FALSE);
   }
   if (!ggaze_window_enhance_is_dirty(p_win)) {
      return (navigator_set_current_file(p_win->p_nav, p_file));
   }
   GFile *p_cur = navigator_get_current(p_win->p_nav);
   if (p_cur != NULL && g_file_equal(p_cur, p_file)) {
      return (FALSE); /* already current: nothing to gate */
   }
   save_gate_maybe_save_then(p_win->p_save_gate, _proceed_grid_select,
                             _file_ctx_new(p_win, p_file), _file_ctx_free);
   return (FALSE);
}

/* --- view mode ----------------------------------------------------------- */

static GgazeViewMode
_get_view(GgazeWindow *p_win) {
   const char *c_cur =
      gtk_stack_get_visible_child_name(GTK_STACK(p_win->p_stack));
   for (gsize u = 0; u < G_N_ELEMENTS(VIEW_NAMES); u++) {
      if (g_strcmp0(c_cur, VIEW_NAMES[u]) == 0) {
         return ((GgazeViewMode)u);
      }
   }
   return (GGAZE_VIEW_EMPTY);
}

static void
_set_view(GgazeWindow *p_win, GgazeViewMode e_view) {
   gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack),
                                    VIEW_NAMES[e_view]);
   /* The enhance side panel is about the image on screen: it is shown only
    * beside the large view and hidden (not closed -- its state survives a
    * `t` round trip) with the grid or the empty page. */
   gtk_widget_set_visible(p_win->p_side_slot, e_view == GGAZE_VIEW_LARGE);
   /* The info card may be up across the switch (`i`, then `t`), and its
    * plot is about the large view's picture: leaving it for the grid or the
    * empty page must take the plot down for the rest of the card's time
    * (the grid card has no plot -- there is no picture on screen to judge),
    * and coming back must fill it in again. Both fall out of one sync:
    * _info_texture_for() says NULL outside the large view. Safe at init,
    * where the overlay and the navigator may not exist yet: the sync is a
    * no-op without either. */
   _sync_info_plot(p_win);
}

/* TRUE iff a folder is open; otherwise says so (the keys that need a folder
 * used to no-op silently, which read as "the key does nothing"). */
static gboolean
_require_folder(GgazeWindow *p_win) {
   if (p_win->p_nav != NULL) {
      return (TRUE);
   }
   _show_status(p_win, "Nothing open \u2014 press o to open an image or O a "
                       "folder");
   return (FALSE);
}

/* Stop the slideshow if it runs, with a status line (any manual navigation,
 * Esc, leaving the large view or replacing the folder stops it). */
static void
_slideshow_stop(GgazeWindow *p_win, const char *c_why) {
   if (p_win->u_slideshow == 0) {
      return;
   }
   g_source_remove(p_win->u_slideshow);
   p_win->u_slideshow = 0;
   if (c_why != NULL) {
      _show_status(p_win, c_why);
   }
}

/* Show the empty-state page when the folder has nothing live to show (no
 * images, all trashed, unreadable), else bring the grid back if the empty
 * page is up. The title keeps "<folder> · 0/N" through _update_header. */
static void
_update_empty_state(GgazeWindow *p_win) {
   if (p_win->p_nav == NULL) {
      return;
   }
   guint u_live = navigator_get_remaining(p_win->p_nav);
   if (u_live > 0) {
      if (_get_view(p_win) == GGAZE_VIEW_EMPTY) {
         _set_view(p_win, GGAZE_VIEW_GRID);
      }
      return;
   }
   char *c_folder = g_file_get_basename(navigator_get_dir(p_win->p_nav));
   guint u_total  = navigator_get_count(p_win->p_nav);
   char *c_title  = NULL;
   char *c_desc   = NULL;
   if (!navigator_is_readable(p_win->p_nav)) {
      c_title = g_strdup_printf("Cannot read %s", c_folder);
      c_desc  = g_strdup(navigator_get_error(p_win->p_nav));
   } else if (u_total > 0) {
      c_title = g_strdup_printf("All %u images trashed", u_total);
      c_desc  = g_strdup("Press u to restore the last one, "
                         "o to open something else");
   } else {
      c_title = g_strdup_printf("No images in %s", c_folder);
      c_desc  = g_strdup("Press o to open an image, O a folder, "
                         "or drop one here");
   }
   adw_status_page_set_title(ADW_STATUS_PAGE(p_win->p_status_page), c_title);
   adw_status_page_set_description(ADW_STATUS_PAGE(p_win->p_status_page),
                                   c_desc);
   _set_view(p_win, GGAZE_VIEW_EMPTY);
   g_free(c_title);
   g_free(c_desc);
   g_free(c_folder);
}

/* --- actions ------------------------------------------------------------- */

/* Manual navigation stops a running slideshow before the gate runs. */
static void
_stop_slideshow_for_nav(GgazeWindow *p_win) {
   _slideshow_stop(p_win, "Slideshow stopped");
}

static void
_action_prev(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _stop_slideshow_for_nav(GGAZE_WINDOW(p_data));
   save_gate_maybe_save_then(GGAZE_WINDOW(p_data)->p_save_gate, _proceed_prev,
                             p_data, NULL);
}

static void
_action_next(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _stop_slideshow_for_nav(GGAZE_WINDOW(p_data));
   save_gate_maybe_save_then(GGAZE_WINDOW(p_data)->p_save_gate, _proceed_next,
                             p_data, NULL);
}

static void
_action_cursor_vertical(GgazeWindow *p_win, int i_direction) {
   if (_get_view(p_win) == GGAZE_VIEW_GRID && p_win->p_grid != NULL) {
      ggaze_grid_move_cursor(p_win->p_grid, i_direction);
   } else {
      ggaze_viewer_pan(GGAZE_VIEWER(p_win->p_viewer), 0.0,
                       i_direction * GGAZE_VIEWER_PAN_STEP);
   }
}

/* Shift+H / Shift+L: horizontal pan in the large view (the grid has no
 * horizontal cursor; Left/Right are prev/next everywhere). */
static void
_action_pan_left(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   ggaze_viewer_pan(GGAZE_VIEWER(GGAZE_WINDOW(p_data)->p_viewer),
                    -GGAZE_VIEWER_PAN_STEP, 0.0);
}

static void
_action_pan_right(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   ggaze_viewer_pan(GGAZE_VIEWER(GGAZE_WINDOW(p_data)->p_viewer),
                    GGAZE_VIEWER_PAN_STEP, 0.0);
}

static void
_action_cursor_down(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _action_cursor_vertical(GGAZE_WINDOW(p_data), 1);
}

static void
_action_cursor_up(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _action_cursor_vertical(GGAZE_WINDOW(p_data), -1);
}

static void
_action_first(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _stop_slideshow_for_nav(GGAZE_WINDOW(p_data));
   save_gate_maybe_save_then(GGAZE_WINDOW(p_data)->p_save_gate, _proceed_first,
                             p_data, NULL);
}

static void
_action_last(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _stop_slideshow_for_nav(GGAZE_WINDOW(p_data));
   save_gate_maybe_save_then(GGAZE_WINDOW(p_data)->p_save_gate, _proceed_last,
                             p_data, NULL);
}

/* `q`: quit. If an unsaved (GEGL) enhance preview is active, prompts
 * Save/Discard/Cancel first (docs/gegl.md, IMPLEMENTATION.md M9 "navigate/
 * d/D/m/quit with dirty"); quits immediately if nothing is dirty. */
static void
_action_quit(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   save_gate_maybe_save_then(GGAZE_WINDOW(p_data)->p_save_gate, _proceed_quit,
                             p_data, NULL);
}

/* Native window close (WM "X" button, Alt+F4, etc.) -- GTK4 never routes
 * this through win.quit/_action_quit, so without this handler it bypassed
 * the dirty-preview gate entirely (tu0 review round 2, issue 2). Reuses
 * _action_quit's own continuation (_proceed_quit just calls
 * gtk_window_close again): if nothing is dirty, propagate so the default
 * close-request handling proceeds immediately; if dirty, stop this close and
 * let _maybe_save_then's Save/Discard/Cancel prompt decide -- once it
 * resolves (Save or Discard clears the mask first), _proceed_quit's
 * gtk_window_close() re-emits "close-request", which this handler now sees
 * as clean and lets through.
 *
 * Repeated closes while the prompt is up (Alt+F4 three times: not input
 * events, so the modal grab does not swallow them) keep returning STOP but
 * do NOT open further dialogs -- _maybe_save_then queues a request while one
 * is outstanding (round 2, finding d).
 *
 * The outstanding-dialog check comes FIRST, ahead of the dirty test, for the
 * same reason _maybe_save_then orders its own two guards that way (round 3,
 * finding j): the mask can go clear UNDER a live dialog -- the slideshow tick
 * discards a dirty preview outright and a GFileMonitor rescan moves
 * navigator.current, and neither is an input event the modal grab can stop.
 * Testing only the mask therefore let the very next Alt+F4 propagate into
 * gtk_window_destroy() with a dialog still up; that destroy cannot dispose
 * the window (it drops one ref, the toplevel list's, and the dialog's own ctx
 * holds an owned window ref of its own -- see _save_prompt_show), so nothing
 * ever cancels the dialog and it is orphaned on screen with every ctx it
 * carries abandoned (2w0 review, finding A).
 *
 * It asks _modal_dialog_outstanding(), not "is the Save prompt up?": the `D`
 * delete confirm is raised AFTER that prompt has resolved, so b_save_prompt is
 * FALSE and the mask is clear while it is on screen, and a native close walked
 * straight past both -- orphaning the confirm, its _DeleteCtx and the
 * deep-copied target list, with an answer that would then run
 * ggaze_window_delete_captured against a destroyed window (task aw0).
 *
 * The delete confirm is refused WITHOUT going through _maybe_save_then, unlike
 * the Save prompt. That is not a stylistic difference: _maybe_save_then queues
 * only behind the SAVE prompt (b_save_prompt), which is FALSE while just the
 * confirm is up, so neither branch it could take here is the one we want.
 *
 *   - Clean mask: it runs the continuation straight away, and _proceed_quit's
 *     gtk_window_close() does nothing at all when called from inside a
 *     close-request handler -- gtk_window_close() returns early on
 *     priv->in_emit_close_request, which gtk_window_emit_close_request() sets
 *     around the g_signal_emit (gtk 4.22.4 gtkwindow.c:1475 and :3876,
 *     commented there as "avoid re-entrancy issues when calling
 *     gtk_window_close from a close-request handler"). So the leg would be a
 *     SILENT NO-OP: nothing queued, nothing prompted, nothing closed, while
 *     looking like it had acted.
 *   - Dirty mask: it goes to _save_prompt_show and stacks a SECOND modal
 *     dialog on top of the confirm -- precisely what the one-prompt-at-a-time
 *     guard above exists to prevent.
 *
 * Refusing outright is also all the user needs: the confirm is modal and on
 * screen, so answering it (either way) clears the slot and the next Alt+F4
 * goes through.
 *
 * Refusing a close under the Save prompt does not strand the user either: the
 * request is queued behind the prompt, so answering it in favour of proceeding
 * flushes that quit through the gate and closes the window, and answering
 * Cancel means "stay here", which is exactly what a refused close leaves. A
 * prompt whose OWN continuation is the quit still gets through, because
 * _save_dialog_cb clears b_save_prompt before running it.
 *
 * Every Save-prompt name used above -- _save_prompt_show, _save_dialog_cb and
 * the b_save_prompt flag itself -- exists ONLY in a GEGL build. This handler
 * does not: it reaches all of it through _modal_dialog_outstanding (i.e.
 * _save_prompt_outstanding), ggaze_window_enhance_is_dirty and
 * _maybe_save_then, each of which has a non-GEGL stub. So in a minimal build
 * the first two answer FALSE always, the Save-prompt half of this comment is
 * simply never live, and the delete-confirm leg is the only one that can
 * refuse a close. */
static gboolean
_on_close_request(GtkWindow *p_gtk_win, gpointer p_data) {
   (void)p_gtk_win;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (!_modal_dialog_outstanding(p_win) &&
       !ggaze_window_enhance_is_dirty(p_win)) {
      return (GDK_EVENT_PROPAGATE); /* nothing to protect: allow the close */
   }
   if (_delete_confirm_outstanding(p_win)) {
      return (GDK_EVENT_STOP); /* answer the confirm first (no queue here) */
   }
   save_gate_maybe_save_then(p_win->p_save_gate, _proceed_quit, p_win, NULL);
   return (GDK_EVENT_STOP); /* block until the prompt resolves */
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

/* `o`: pick an image (filtered to image MIME types; "All files" stays
 * available). */
static void
_action_open(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow   *p_win = GGAZE_WINDOW(p_data);
   GtkFileDialog *p_dlg = gtk_file_dialog_new();
   gtk_file_dialog_set_title(p_dlg, "Open image");
   GtkFileFilter *p_images = gtk_file_filter_new();
   gtk_file_filter_set_name(p_images, "Images");
   gtk_file_filter_add_mime_type(p_images, "image/*");
   GtkFileFilter *p_all = gtk_file_filter_new();
   gtk_file_filter_set_name(p_all, "All files");
   gtk_file_filter_add_pattern(p_all, "*");
   GListStore *p_filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
   g_list_store_append(p_filters, p_images);
   g_list_store_append(p_filters, p_all);
   gtk_file_dialog_set_filters(p_dlg, G_LIST_MODEL(p_filters));
   gtk_file_dialog_set_default_filter(p_dlg, p_images);
   g_object_unref(p_filters);
   g_object_unref(p_images);
   g_object_unref(p_all);
   gtk_file_dialog_open(p_dlg, GTK_WINDOW(p_win), NULL, _open_dialog_cb,
                        g_object_ref(p_win));
   g_object_unref(p_dlg);
}

static void
_open_folder_dialog_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   GtkFileDialog *p_dlg = GTK_FILE_DIALOG(p_src);
   GError        *p_err = NULL;
   GFile *p_dir = gtk_file_dialog_select_folder_finish(p_dlg, p_res, &p_err);
   if (p_dir != NULL) {
      ggaze_window_open(GGAZE_WINDOW(p_data), p_dir);
      g_object_unref(p_dir);
   } else if (p_err != NULL) {
      if (!g_error_matches(p_err, GTK_DIALOG_ERROR,
                           GTK_DIALOG_ERROR_DISMISSED)) {
         g_warning("ggaze: open-folder dialog failed: %s", p_err->message);
      }
      g_error_free(p_err);
   }
   g_object_unref(p_data);
}

/* `O`: pick a folder (opens in the grid). */
static void
_action_open_folder(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow   *p_win = GGAZE_WINDOW(p_data);
   GtkFileDialog *p_dlg = gtk_file_dialog_new();
   gtk_file_dialog_set_title(p_dlg, "Open folder");
   gtk_file_dialog_select_folder(p_dlg, GTK_WINDOW(p_win), NULL,
                                 _open_folder_dialog_cb, g_object_ref(p_win));
   g_object_unref(p_dlg);
}

/* --- M7: trash / delete / undo / view toggle / resize ------------------- */

/* Bin p_target (the file `d` was pressed on, captured then -- NOT a fresh read
 * of navigator.current, see _FileCtx). The policy (still-in-folder guard,
 * advance only if it was current, undo record, wording) is fileops'. */
static void
_do_trash_now(GgazeWindow *p_win, GFile *p_target) {
   fileops_trash(p_win->p_fileops, p_target);
}

static gboolean
_proceed_trash(gpointer p_data) {
   _FileCtx *p_ctx = (_FileCtx *)p_data;
   _do_trash_now(p_ctx->p_win, p_ctx->p_file);
   return (G_SOURCE_REMOVE);
}

/* `d`: move the current file to ./Trash, then advance. If an unsaved (GEGL)
 * enhance preview is active on the current file, prompts Save/Discard/Cancel
 * first; trashing proceeds only after that resolves (immediately if nothing
 * is dirty). The victim is captured HERE, at key-press time, so answering the
 * prompt can never bin whatever happens to be current by then (round 3,
 * finding h). */
static void
_action_trash(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (!_require_folder(p_win)) {
      return;
   }
   GFile *p_cur = navigator_get_current(p_win->p_nav);
   save_gate_maybe_save_then(p_win->p_save_gate, _proceed_trash,
                             _file_ctx_new(p_win, p_cur), _file_ctx_free);
}

/* TRUE iff p_win still navigates p_dir (the folder the captured delete targets
 * came from is still open), so a pending confirm dialog may safely delete
 * them. FALSE if the folder was replaced (single-instance open / drop) while
 * the dialog was pending. See window.h. */
gboolean
ggaze_window_delete_targets_still_current(GgazeWindow *p_win, GFile *p_dir) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), FALSE);
   return (
      p_win->p_delete_confirm != NULL
         ? delete_confirm_targets_still_current(p_win->p_delete_confirm, p_dir)
         : FALSE);
}

/* Permanently delete each file in p_files (the captured target set); the
 * cursor advances only when one of them really was current, and every
 * outcome is reported -- fileops owns that policy. */
static void
_do_delete_files(GgazeWindow *p_win, GList *p_files) {
   fileops_delete_files(p_win->p_fileops, p_files);
}

/* Process a confirmed bulk-delete against the captured targets p_files
 * (borrowed; not freed here). Deletes EXACTLY those files iff p_win still
 * navigates p_dir; otherwise the folder was replaced while the confirm dialog
 * was pending and the delete is refused (no files touched). Returns TRUE iff
 * the delete proceeded. See window.h. */
gboolean
ggaze_window_delete_captured(GgazeWindow *p_win, GFile *p_dir, GList *p_files) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), FALSE);
   return (p_win->p_delete_confirm != NULL
              ? delete_confirm_captured(p_win->p_delete_confirm, p_dir, p_files)
              : FALSE);
}

/* Permanently delete the target set p_files (borrowed), captured when `D` was
 * pressed -- see _capture_targets. Nothing here re-reads the navigator's
 * marks: the marks-vs-current DECISION, not just its outcome, is part of what
 * the user asked for, and it is as perishable as navigator.current.
 *
 * Round 4, finding (p): this used to re-read navigator_get_mark_count() and
 * navigator_get_marks() at answer time, on the (wrong) assumption that only
 * user input can change the mark set. navigator.c's _relist() prunes marks
 * whose file left the listing, and the folder GFileMonitor driving it keeps
 * firing behind the input-only modal grab -- so "mark one file, press `D`,
 * something else removes that file" collapsed the count to 0 and the unmarked
 * leg then PERMANENTLY deleted the current image (no trash, no undo), which
 * was neither marked nor chosen, and was the very file the prompt existed to
 * protect. A 3-marks-to-1 pruning likewise slipped past the >1-mark confirm.
 *
 * More than one captured target still opens that confirm dialog; a single one
 * is checked against fileops_target_still_in_folder first, so a target that
 * vanished behind the prompt is refused out loud rather than failing silently.
 */
static void
_do_delete_now(GgazeWindow *p_win, GList *p_files) {
   if (p_win->p_nav == NULL || p_win->p_trash == NULL || p_files == NULL) {
      return;
   }
   if (p_files->next != NULL) { /* >1 captured target */
      delete_confirm_ask(p_win->p_delete_confirm, p_files);
      return;
   }
   if (!fileops_target_still_in_folder(p_win->p_fileops,
                                       G_FILE(p_files->data))) {
      _show_status(p_win, "Nothing deleted \u2014 the file is gone");
      return;
   }
   _do_delete_files(p_win, p_files);
}

static gboolean
_proceed_delete(gpointer p_data) {
   _FilesCtx *p_ctx = (_FilesCtx *)p_data;
   _do_delete_now(p_ctx->p_win, p_ctx->p_files);
   return (G_SOURCE_REMOVE);
}

/* `D`: permanently delete the marked set (else the current file), then
 * advance -- see _do_delete_now for the >1-mark confirm dialog. If an unsaved
 * (GEGL) enhance preview is active, prompts Save/Discard/Cancel first;
 * deletion proceeds only after that resolves.
 *
 * The WHOLE target set -- including the marks-vs-current decision itself --
 * is captured HERE, at key-press time (round 4, finding p). Capturing only
 * the current file, as round 3 did, left that decision to be re-derived once
 * the prompt was answered, against a mark set the folder monitor can prune in
 * the meantime. */
static void
_action_delete(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (!_require_folder(p_win)) {
      return;
   }
   save_gate_maybe_save_then(p_win->p_save_gate, _proceed_delete,
                             _files_ctx_new(p_win, _capture_targets(p_win)),
                             _files_ctx_free);
}

/* `u`: undo the last destructive action, whichever of trash/move happened
 * most recently (decision P: one unified undo). Reopening a folder resets
 * BOTH engines' undo state and the Undo coordinator's record together
 * (ggaze_window_open clears p_trash and calls mover_clear_last +
 * undo_reset), so a stale record from a folder the user is no longer looking
 * at can never be the preferred branch below. The fallback branches in
 * undo_choose() instead serve the legitimate WITHIN-session case: e.g. move
 * a file, then trash a file (trash is now preferred), undo once (undoes the
 * trash, resets the record to NONE) -- the move is still undoable in the
 * CURRENT folder, so a second `u` should undo that too rather than silently
 * do nothing. */
static void
_action_undo(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (!_require_folder(p_win)) {
      return;
   }
   fileops_undo(p_win->p_fileops);
}

static void
_action_toggle_view(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (!_require_folder(p_win)) {
      return;
   }
   if (_get_view(p_win) == GGAZE_VIEW_LARGE) {
      _slideshow_stop(p_win, "Slideshow stopped");
      _set_view(p_win, GGAZE_VIEW_GRID);
      return;
   }
   if (_get_view(p_win) == GGAZE_VIEW_EMPTY) {
      return; /* nothing to show large */
   }
   /* Leaving the grid: sync navigator.current to the highlighted cell so the
    * large view opens the selected image. Since tu0 that sync goes through
    * _grid_select_gate, and its return value is meaningful (round 2, finding
    * c): TRUE means current really moved, in which case _nav_changed_cb has
    * ALREADY run _load_current and repeating it here would only be a
    * redundant second paint. FALSE means either a no-op (the highlighted cell
    * is already current) or that a dirty enhance preview deferred the change
    * behind the Save/Discard/Cancel prompt -- then nothing loaded it, so load
    * it here; _show_texture keeps an unanswered preview on screen. */
   gboolean b_moved =
      p_win->p_grid != NULL && ggaze_grid_sync_current(p_win->p_grid);
   _set_view(p_win, GGAZE_VIEW_LARGE);
   if (!b_moved) {
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
   GFile *p_target = NULL;
   if (_get_view(p_win) == GGAZE_VIEW_GRID && p_win->p_grid != NULL) {
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
   GFile *p_target = NULL;
   if (_get_view(p_win) == GGAZE_VIEW_GRID && p_win->p_grid != NULL) {
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
      _show_status(p_win, "Nothing to copy");
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

static void
_action_shortcuts(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   /* Build the help window from the same SHORTCUTS[] table the binding
    * controller uses (shortcuts.c), so the help never drifts from the live
    * keybindings. No builder lifetime to manage: the window owns its
    * contents and is dropped on destroy. */
   GtkShortcutsWindow *p_w = shortcuts_build_help(GTK_WINDOW(p_win));
   gtk_window_present(GTK_WINDOW(p_w));
}

/* Apply a thumbnail size (clamped to 64-512) to the grid and persist it, so
 * +/- in the grid survive a restart as the schema promises. The
 * changed::thumbnail-size handler compares against i_grid_size, so writing
 * the value we already applied does not bounce. */
static void
_set_grid_size(GgazeWindow *p_win, int i_size) {
   int i_sz           = CLAMP(i_size, 64, 512);
   p_win->i_grid_size = i_sz;
   if (p_win->p_grid != NULL) {
      ggaze_grid_set_thumbnail_size(p_win->p_grid, i_sz);
   }
   if (p_win->p_settings != NULL &&
       settings_get_thumbnail_size(p_win->p_settings) != i_sz) {
      settings_set_thumbnail_size(p_win->p_settings, i_sz);
   }
}

static void
_action_zoom_in(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (_get_view(p_win) == GGAZE_VIEW_LARGE) {
      ggaze_viewer_zoom_in(GGAZE_VIEWER(p_win->p_viewer));
   } else {
      _set_grid_size(p_win, p_win->i_grid_size + 32);
   }
}

static void
_action_zoom_out(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (_get_view(p_win) == GGAZE_VIEW_LARGE) {
      ggaze_viewer_zoom_out(GGAZE_VIEWER(p_win->p_viewer));
   } else {
      _set_grid_size(p_win, p_win->i_grid_size - 32);
   }
}

/* `0`: toggle fit / 100% in the large view; reset the thumbnail size to the
 * default in the grid. */
static void
_action_zoom_reset(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
#if GGAZE_HAVE_GEGL
   /* `0` is the panel's "Original" hotkey (docs/gegl.md): while the panel
    * is open it drops the preview instead of toggling the zoom. */
   if (enhance_ctrl_is_open(p_win->p_enhance_ctrl)) {
      enhance_ctrl_discard(p_win->p_enhance_ctrl);
      return;
   }
#endif
   if (_get_view(p_win) == GGAZE_VIEW_LARGE) {
      ggaze_viewer_toggle_fit_100(GGAZE_VIEWER(p_win->p_viewer));
   } else {
      _set_grid_size(p_win, 128);
   }
}

/* --- M4: fullscreen / slideshow / info / back --------------------------- */

static void
_action_fullscreen(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   /* Ask the window, not a private flag: a compositor-side toggle would
    * otherwise desync the two. */
   if (gtk_window_is_fullscreen(GTK_WINDOW(p_win))) {
      gtk_window_unfullscreen(GTK_WINDOW(p_win));
   } else {
      gtk_window_fullscreen(GTK_WINDOW(p_win));
   }
}

static void
_action_slideshow(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->u_slideshow != 0) {
      _slideshow_stop(p_win, "Slideshow stopped");
   } else {
      if (!_require_folder(p_win)) {
         return;
      }
      if (_get_view(p_win) != GGAZE_VIEW_LARGE) {
         /* A slideshow is a large-view thing: enter it on the current image
          * instead of making the grid highlight jump every few seconds. */
         if (p_win->p_grid != NULL) {
            ggaze_grid_sync_current(p_win->p_grid);
         }
         _set_view(p_win, GGAZE_VIEW_LARGE);
         _load_current(p_win);
      }
      gdouble d_delay = 3.0;
      if (p_win->p_settings != NULL) {
         d_delay = settings_get_slideshow_delay(p_win->p_settings);
      }
      if (d_delay < 0.1) {
         d_delay = 0.1;
      }
      p_win->u_slideshow =
         g_timeout_add((guint)(d_delay * 1000.0), _slideshow_tick, p_win);
      char *c_msg = g_strdup_printf(
         "Slideshow started (%.0fs) \u2014 S or Esc to stop", d_delay);
      _show_status(p_win, c_msg);
      g_free(c_msg);
   }
}

static void
_action_info(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _show_info(GGAZE_WINDOW(p_data));
}

/* Esc: one contextual step per press, in this order -- stop a running
 * slideshow; discard an active enhance preview (said out loud: it used to
 * vanish silently); leave fullscreen; clear marks; large -> grid; and in the
 * grid quit, but only on a SECOND Esc within GGAZE_ESC_QUIT_WINDOW_MS (the
 * first one says so), because `Esc Esc` from the large view is a common
 * "get me out of here" reflex that must not exit the program. */
static void
_action_back(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->u_slideshow != 0) {
      _slideshow_stop(p_win, "Slideshow stopped");
      return;
   }
#if GGAZE_HAVE_GEGL
   /* With the enhance panel open, Esc closes the panel and keeps the
    * preview; the next Esc drops the preview (saved or not -- it is on
    * screen either way). */
   if (enhance_ctrl_close(p_win->p_enhance_ctrl)) {
      return;
   }
   if (enhance_ctrl_is_active(p_win->p_enhance_ctrl)) {
      enhance_ctrl_discard(p_win->p_enhance_ctrl);
      _show_status(p_win, "Enhance preview discarded");
      return;
   }
#endif
   if (gtk_window_is_fullscreen(GTK_WINDOW(p_win))) {
      gtk_window_unfullscreen(GTK_WINDOW(p_win));
      return;
   }
   if (p_win->p_nav != NULL && navigator_get_mark_count(p_win->p_nav) > 0) {
      navigator_clear_marks(p_win->p_nav); /* "changed" (marks) -> badges */
      return;
   }
   if (_get_view(p_win) == GGAZE_VIEW_LARGE) {
      _set_view(p_win, GGAZE_VIEW_GRID);
      return;
   }
   gint64 i_now = g_get_monotonic_time();
   if (p_win->i_esc_at != 0 &&
       i_now - p_win->i_esc_at < (gint64)GGAZE_ESC_QUIT_WINDOW_MS * 1000) {
      gtk_window_close(GTK_WINDOW(p_win));
      return;
   }
   p_win->i_esc_at = i_now;
   _show_status(p_win, "Press Esc again or q to quit");
}

/* --- Enhance controller host ops + action routing ----------------------- *
 * The GgazeWindow forwards only the a/s/digit/Space actions and a few
 * choke-point queries to the EnhanceCtrl (see enhance-ctrl.h); these ops are
 * the controller's view back into the window -- the texture/view display,
 * the status line, the current-file/texturecache getters, the side-panel
 * slot, and the navigator flag. The GEGL build wires them into _ENHANCE_OPS
 * and creates the controller in _init_enhance_state; the non-GEGL build has
 * no controller (p_enhance_ctrl stays NULL) and the actions/public API below
 * are stubs. */
#if GGAZE_HAVE_GEGL
static void
_ec_show_texture(gpointer p_host, GdkTexture *p_tex) {
   _show_texture(GGAZE_WINDOW(p_host), p_tex);
}

static void
_ec_update_header(gpointer p_host) {
   _update_header(GGAZE_WINDOW(p_host));
}

static void
_ec_show_status(gpointer p_host, const char *c_msg) {
   _show_status(GGAZE_WINDOW(p_host), c_msg);
}

static void
_ec_load_current(gpointer p_host) {
   _load_current(GGAZE_WINDOW(p_host));
}

static GFile *
_ec_current_file(gpointer p_host) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_host);
   return (p_win->p_nav != NULL ? navigator_get_current(p_win->p_nav) : NULL);
}

static GdkTexture *
_ec_cached_texture(gpointer p_host, GFile *p_file) {
   return (viewload_get_cached(GGAZE_WINDOW(p_host)->p_viewload, p_file));
}

static void
_ec_ensure_large_view(gpointer p_host) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_host);
   if (_get_view(p_win) != GGAZE_VIEW_LARGE) {
      _set_view(p_win, GGAZE_VIEW_LARGE);
   }
}

static GtkWidget *
_ec_panel_slot(gpointer p_host) {
   return (GGAZE_WINDOW(p_host)->p_side_slot);
}

static gboolean
_ec_has_navigator(gpointer p_host) {
   return (GGAZE_WINDOW(p_host)->p_nav != NULL);
}

static const EnhanceUIHostOps _ENHANCE_OPS = {
   .show_texture       = _ec_show_texture,
   .update_header      = _ec_update_header,
   .show_status        = _ec_show_status,
   .load_current       = _ec_load_current,
   .ensure_large_view  = _ec_ensure_large_view,
   .get_current_file   = _ec_current_file,
   .get_cached_texture = _ec_cached_texture,
   .panel_slot         = _ec_panel_slot,
   .has_navigator      = _ec_has_navigator,
};

/* win.enhance (key 'a'): open the side panel beside the large view (with a
 * preview thumbnail per preset card, or label-only cards when Preferences
 * turns thumbnails off), or close it. Card clicks and hotkeys keep it open so
 * several layered presets can be compared. The widget orchestration lives in
 * the EnhanceCtrl; this just routes the action. */
static void
_action_enhance(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (!_require_folder(p_win)) {
      return;
   }
   gboolean b_thumbnails =
      p_win->p_settings != NULL &&
      settings_get_enhance_preview_thumbnails(p_win->p_settings);
   enhance_ctrl_toggle_open(p_win->p_enhance_ctrl, b_thumbnails);
}

/* win.enhance-N (keys 1-8, always live -- not gated on the panel being
 * open): toggle preset N on/off (layered), then re-apply asynchronously. The
 * preset index rides on the action as data (_add_enhance_actions); nothing
 * parses the action name. */
static void
_action_enhance_n(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   gint         i_idx =
      GPOINTER_TO_INT(g_object_get_data(G_OBJECT(p_a), "ggaze-preset-idx"));
   if (!_require_folder(p_win)) {
      return;
   }
   if (_get_view(p_win) != GGAZE_VIEW_LARGE) {
      /* A stray digit in the grid used to yank the user into the large view
       * with a preset applied and a now-dirty preview. */
      _show_status(p_win, "Enhance presets apply in the large view \u2014 "
                          "press Enter on the highlighted image first");
      return;
   }
   enhance_ctrl_toggle_preset(p_win->p_enhance_ctrl, i_idx);
}

/* win.enhance-save (keys 's' / Ctrl+S, and the panel's Save button): export
 * the current image with the enabled-preset chain to a non-colliding
 * <stem>-enhanced[-<n>].<ext>. Never overwrites the original or an existing
 * enhanced copy; on success the preview counts as saved, so moving on no
 * longer prompts for it. No-op (with a status line) when no preset is
 * enabled. */
static void
_action_enhance_save(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (!_require_folder(p_win)) {
      return;
   }
   enhance_ctrl_save_async(p_win->p_enhance_ctrl, NULL, NULL);
}

/* Hold-Space compare (see window.h): TRUE shows the cached original; FALSE
 * restores the cached modified texture. Delegates to the controller. */
void
ggaze_window_set_hold_original(GgazeWindow *p_win, gboolean b_hold) {
   g_return_if_fail(GGAZE_IS_WINDOW(p_win));
   enhance_ctrl_set_hold_original(p_win->p_enhance_ctrl, b_hold);
}

gboolean
ggaze_window_enhance_is_dirty(GgazeWindow *p_win) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), FALSE);
   return (enhance_ctrl_is_dirty(p_win->p_enhance_ctrl));
}

/* Space key controller (window-level, see _init_enhance_state): press shows
 * the original while held; release restores the modified preview. Always
 * propagates -- nothing else in this codebase binds Space, and claiming it
 * unconditionally would be surprising if that ever changes. */
static gboolean
_space_pressed_cb(GtkEventControllerKey *p_c, guint u_keyval, guint u_kc,
                  GdkModifierType e_state, gpointer p_data) {
   (void)p_c;
   (void)u_kc;
   (void)e_state;
   if (u_keyval == GDK_KEY_space) {
      ggaze_window_set_hold_original(GGAZE_WINDOW(p_data), TRUE);
   }
   return (GDK_EVENT_PROPAGATE);
}

static gboolean
_space_released_cb(GtkEventControllerKey *p_c, guint u_keyval, guint u_kc,
                   GdkModifierType e_state, gpointer p_data) {
   (void)p_c;
   (void)u_kc;
   (void)e_state;
   if (u_keyval == GDK_KEY_space) {
      ggaze_window_set_hold_original(GGAZE_WINDOW(p_data), FALSE);
   }
   return (GDK_EVENT_PROPAGATE);
}
#else  /* !GGAZE_HAVE_GEGL */
static void
_action_enhance(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _show_status(GGAZE_WINDOW(p_data), "GEGL not built in");
}
static void
_action_enhance_save(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _show_status(GGAZE_WINDOW(p_data), "GEGL not built in");
}
static void
_action_enhance_n(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   /* Silent no-op: `a` above already reports "GEGL not built in", and 1-8
    * are common keys that could be pressed incidentally -- there is no
    * discoverable enhance UI in this build for them to react to, so
    * repeating the message on every stray digit keypress would be noisy
    * rather than helpful. */
   (void)p_a;
   (void)p_v;
   (void)p_data;
}

void
ggaze_window_set_hold_original(GgazeWindow *p_win, gboolean b_hold) {
   g_return_if_fail(GGAZE_IS_WINDOW(p_win));
   (void)b_hold; /* nothing dirty is ever possible without GEGL */
}

gboolean
ggaze_window_enhance_is_dirty(GgazeWindow *p_win) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), FALSE);
   return (FALSE);
}
#endif /* GGAZE_HAVE_GEGL */

static gboolean
_slideshow_tick(gpointer p_data) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav != NULL) {
#if GGAZE_HAVE_GEGL
      /* Slideshow auto-advances unattended; a blocking Save/Discard/Cancel
       * dialog would pause it indefinitely with no one to answer it, so a
       * dirty preview is discarded outright here instead of prompted --
       * a deliberate deviation from the interactive nav paths, which do
       * prompt (h/l/g/G/scroll/quit/d/D/m). */
      if (enhance_ctrl_is_dirty(p_win->p_enhance_ctrl)) {
         enhance_ctrl_discard(p_win->p_enhance_ctrl);
      }
#endif
      navigator_next(p_win->p_nav);
   }
   return (G_SOURCE_CONTINUE);
}

/* The texture the card may plot for p_cur: the one on screen, but only when
 * it provably belongs to p_cur. The viewer alone cannot say whose pixels it
 * shows -- on a texturecache miss viewload keeps the PREVIOUS picture up
 * until the new decode lands (viewload_load_current), so during that window
 * `i` would pair the new file's EXIF with the old file's histogram. The
 * mapping lives in viewload's cache (the entry for p_cur is p_cur's decoded
 * texture, or NULL while still decoding) and in the enhance override, which
 * swaps in the preview of that same file. The displayed texture must be one
 * of those two to be plotted; anything else means "still decoding" -> no
 * plot, and _sync_info_plot() fills the plot in once the decode lands. The
 * grid never plots: there is no displayed texture to judge there.
 *
 * One consequence, accepted: while an enhance preview is up the cache
 * entry for p_cur is the ORIGINAL the override maps to the preview, and the
 * cache is a bounded LRU (texturecache.h, cap 4) that prefetch can evict
 * from. Once the original is evicted, get_cached says NULL, the check fails
 * and the card simply has no plot -- never a wrong one, which is the
 * invariant that matters; a missing plot is a visible, harmless gap the
 * next texture change (or `i` again after the original is re-cached) fills
 * in, whereas plotting the preview on trust would reopen the mispairing
 * this function exists to close. */
static GdkTexture *
_info_texture_for(GgazeWindow *p_win, GFile *p_cur) {
   if (_get_view(p_win) != GGAZE_VIEW_LARGE) {
      return (NULL);
   }
   GdkTexture *p_shown =
      ggaze_viewer_get_texture(GGAZE_VIEWER(p_win->p_viewer));
   GdkTexture *p_owned = viewload_get_cached(p_win->p_viewload, p_cur);
   if (p_shown == NULL || p_owned == NULL) {
      return (NULL);
   }
#if GGAZE_HAVE_GEGL
   p_owned = enhance_ctrl_override_texture(p_win->p_enhance_ctrl, p_owned);
#endif
   return (p_shown == p_owned ? p_shown : NULL);
}

/* `i`: toggle the EXIF/dimensions card for the current file (gathered
 * asynchronously by the InfoOverlay), with a histogram of the texture on
 * screen when _info_texture_for() can vouch that it is the current file's
 * (including an active enhance preview, which is the exposure the user is
 * actually judging); otherwise the card comes up without a plot. */
static void
_show_info(GgazeWindow *p_win) {
   if (!_require_folder(p_win)) {
      return;
   }
   GFile *p_cur = navigator_get_current(p_win->p_nav);
   if (p_cur == NULL) {
      return;
   }
   info_overlay_toggle_for_file(p_win->p_info, p_cur,
                                _info_texture_for(p_win, p_cur));
}

/* The picture changed (every path funnels through _show_texture) or the
 * view did (_set_view): tell the info overlay which texture it may plot now
 * -- the displayed one when _info_texture_for() vouches for it as the
 * current file's, else NULL -- so a card that is up follows hold-Space, a
 * landing preset, a decode landing under a card opened while it was in
 * flight, and a `t` to the grid and back. The overlay ignores the call
 * unless a file card is up or being gathered, so this costs nothing on the
 * plain load path. */
static void
_sync_info_plot(GgazeWindow *p_win) {
   if (p_win->p_info == NULL) {
      return; /* mid-teardown or still constructing: no card to keep in step */
   }
   GFile *p_cur =
      p_win->p_nav != NULL ? navigator_get_current(p_win->p_nav) : NULL;
   info_overlay_texture_changed(
      p_win->p_info, p_cur != NULL ? _info_texture_for(p_win, p_cur) : NULL);
}

/* Hide the info overlay because the current file changed: reached from
 * _nav_changed_cb (the choke point every navigation path funnels through)
 * and from _open_now (which does not reliably emit "changed"). The card must
 * never keep showing a PREVIOUS file's data over the new one; a status line
 * shown right after re-shows the label with its own fresh timer. */
static void
_dismiss_info_for_nav(GgazeWindow *p_win) {
   info_overlay_dismiss(p_win->p_info);
}

/* Transient status line (the project's toast): visible in both views. */
static void
_show_status(GgazeWindow *p_win, const char *c_msg) {
   info_overlay_show_status(p_win->p_info, c_msg);
}

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
      GPtrArray *p = settings_get_editors(p_win->p_settings);
      opener_set_progs(p_win->p_opener, p); /* copies SettingsPair internally */
      g_ptr_array_unref(p);
   }
   if (p_win->p_runner != NULL) {
      GPtrArray *p = settings_get_scripts(p_win->p_settings);
      runner_set_scripts(p_win->p_runner,
                         p); /* copies SettingsPair internally */
      g_ptr_array_unref(p);
   }
#if GGAZE_HAVE_GEGL
   if (p_win->p_enhance_ctrl != NULL) {
      /* The engine merges built-ins + these user presets itself. */
      GPtrArray *p_user = settings_get_enhance_presets(p_win->p_settings);
      enhance_ctrl_set_user_presets(p_win->p_enhance_ctrl, p_user);
      g_ptr_array_unref(p_user);
   }
#endif
}

/* GSettings is the single source of truth: a change to any of the engine
 * list keys (destinations/editors/scripts/enhance-presets) or the viewer
 * preference keys (background/scroll-behavior) re-applies live, so
 * Preferences edits take effect immediately instead of on the next launch.
 * The four list keys reload all engine lists (cheap; lists are small), and
 * the two viewer keys re-apply background/scroll. Other keys (thumbnail-
 * size, hide-trashed, sort, ...) are already applied at their own action/
 * open sites and are not wired here to avoid churn on frequent writes like
 * the +/- zoom. */
static void
_on_pref_changed(GSettings *p_gs, const char *c_key, gpointer p_data) {
   (void)p_gs;
   (void)c_key;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   _load_engine_lists(p_win);
   _apply_viewer_prefs(p_win);
}

/* The folder-shaped preferences (sort, wrap, RAW sidecars, hide trashed,
 * thumbnail size) apply to the OPEN folder too, not only the next one: a
 * user who changes "Sort order" in Preferences expects the grid to re-sort
 * now. The navigator/grid setters are no-ops when the value is unchanged,
 * so a Preferences write that matches the live state costs nothing. */
static void
_on_folder_pref_changed(GSettings *p_gs, const char *c_key, gpointer p_data) {
   (void)p_gs;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_settings == NULL) {
      return;
   }
   if (g_strcmp0(c_key, "thumbnail-size") == 0) {
      int i_sz = settings_get_thumbnail_size(p_win->p_settings);
      if (i_sz != p_win->i_grid_size) {
         _set_grid_size(p_win, i_sz);
      }
      return;
   }
   if (p_win->p_nav == NULL) {
      return;
   }
   if (g_strcmp0(c_key, "sort") == 0) {
      navigator_set_sort(p_win->p_nav, settings_get_sort(p_win->p_settings));
   } else if (g_strcmp0(c_key, "wrap") == 0) {
      navigator_set_wrap(p_win->p_nav, settings_get_wrap(p_win->p_settings));
   } else if (g_strcmp0(c_key, "hide-raw-sidecars") == 0) {
      navigator_set_hide_raw(p_win->p_nav,
                             settings_get_hide_raw(p_win->p_settings));
   } else if (g_strcmp0(c_key, "hide-trashed") == 0 && p_win->p_grid != NULL) {
      ggaze_grid_set_hide_trashed(p_win->p_grid,
                                  settings_get_hide_trashed(p_win->p_settings));
   }
}

/* Scroll-wheel navigate (GGAZE_SCROLL_NAVIGATE): advance the navigator,
 * prompting Save/Discard/Cancel first if an unsaved (GEGL) enhance preview is
 * active, same as the h/l/g/G actions (_action_prev/next/first/last). */
static void
_on_viewer_navigate(GgazeViewer *p_v, gint i_dir, gpointer p_data) {
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL) {
      return;
   }
   if (i_dir >= 0) {
      save_gate_maybe_save_then(p_win->p_save_gate, _proceed_next, p_win, NULL);
   } else {
      save_gate_maybe_save_then(p_win->p_save_gate, _proceed_prev, p_win, NULL);
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

/* Row click / hotkey: launch that editor on the original current file, then
 * close the popover. */
static void
_open_ext_activate(gpointer p_data, guint u_idx) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   ggaze_window_open_external_index(p_win, u_idx);
   popup_list_delete(&p_win->p_open_ext_pop);
}

/* Build and pop up the open-external popover listing the configured editors
 * (`e`). Toggles closed on a second press. If none are configured, the popup
 * shows a single message row pointing at Preferences (`,`) instead. */
static void
_action_open_external(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (!_require_folder(p_win)) {
      return;
   }
   /* Toggle: a second `e` while the popover is up just closes it. */
   if (p_win->p_open_ext_pop != NULL) {
      popup_list_delete(&p_win->p_open_ext_pop);
      return;
   }
   const GPtrArray *p_progs =
      p_win->p_opener != NULL ? opener_get_progs(p_win->p_opener) : NULL;
   char *c_title = NULL;
   {
      GFile *p_cur = navigator_get_current(p_win->p_nav);
      if (p_cur != NULL) {
         char *c_name = g_file_get_basename(p_cur);
         c_title      = g_strdup_printf("Open %s in:", c_name);
         g_free(c_name);
      }
   }
   popup_list_new(p_win->p_stack, &p_win->p_open_ext_pop,
                  c_title != NULL ? c_title : "Open in:",
                  "No editors configured. Press , to open Preferences.",
                  p_progs, popup_list_settings_pair_name, _open_ext_activate,
                  p_win);
   g_free(c_title);
   /* Focuses the first editor row itself -- see "POPOVER KEYBOARD FOCUS"
    * near the top of this file. */
   popup_list_popup(p_win->p_open_ext_pop);
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
   const SettingsPair *p_prog = g_ptr_array_index((GPtrArray *)p_progs, u_idx);
   GError             *p_err  = NULL;
   gboolean            b_ok   = opener_launch(p_cur, p_prog, &p_err);
   if (!b_ok) {
      char *c_msg =
         g_strdup_printf("Could not open in %s: %s",
                         p_prog->c_name != NULL ? p_prog->c_name : "(unnamed)",
                         p_err != NULL ? p_err->message : "(no detail)");
      g_warning("ggaze: %s", c_msg);
      _show_status(p_win, c_msg);
      g_free(c_msg);
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
   int          i_code = runner_run_finish(p_res, &p_err);

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

/* Row click / hotkey: run that script on the original current file + folder,
 * then close the popover. */
static void
_run_script_activate(gpointer p_data, guint u_idx) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   ggaze_window_run_script_index(p_win, u_idx);
   popup_list_delete(&p_win->p_run_script_pop);
}

/* Build and pop up the run-script popover listing the configured scripts
 * (`!`). Toggles closed on a second press. If none are configured, the popup
 * shows a single message row pointing at Preferences (`,`) instead. */
static void
_action_run_script(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (!_require_folder(p_win)) {
      return;
   }
   /* Toggle: a second `!` while the popover is up just closes it. */
   if (p_win->p_run_script_pop != NULL) {
      popup_list_delete(&p_win->p_run_script_pop);
      return;
   }
   const GPtrArray *p_scripts =
      p_win->p_runner != NULL ? runner_get_scripts(p_win->p_runner) : NULL;
   popup_list_new(
      p_win->p_stack, &p_win->p_run_script_pop,
      "Run script:", "No scripts configured. Press , to open Preferences.",
      p_scripts, popup_list_settings_pair_name, _run_script_activate, p_win);
   /* Focuses the first script row itself -- see "POPOVER KEYBOARD FOCUS"
    * near the top of this file. */
   popup_list_popup(p_win->p_run_script_pop);
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
   const SettingsPair *p_sc  = g_ptr_array_index((GPtrArray *)p_scripts, u_idx);
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

/* The marks-or-current target set a move acts on is the same one `D` uses --
 * see _capture_targets, which is where that rule (and why it must be resolved
 * at key-press time) now lives for both. */

/* Move the target set p_files (borrowed; the caller frees) to destination
 * u_idx. Split out of ggaze_window_move_index so the `m` popup can CAPTURE
 * its targets when the row is clicked and still move exactly those once the
 * Save/Discard/Cancel prompt resolves. The policy is fileops'. */
static gboolean
_move_captured(GgazeWindow *p_win, guint u_idx, GList *p_files) {
   return (fileops_move(p_win->p_fileops, u_idx, p_files));
}

/* Move u_idx (0-based, in the configured destinations list order) — see
 * window.h. Derives its targets right now (marks, else the current file),
 * which is exactly right for a direct, synchronous call. */
gboolean
ggaze_window_move_index(GgazeWindow *p_win, guint u_idx) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), FALSE);
   if (p_win->p_nav == NULL || p_win->p_mover == NULL) {
      return (FALSE);
   }
   GList *p_files = _capture_targets(p_win);
   if (p_files == NULL) {
      return (FALSE);
   }
   gboolean b_ok = _move_captured(p_win, u_idx, p_files);
   g_list_free_full(p_files, (GDestroyNotify)g_object_unref);
   return (b_ok);
}

/* Captures the destination index AND the target set for the Save/Discard/
 * Cancel prompt's continuation (ggaze_window_move_index itself stays a plain,
 * synchronous, directly-testable function -- see window.h -- so the
 * dirty-preview gate lives here, at the two real UI entry points (row click /
 * hotkey), rather than inside it. */
typedef struct {
   _FilesCtx t_base; /* window + captured target set (see _FilesCtx) */
   guint     u_idx;  /* destination index */
} _MoveIdxCtx;

static gboolean
_proceed_move_idx(gpointer p_data) {
   _MoveIdxCtx *p_ctx = (_MoveIdxCtx *)p_data;
   _move_captured(p_ctx->t_base.p_win, p_ctx->u_idx, p_ctx->t_base.p_files);
   return (G_SOURCE_REMOVE);
}

/* _maybe_save_then owns the ctx and frees it through this on every exit path,
 * so Cancel/dismiss/failed-Save no longer leak the window ref it holds
 * (round 2, finding b). */
static void
_move_idx_ctx_free(gpointer p_data) {
   _MoveIdxCtx *p_ctx = (_MoveIdxCtx *)p_data;
   _files_ctx_clear(&p_ctx->t_base);
   g_free(p_ctx);
}

/* Close the popover and move to destination u_idx, prompting Save/Discard/
 * Cancel first if an unsaved (GEGL) enhance preview is active on the current
 * file (docs/gegl.md, IMPLEMENTATION.md M9 "navigate/d/D/m/quit with
 * dirty"). Shared by the row click and hotkey paths below. The targets are
 * captured here, when the row is clicked, not re-derived when the prompt is
 * answered (round 3, finding h; round 4, finding p for why the marks-vs-
 * current decision has to be captured with them). */
static void
_move_go(GgazeWindow *p_win, guint u_idx) {
   popup_list_delete(&p_win->p_move_pop);
   _MoveIdxCtx *p_ctx = g_new(_MoveIdxCtx, 1);
   _files_ctx_init(&p_ctx->t_base, p_win, _capture_targets(p_win));
   p_ctx->u_idx = u_idx;
   save_gate_maybe_save_then(p_win->p_save_gate, _proceed_move_idx, p_ctx,
                             _move_idx_ctx_free);
}

/* Row click / hotkey: close the popover, then move to that destination,
 * prompting Save/Discard/Cancel first if an unsaved enhance preview is
 * active. The close runs BEFORE the prompt (see _move_go) so the popover is
 * gone while the prompt is up. */
static void
_move_activate(gpointer p_data, guint u_idx) {
   _move_go(GGAZE_WINDOW(p_data), u_idx);
}

/* Build and pop up the move popover listing the configured destinations
 * (`m`). Toggles closed on a second press, same as `e`/`!`. */
static void
_action_move(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (!_require_folder(p_win) || p_win->p_mover == NULL) {
      return;
   }
   if (p_win->p_move_pop != NULL) {
      popup_list_delete(&p_win->p_move_pop);
      return;
   }
   GList *p_targets = _capture_targets(p_win);
   guint  u_count   = g_list_length(p_targets);
   g_list_free_full(p_targets, (GDestroyNotify)g_object_unref);
   if (u_count == 0) {
      _show_status(p_win, "Nothing to move");
      return;
   }
   const GPtrArray *p_dests = mover_get_dests(p_win->p_mover);
   char            *c_title =
      g_strdup_printf("Move %u image%s to:", u_count, u_count == 1 ? "" : "s");
   popup_list_new(p_win->p_stack, &p_win->p_move_pop, c_title,
                  "No destinations configured. Press , to open Preferences.",
                  p_dests, popup_list_settings_pair_name, _move_activate,
                  p_win);
   g_free(c_title);
   /* Focuses the first destination row itself -- see "POPOVER KEYBOARD
    * FOCUS" near the top of this file. */
   popup_list_popup(p_win->p_move_pop);
}

static const GActionEntry ACTIONS[] = {
   {.name = "prev", .activate = _action_prev},
   {.name = "next", .activate = _action_next},
   {.name = "cursor-down", .activate = _action_cursor_down},
   {.name = "cursor-up", .activate = _action_cursor_up},
   {.name = "first", .activate = _action_first},
   {.name = "last", .activate = _action_last},
   {.name = "open", .activate = _action_open},
   {.name = "open-folder", .activate = _action_open_folder},
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
   {.name = "zoom-in", .activate = _action_zoom_in},
   {.name = "zoom-out", .activate = _action_zoom_out},
   {.name = "zoom-reset", .activate = _action_zoom_reset},
   {.name = "pan-left", .activate = _action_pan_left},
   {.name = "pan-right", .activate = _action_pan_right},
   {.name = "enter-large", .activate = _action_enter_large},
   {.name = "menu", .activate = _action_menu},
   {.name = "empty-trash", .activate = _action_empty_trash},
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
_drop_cb(GtkDropTarget *p_t, const GValue *p_val, gdouble d_x, gdouble d_y,
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
   guint        u_n     = g_slist_length(p_files);
   if (u_n == 0) {
      return (FALSE);
   }
   /* Decision Z: many files -> first file's folder in the grid. */
   GFile **pp = g_new(GFile *, u_n);
   guint   u  = 0;
   for (GSList *p_it = p_files; p_it != NULL; p_it = p_it->next) {
      pp[u++] = G_FILE(p_it->data);
   }
   ggaze_window_open_files(p_win, pp, (gint)u_n);
   g_free(pp);
   return (TRUE);
}

/* Drop highlight: a frame around the window content while a file list hovers
 * over it, cleared when it leaves or drops. */
static GdkDragAction
_drop_enter_cb(GtkDropTarget *p_t, gdouble d_x, gdouble d_y, gpointer p_data) {
   (void)p_t;
   (void)d_x;
   (void)d_y;
   gtk_widget_add_css_class(GGAZE_WINDOW(p_data)->p_overlay, "ggaze-drop");
   return (GDK_ACTION_COPY);
}

static void
_drop_leave_cb(GtkDropTarget *p_t, gpointer p_data) {
   (void)p_t;
   gtk_widget_remove_css_class(GGAZE_WINDOW(p_data)->p_overlay, "ggaze-drop");
}

/* --- navigator changed -> reload ----------------------------------------- */

static void
_nav_changed_cb(Navigator *p_nav, guint u_flags, gpointer p_data) {
   (void)p_nav;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if ((u_flags & (GGAZE_NAV_CURSOR | GGAZE_NAV_LISTING)) == 0) {
      /* Marks or the removed set changed: only the title (mark count,
       * remaining count) is affected. */
      _update_header(p_win);
      return;
   }
#if GGAZE_HAVE_GEGL
   /* "changed" fires for every navigator rescan, not only an actual move to a
    * different current file -- notably, win.enhance-save writes the
    * "-enhanced" copy into the SAME live-monitored folder, whose GFileMonitor
    * then schedules a debounced rescan that re-emits "changed" a few hundred
    * ms later even though navigator.current never moved. The enhance
    * controller's nav_changed resets only on an actual identity change, so
    * that incidental rescan does not silently discard the still-active
    * preview (tests/test_enhance_flow.c save-twice-in-a-row).
    */
   enhance_ctrl_nav_changed(p_win->p_enhance_ctrl);
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
   _update_empty_state(p_win);
}

/* Enter/double-click on a cell: switch to large view and show whatever
 * navigator.current now is. The cell's own selection was already routed
 * through _grid_select_gate before this runs, so current may deliberately
 * NOT have moved (a dirty enhance preview put the change behind the
 * Save/Discard/Cancel prompt) -- keeping the still-active preview on screen
 * in that case is _show_texture's job, not this one's (see there). */
static void
_on_grid_activate(GgazeGrid *p_grid, gpointer p_data) {
   (void)p_grid;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   _set_view(p_win, GGAZE_VIEW_LARGE);
   _load_current(p_win);
}

/* Enter (global): in the grid, open the highlighted image large -- the same
 * thing the flowbox's own Enter does, but through the shortcuts table so
 * the `?` help lists it and it works when focus wandered off the grid. */
static void
_action_enter_large(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL || _get_view(p_win) != GGAZE_VIEW_GRID ||
       p_win->p_grid == NULL) {
      return;
   }
   ggaze_grid_sync_current(p_win->p_grid);
   _on_grid_activate(p_win->p_grid, p_win);
}

/* F10 / the header-bar button: pop the main menu. */
static void
_action_menu(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_menu_btn != NULL) {
      gtk_menu_button_popup(GTK_MENU_BUTTON(p_win->p_menu_btn));
   }
}

/* Empty Trash confirm answered. p_data is a ref on the window. */
static void
_empty_trash_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   GError      *p_err = NULL;
   int          i_btn =
      gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(p_src), p_res, &p_err);
   g_clear_error(&p_err);
   if (i_btn == 1 && !p_win->b_disposed && p_win->p_trash != NULL) {
      guint u_done = 0;
      if (trash_empty(p_win->p_trash, &u_done, &p_err)) {
         char *c_msg = g_strdup_printf("Emptied Trash (%u file%s)", u_done,
                                       u_done == 1 ? "" : "s");
         _show_status(p_win, c_msg);
         g_free(c_msg);
      } else {
         char *c_msg =
            g_strdup_printf("Empty Trash failed after %u: %s", u_done,
                            p_err != NULL ? p_err->message : "?");
         _show_status(p_win, c_msg);
         g_free(c_msg);
         g_clear_error(&p_err);
      }
      undo_reset(p_win->p_undo); /* the restorable file is gone */
   }
   g_object_unref(p_win);
}

/* `E` / main menu "Empty Trash": permanently delete the folder's .Trash
 * contents after a confirm (no undo past this point). */
static void
_action_empty_trash(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (!_require_folder(p_win) || p_win->p_trash == NULL) {
      return;
   }
   guint u_n = trash_count(p_win->p_trash);
   if (u_n == 0) {
      _show_status(p_win, "Trash is already empty");
      return;
   }
   GtkAlertDialog *p_dlg = gtk_alert_dialog_new(
      "Permanently delete %u file%s in this folder's Trash?", u_n,
      u_n == 1 ? "" : "s");
   gtk_alert_dialog_set_detail(p_dlg, "This cannot be undone.");
   gtk_alert_dialog_set_buttons(
      p_dlg, (const char *[]){"Cancel", "Empty Trash", NULL});
   gtk_alert_dialog_set_cancel_button(p_dlg, 0);
   gtk_alert_dialog_choose(p_dlg, GTK_WINDOW(p_win), NULL, _empty_trash_cb,
                           g_object_ref(p_win));
   g_object_unref(p_dlg);
}

/* --- load current into the viewer ---------------------------------------- */

static void
_show_texture(GgazeWindow *p_win, GdkTexture *p_tex) {
   /* Only update the viewer's texture here; do NOT force the stack to "large".
    * The stack is owned by the caller: file-open / toggle / grid-activate set
    * "large" themselves before loading, and directory-open sets "grid".
    * Forcing large here would yank a just-opened folder back out of the grid
    * view the moment its first image finishes loading.
    *
    * Every path that puts an image on screen funnels through here -- cache
    * hit, progressive partial, full async result, hold-Space, the finished
    * enhance itself -- which is why the enhance-preview override lives here
    * rather than at any single call site (tu0 review round 2, findings c/e).
    * Point-patching _on_grid_activate was not enough: the pipeline only
    * paints synchronously on a texturecache HIT, so on a miss the async
    * finish landed after the restore and the plain original won the race;
    * and the view toggle (`t`, `t`) never had the patch at all. */
#if GGAZE_HAVE_GEGL
   p_tex = enhance_ctrl_override_texture(p_win->p_enhance_ctrl, p_tex);
#endif
   ggaze_viewer_set_texture(GGAZE_VIEWER(p_win->p_viewer), p_tex);
   /* After the override, so the card plots what is actually on screen. */
   _sync_info_plot(p_win);
}

/* Show navigator.current through the ViewLoad pipeline (texture LRU, one
 * active load, prefetch, last-write-wins -- see viewload.h). */
static void
_load_current(GgazeWindow *p_win) {
   if (p_win->p_nav == NULL) {
      return;
   }
   viewload_load_current(p_win->p_viewload);
}

/* --- ViewLoad host ops (the pipeline's view back into the window) ------- */

static void
_vl_show_texture(gpointer p_host, GdkTexture *p_tex) {
   _show_texture(GGAZE_WINDOW(p_host), p_tex);
}

static void
_vl_update_header(gpointer p_host) {
   _update_header(GGAZE_WINDOW(p_host));
}

static void
_vl_show_status(gpointer p_host, const char *c_msg) {
   _show_status(GGAZE_WINDOW(p_host), c_msg);
}

static const ViewLoadHostOps _VIEWLOAD_OPS = {
   .show_texture  = _vl_show_texture,
   .update_header = _vl_update_header,
   .show_status   = _vl_show_status,
};

/* "<file> · n/total" (live images on both sides), or "<folder> · 0/N" when
 * nothing is current, plus " · N marked" when marks exist. NULL without a
 * folder. Caller frees. */
static gchar *
_title_for_nav(Navigator *p_nav) {
   gchar *c_title = NULL;
   {
      GFile *p_cur       = navigator_get_current(p_nav);
      guint  u_remaining = navigator_get_remaining(p_nav);
      guint  u_total     = navigator_get_count(p_nav);
      if (p_cur != NULL) {
         /* "n/total" over LIVE entries on both sides: the position among
          * the not-yet-trashed images over how many of those remain. The
          * old listing-index numerator overtook the denominator ("6/5")
          * as soon as anything was trashed. */
         char *c_name = g_file_get_basename(p_cur);
         guint u_pos  = navigator_get_live_position(p_nav);
         c_title =
            g_strdup_printf("%s  \u00b7  %u/%u", c_name, u_pos, u_remaining);
         g_free(c_name);
      } else {
         /* Nothing current: keep the folder name and the count on screen
          * instead of a bare "ggaze". */
         char *c_folder = g_file_get_basename(navigator_get_dir(p_nav));
         c_title = g_strdup_printf("%s  \u00b7  0/%u", c_folder, u_total);
         g_free(c_folder);
      }
      /* Append the marked count so multi-selection is visible in the title. */
      guint u_marks = navigator_get_mark_count(p_nav);
      if (u_marks > 0 && c_title != NULL) {
         char *c_tmp =
            g_strdup_printf("%s  \u00b7  %u marked", c_title, u_marks);
         g_free(c_title);
         c_title = c_tmp;
      }
   }
   return (c_title);
}

static void
_update_header(GgazeWindow *p_win) {
   gchar *c_title = p_win->p_nav != NULL ? _title_for_nav(p_win->p_nav) : NULL;
#if GGAZE_HAVE_GEGL
   /* Append the enabled enhance preset names (comma-joined) when layered. */
   if (p_win->p_enhance_ctrl != NULL && c_title != NULL) {
      char *c_presets =
         enhancer_describe_mask(enhance_ctrl_get_presets(p_win->p_enhance_ctrl),
                                enhance_ctrl_get_mask(p_win->p_enhance_ctrl));
      if (c_presets != NULL) {
         char *c_tmp = g_strdup_printf("%s  \u00b7  %s", c_title, c_presets);
         g_free(c_title);
         c_title = c_tmp;
         g_free(c_presets);
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

/* Cancel the `D` delete-confirm dialog, for the same reason save_gate_dispose
 * cancels the Save prompt: nothing but the dialog itself can finish its GTask,
 * so without this the _DeleteCtx -- and the deep copy of the captured target
 * list it carries -- was simply abandoned when the window went away.
 *
 * The cancel resolves the dialog as "not confirmed", which is the only honest
 * answer here and the safe one twice over: _delete_confirm_answered_yes sees
 * the cancel's error and refuses, and dispose has already cleared p_nav, so
 * ggaze_window_delete_captured would refuse as well.
 *
 * Not under GGAZE_HAVE_GEGL, unlike the enhance dispose above: this dialog
 * exists in every build. Cancelling a NULL cancellable is a no-op, so no
 * guard is needed for the (usual) case where no confirm is up. */
static void
_delete_confirm_dispose(GgazeWindow *p_win) {
   delete_confirm_dispose(p_win->p_delete_confirm);
}

static void
ggaze_window_dispose(GObject *p_obj) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_obj);
   p_win->b_disposed = TRUE; /* async callbacks check this before touching UI */
   viewload_dispose(p_win->p_viewload); /* cancels loads; before the nav */
   if (p_win->p_nav != NULL) {
      g_signal_handlers_disconnect_by_data(p_win->p_nav, p_win);
      if (p_win->p_grid != NULL) {
         ggaze_grid_detach(p_win->p_grid); /* before the nav is freed */
      }
      g_clear_object(&p_win->p_nav);
   }
   popup_list_delete(&p_win->p_open_ext_pop);
   popup_list_delete(&p_win->p_run_script_pop);
   popup_list_delete(&p_win->p_move_pop);
   _delete_confirm_dispose(p_win);
   save_gate_dispose(p_win->p_save_gate);
#if GGAZE_HAVE_GEGL
   enhance_ctrl_dispose(p_win->p_enhance_ctrl);
#endif
   if (p_win->u_slideshow != 0) {
      g_source_remove(p_win->u_slideshow);
      p_win->u_slideshow = 0;
   }
   info_overlay_dispose(p_win->p_info); /* timer + in-flight decode; no
                                         * widget touch after this */
   fileops_set_folder(p_win->p_fileops, NULL, NULL);
   g_clear_pointer(&p_win->p_trash, trash_delete);
   g_clear_pointer(&p_win->p_thumb, thumbnail_delete);
   g_clear_pointer(&p_win->p_runner, runner_delete);
   g_clear_pointer(&p_win->p_opener, opener_delete);
   g_clear_pointer(&p_win->p_mover, mover_delete);
   g_clear_pointer(&p_win->p_fileops, fileops_delete);
   g_clear_pointer(&p_win->p_undo, undo_delete);
   g_clear_pointer(&p_win->p_settings, settings_delete);
   /* p_stack/p_viewer/p_grid are GtkWidgets parented to the window; GTK
    * releases them. */
   G_OBJECT_CLASS(ggaze_window_parent_class)->dispose(p_obj);
}

static void ggaze_window_finalize(GObject *p_obj);

static void
ggaze_window_class_init(GgazeWindowClass *p_klass) {
   GObjectClass *p_obj_class = G_OBJECT_CLASS(p_klass);
   p_obj_class->dispose      = ggaze_window_dispose;
   p_obj_class->finalize     = ggaze_window_finalize;
}

/* p_delete_confirm, p_save_gate, p_viewload, p_info and p_enhance_ctrl are
 * freed here (not in dispose): a pending dialog's ctx (and the enhance
 * controller's in-flight async ctx) holds an owned ref to the WINDOW, so the
 * window object -- and thus these helpers -- outlive the dialog. Dispose only
 * cancels them (delete_confirm_dispose / save_gate_dispose /
 * enhance_ctrl_dispose); finalize frees the helpers once the last ctx has
 * dropped its window ref. */
static void
ggaze_window_finalize(GObject *p_obj) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_obj);
   g_clear_pointer(&p_win->p_delete_confirm, delete_confirm_delete);
   g_clear_pointer(&p_win->p_save_gate, save_gate_delete);
   g_clear_pointer(&p_win->p_viewload, viewload_delete);
   g_clear_pointer(&p_win->p_info, info_overlay_delete);
#if GGAZE_HAVE_GEGL
   g_clear_pointer(&p_win->p_enhance_ctrl, enhance_ctrl_delete);
#endif
   G_OBJECT_CLASS(ggaze_window_parent_class)->finalize(p_obj);
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
             "/* status line / EXIF card over the picture. */\n"
             ".ggaze-info {\n"
             "  background-color: rgba(0, 0, 0, 0.7);\n"
             "  color: #ffffff;\n"
             "  padding: 6px 10px;\n"
             "  border-radius: 6px;\n"
             "}\n"
             "/* file list hovering over the window (drop target). */\n"
             ".ggaze-drop {\n"
             "  box-shadow: inset 0 0 0 3px #3584e4;\n"
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

#if GGAZE_HAVE_GEGL
/* Create the GEGL enhance controller and install the hold-Space compare
 * key controller, extracted out of ggaze_window_init to keep it under
 * CLAUDE.md's 50-line hard limit. Hold-Space (docs/ui-and-interactions.md
 * "Compare original vs modified") needs press AND release, which
 * shortcuts.c's GtkShortcutController (action triggers fire on press only)
 * cannot express, so this is a dedicated key controller straight on the
 * window, capture phase like the popovers' own controllers. Space is not
 * bound to anything else, so ordering relative to shortcuts_install's
 * GLOBAL-scope controller does not matter here. The controller owns every
 * enhance field + the Enhancer engine; the space callbacks route back into
 * it through the public ggaze_window_set_hold_original. */
static void
_init_enhance_state(GgazeWindow *p_win) {
   p_win->p_enhance_ctrl = enhance_ctrl_new(&_ENHANCE_OPS, p_win);

   GtkEventController *p_space_kc = gtk_event_controller_key_new();
   gtk_event_controller_set_propagation_phase(p_space_kc, GTK_PHASE_CAPTURE);
   g_signal_connect(p_space_kc, "key-pressed", G_CALLBACK(_space_pressed_cb),
                    p_win);
   g_signal_connect(p_space_kc, "key-released", G_CALLBACK(_space_released_cb),
                    p_win);
   gtk_widget_add_controller(GTK_WIDGET(p_win), p_space_kc);
}
#endif

/* --- FileOps report op ---------------------------------------------------- */

static void
_fo_report(gpointer p_host, const char *c_msg) {
   _show_status(GGAZE_WINDOW(p_host), c_msg);
}

/* --- DeleteConfirm host ops (window side of the bulk-delete flow) -------- */

static void
_dc_show_status(gpointer p_host, const char *c_msg) {
   _show_status(GGAZE_WINDOW(p_host), c_msg);
}

static void
_dc_perform_delete(gpointer p_host, GList *p_files) {
   _do_delete_files(GGAZE_WINDOW(p_host), p_files);
}

static GFile *
_dc_current_dir(gpointer p_host) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_host);
   return (p_win->p_nav != NULL ? navigator_get_dir(p_win->p_nav) : NULL);
}

static const DeleteConfirmHostOps _DELETE_CONFIRM_OPS = {
   .show_status    = _dc_show_status,
   .perform_delete = _dc_perform_delete,
   .current_dir    = _dc_current_dir,
};

/* --- SaveGate host ops (window side of the Save/Discard/Cancel gate) ----- */

static gboolean
_sg_is_dirty(gpointer p_host) {
   return (ggaze_window_enhance_is_dirty(GGAZE_WINDOW(p_host)));
}

static void
_sg_show_status(gpointer p_host, const char *c_msg) {
   _show_status(GGAZE_WINDOW(p_host), c_msg);
}

#if GGAZE_HAVE_GEGL
/* The Save button (the SaveGate host's do_save op): completes fn_done with
 * TRUE iff the user's action may proceed. "Nothing to save" is NOT a
 * failure: the preview can legitimately be gone by the time the prompt is
 * answered (the slideshow timer and the folder's GFileMonitor both run
 * behind a modal dialog, and either can clear the mask), and there is then
 * nothing left to protect. Reporting it and proceeding is the honest
 * outcome. A real export failure completes with FALSE: it must not be
 * silently downgraded to Discard -- the mask and preview stay so the user
 * can retry, and the window stays alive so the error message is readable.
 * The export itself runs in a worker (enhance_ctrl_save_async). */
static void
_sg_do_save(gpointer p_host, SaveGateSaveDoneFn fn_done, gpointer p_done_data) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_host);
   if (!enhance_ctrl_can_save(p_win->p_enhance_ctrl)) {
      _show_status(p_win, "Nothing to save \u2014 the preview is gone");
      fn_done(TRUE, p_done_data);
      return;
   }
   enhance_ctrl_save_async(p_win->p_enhance_ctrl, fn_done, p_done_data);
}

static void
_sg_discard(gpointer p_host) {
   enhance_ctrl_discard(GGAZE_WINDOW(p_host)->p_enhance_ctrl);
}
#else /* !GGAZE_HAVE_GEGL */
/* Without GEGL is_dirty is always FALSE, so the gate never prompts and these
 * are never called -- stubs that keep the ops table whole in every build. */
static void
_sg_do_save(gpointer p_host, SaveGateSaveDoneFn fn_done, gpointer p_done_data) {
   (void)p_host;
   fn_done(TRUE, p_done_data);
}

static void
_sg_discard(gpointer p_host) {
   (void)p_host;
}
#endif

static const SaveGateHostOps _SAVE_GATE_OPS = {
   .is_dirty          = _sg_is_dirty,
   .do_save           = _sg_do_save,
   .discard           = _sg_discard,
   .show_status       = _sg_show_status,
   .quit_continuation = _proceed_quit,
};

/* Re-apply preferences live when the user edits them in Preferences instead
 * of waiting for a restart: the engine lists + viewer prefs through
 * _on_pref_changed, the folder-shaped keys through _on_folder_pref_changed.
 */
static void
_watch_pref_keys(GgazeWindow *p_win) {
   GSettings *p_gs = settings_get_gsettings(p_win->p_settings);
   if (p_gs == NULL) {
      return;
   }
   static const char *KEYS[] = {
      "destinations",    "editors",    "scripts",
      "enhance-presets", "background", "scroll-behavior",
   };
   for (gsize i = 0; i < G_N_ELEMENTS(KEYS); i++) {
      char *c_sig = g_strdup_printf("changed::%s", KEYS[i]);
      g_signal_connect(p_gs, c_sig, G_CALLBACK(_on_pref_changed), p_win);
      g_free(c_sig);
   }
   static const char *FOLDER_KEYS[] = {
      "sort", "wrap", "hide-raw-sidecars", "hide-trashed", "thumbnail-size",
   };
   for (gsize i = 0; i < G_N_ELEMENTS(FOLDER_KEYS); i++) {
      char *c_sig = g_strdup_printf("changed::%s", FOLDER_KEYS[i]);
      g_signal_connect(p_gs, c_sig, G_CALLBACK(_on_folder_pref_changed), p_win);
      g_free(c_sig);
   }
}

/* The view-load pipeline, thumbnail cache, per-folder trash/grid
 * placeholders, and the configured engines (mover/opener/runner, plus the
 * GEGL enhancer when built in) fed from GSettings. */
static void
_init_engines_and_settings(GgazeWindow *p_win) {
   p_win->p_viewload  = viewload_new(&_VIEWLOAD_OPS, p_win, 4);
   p_win->p_thumb     = thumbnail_new();
   p_win->p_trash     = NULL; /* created on open */
   p_win->p_grid      = NULL; /* created on open */
   p_win->i_grid_size = 128;
   p_win->p_settings  = settings_new();
   p_win->p_mover     = mover_new();
   p_win->p_opener    = opener_new();
   p_win->p_runner    = runner_new();
   p_win->p_undo      = undo_new();
   p_win->p_fileops =
      fileops_new(p_win->p_mover, p_win->p_undo, _fo_report, p_win);
   p_win->p_delete_confirm = delete_confirm_new(&_DELETE_CONFIRM_OPS, p_win);
   p_win->p_save_gate      = save_gate_new(&_SAVE_GATE_OPS, p_win);
   if (p_win->p_settings != NULL) {
      p_win->i_grid_size =
         CLAMP(settings_get_thumbnail_size(p_win->p_settings), 64, 512);
      _watch_pref_keys(p_win);
   }
#if GGAZE_HAVE_GEGL
   _init_enhance_state(p_win); /* must run before _load_engine_lists below,
                                * which feeds settings into the controller's
                                * enhancer once it exists */
#endif
   /* Feed the configured a(ss) lists into the engines now that all of them
    * (incl. the GEGL enhancer) exist. */
   _load_engine_lists(p_win);
}

/* Wrap p_win->p_stack (built already) in a GtkOverlay with the auto-hiding
 * info label floating on top, put that and the enhance side-panel slot in a
 * row, and make the row the window's child. The slot is an empty box until
 * `a` appends the panel to it, so it costs no width while closed. Split out
 * of _init_stack_and_viewer to keep it under the ~30-line convention. */
static void
_init_info_overlay(GgazeWindow *p_win) {
   p_win->p_overlay = gtk_overlay_new();
   gtk_overlay_set_child(GTK_OVERLAY(p_win->p_overlay), p_win->p_stack);
   p_win->p_info = info_overlay_new(GTK_OVERLAY(p_win->p_overlay));
   gtk_widget_set_hexpand(p_win->p_overlay, TRUE);
   gtk_widget_set_vexpand(p_win->p_overlay, TRUE);
   p_win->p_side_slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
   gtk_widget_set_vexpand(p_win->p_side_slot, TRUE);
   GtkWidget *p_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
   gtk_box_append(GTK_BOX(p_row), p_win->p_overlay);
   gtk_box_append(GTK_BOX(p_row), p_win->p_side_slot);
   gtk_window_set_child(GTK_WINDOW(p_win), p_row);
}

/* Header bar, the grid/large GtkStack (+ its info overlay), and the viewer
 * widget. Split out of ggaze_window_init to keep it under CLAUDE.md's
 * 50-line hard limit (tu0 review round 2, issue 5). */
/* One header-bar icon button bound to a win.* action, with the title and
 * live keys from the shortcuts table as its tooltip ("Next image (l / Right)").
 */
static GtkWidget *
_header_button(const char *c_icon, const char *c_action) {
   GtkWidget *p_btn = gtk_button_new_from_icon_name(c_icon);
   gtk_actionable_set_action_name(GTK_ACTIONABLE(p_btn), c_action);
   char *c_tip = shortcuts_tooltip_for_action(c_action);
   gtk_widget_set_tooltip_text(p_btn, c_tip != NULL ? c_tip : c_action);
   g_free(c_tip);
   return (p_btn);
}

/* Append a menu item for c_action whose label comes from the shortcuts
 * table (with the keys in parentheses), so menu and keys cannot drift. */
static void
_menu_add(GMenu *p_menu, const char *c_action, const char *c_fallback) {
   const char *c_title = shortcuts_title_for_action(c_action);
   char       *c_keys  = shortcuts_keys_for_action(c_action);
   char       *c_label =
      c_keys != NULL
         ? g_strdup_printf("%s (%s)", c_title != NULL ? c_title : c_fallback,
                           c_keys)
         : g_strdup(c_title != NULL ? c_title : c_fallback);
   g_menu_append(p_menu, c_label, c_action);
   g_free(c_label);
   g_free(c_keys);
}

/* The main menu (F10): every action, for mouse users and as a key legend. */
static GMenuModel *
_build_main_menu(void) {
   GMenu *p_files = g_menu_new();
   _menu_add(p_files, "win.open", "Open image");
   _menu_add(p_files, "win.open-folder", "Open folder");
   GMenu *p_edit = g_menu_new();
   _menu_add(p_edit, "win.copy", "Copy");
   _menu_add(p_edit, "win.move", "Move to...");
   _menu_add(p_edit, "win.open-external", "Open in...");
   _menu_add(p_edit, "win.run-script", "Run script...");
   _menu_add(p_edit, "win.enhance", "Enhance");
   GMenu *p_trash = g_menu_new();
   _menu_add(p_trash, "win.trash", "Trash");
   _menu_add(p_trash, "win.delete", "Delete permanently");
   _menu_add(p_trash, "win.undo", "Undo");
   _menu_add(p_trash, "win.empty-trash", "Empty Trash");
   GMenu *p_view = g_menu_new();
   _menu_add(p_view, "win.toggle-view", "Toggle grid / large");
   _menu_add(p_view, "win.fullscreen", "Fullscreen");
   _menu_add(p_view, "win.slideshow", "Slideshow");
   _menu_add(p_view, "win.info", "Info overlay");
   _menu_add(p_view, "win.mark-all", "Mark all");
   GMenu *p_app = g_menu_new();
   _menu_add(p_app, "win.preferences", "Preferences");
   _menu_add(p_app, "win.shortcuts", "Keyboard shortcuts");
   _menu_add(p_app, "win.quit", "Quit");
   GMenu *p_menu = g_menu_new();
   g_menu_append_section(p_menu, NULL, G_MENU_MODEL(p_files));
   g_menu_append_section(p_menu, NULL, G_MENU_MODEL(p_edit));
   g_menu_append_section(p_menu, NULL, G_MENU_MODEL(p_trash));
   g_menu_append_section(p_menu, NULL, G_MENU_MODEL(p_view));
   g_menu_append_section(p_menu, NULL, G_MENU_MODEL(p_app));
   g_object_unref(p_files);
   g_object_unref(p_edit);
   g_object_unref(p_trash);
   g_object_unref(p_view);
   g_object_unref(p_app);
   return (G_MENU_MODEL(p_menu));
}

/* Header bar (libadwaita, decision #29): prev/next/toggle on the left, the
 * main menu on the right, every button tooltipped with its keys so the
 * keyboard-first UI is discoverable with a mouse. */
static void
_init_header_bar(GgazeWindow *p_win) {
   GtkWidget *p_header = adw_header_bar_new();
   adw_header_bar_pack_start(
      ADW_HEADER_BAR(p_header),
      _header_button("go-previous-symbolic", "win.prev"));
   adw_header_bar_pack_start(ADW_HEADER_BAR(p_header),
                             _header_button("go-next-symbolic", "win.next"));
   adw_header_bar_pack_start(
      ADW_HEADER_BAR(p_header),
      _header_button("view-grid-symbolic", "win.toggle-view"));
   p_win->p_menu_btn = gtk_menu_button_new();
   gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(p_win->p_menu_btn),
                                 "open-menu-symbolic");
   gtk_menu_button_set_primary(GTK_MENU_BUTTON(p_win->p_menu_btn), TRUE);
   GMenuModel *p_model = _build_main_menu();
   gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(p_win->p_menu_btn), p_model);
   g_object_unref(p_model);
   char *c_tip = shortcuts_tooltip_for_action("win.menu");
   gtk_widget_set_tooltip_text(p_win->p_menu_btn, c_tip);
   g_free(c_tip);
   adw_header_bar_pack_end(ADW_HEADER_BAR(p_header), p_win->p_menu_btn);
   adw_header_bar_pack_end(
      ADW_HEADER_BAR(p_header),
      _header_button("view-fullscreen-symbolic", "win.fullscreen"));
   adw_header_bar_pack_end(
      ADW_HEADER_BAR(p_header),
      _header_button("media-playback-start-symbolic", "win.slideshow"));
   gtk_window_set_titlebar(GTK_WINDOW(p_win), p_header);
}

/* The grid/large GtkStack (+ its info overlay), the empty-state page and the
 * viewer widget. The "grid" child is created on open; until then the empty
 * page tells a first-time user what to do instead of a placeholder label. */
static void
_init_stack_and_viewer(GgazeWindow *p_win) {
   p_win->p_stack = gtk_stack_new();
   gtk_stack_set_transition_type(GTK_STACK(p_win->p_stack),
                                 GTK_STACK_TRANSITION_TYPE_CROSSFADE);
   _init_info_overlay(p_win);

   p_win->p_status_page = adw_status_page_new();
   adw_status_page_set_icon_name(ADW_STATUS_PAGE(p_win->p_status_page),
                                 "image-x-generic-symbolic");
   adw_status_page_set_title(ADW_STATUS_PAGE(p_win->p_status_page),
                             "Nothing open");
   adw_status_page_set_description(
      ADW_STATUS_PAGE(p_win->p_status_page),
      "Press o to open an image, O a folder, or drop one here. "
      "Press ? for the keyboard shortcuts.");
   gtk_stack_add_named(GTK_STACK(p_win->p_stack), p_win->p_status_page,
                       VIEW_NAMES[GGAZE_VIEW_EMPTY]);

   GtkWidget *p_grid = gtk_label_new("");
   gtk_stack_add_named(GTK_STACK(p_win->p_stack), p_grid,
                       VIEW_NAMES[GGAZE_VIEW_GRID]);

   p_win->p_viewer = ggaze_viewer_new();
   gtk_widget_set_hexpand(p_win->p_viewer, TRUE);
   gtk_widget_set_vexpand(p_win->p_viewer, TRUE);
   gtk_stack_add_named(GTK_STACK(p_win->p_stack), p_win->p_viewer,
                       VIEW_NAMES[GGAZE_VIEW_LARGE]);
   g_signal_connect(p_win->p_viewer, "navigate",
                    G_CALLBACK(_on_viewer_navigate), p_win);
   _apply_viewer_prefs(p_win);

   _set_view(p_win, GGAZE_VIEW_EMPTY);
}

/* One win.enhance-N action per addressable preset (1..GGAZE_ENHANCE_MAX_
 * PRESETS), all sharing one handler that reads the index from the action's
 * data -- so the cap lives in enhancer.h and no handler parses its name. */
static void
_add_enhance_actions(GgazeWindow *p_win) {
   for (gint i = 0; i < GGAZE_ENHANCE_MAX_PRESETS; i++) {
      char          *c_name = g_strdup_printf("enhance-%d", i + 1);
      GSimpleAction *p_act  = g_simple_action_new(c_name, NULL);
      g_object_set_data(G_OBJECT(p_act), "ggaze-preset-idx",
                        GINT_TO_POINTER(i));
      g_signal_connect(p_act, "activate", G_CALLBACK(_action_enhance_n), p_win);
      g_action_map_add_action(G_ACTION_MAP(p_win), G_ACTION(p_act));
      g_object_unref(p_act);
      g_free(c_name);
   }
}

static void
ggaze_window_init(GgazeWindow *p_win) {
   _ensure_css();
   _init_engines_and_settings(p_win);
   _init_header_bar(p_win);
   _init_stack_and_viewer(p_win);

   /* Actions + keybindings (decision #10/#12). */
   g_action_map_add_action_entries(G_ACTION_MAP(p_win), ACTIONS,
                                   G_N_ELEMENTS(ACTIONS), p_win);
   _add_enhance_actions(p_win);
   shortcuts_install(GTK_WIDGET(p_win));

   /* File/folder drag-and-drop (decision #27). */
   GtkDropTarget *p_drop =
      gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
   g_signal_connect(p_drop, "drop", G_CALLBACK(_drop_cb), p_win);
   g_signal_connect(p_drop, "enter", G_CALLBACK(_drop_enter_cb), p_win);
   g_signal_connect(p_drop, "leave", G_CALLBACK(_drop_leave_cb), p_win);
   gtk_widget_add_controller(GTK_WIDGET(p_win), GTK_EVENT_CONTROLLER(p_drop));

   /* Native window close (WM "X"/Alt+F4): gate it through the same
    * Save/Discard/Cancel prompt win.quit uses (tu0 review round 2, issue
    * 2) -- see _on_close_request. */
   g_signal_connect(p_win, "close-request", G_CALLBACK(_on_close_request),
                    p_win);
}

/* --- public -------------------------------------------------------------- */

GgazeWindow *
ggaze_window_new(GgazeApp *p_app) {
   return (GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, "application", p_app,
                                     "default-width", 800, "default-height",
                                     600, NULL)));
}

/* Drop the previous navigator (if any) before ggaze_window_open builds a
 * fresh one: disconnect its signal handlers, detach the grid that was
 * watching it, and release it. */
static void
_open_reset_existing_nav(GgazeWindow *p_win) {
   if (p_win->p_nav == NULL) {
      return;
   }
   g_signal_handlers_disconnect_by_data(p_win->p_nav, p_win);
   if (p_win->p_grid != NULL) {
      ggaze_grid_detach(p_win->p_grid);
   }
   viewload_set_navigator(p_win->p_viewload, NULL);
   fileops_set_folder(p_win->p_fileops, NULL, NULL);
   g_clear_object(&p_win->p_nav);
}

/* Resolve the open target: a directory arg lists itself with no initial
 * current file; a file arg lists its parent with that file as the initial
 * current file. *p_out_dir is NULL (nothing else touched) if the parent
 * directory can't be determined. Returns TRUE if p_arg is itself a
 * directory. */
static gboolean
_open_resolve_target(GFile *p_arg, GFile **p_out_dir, GFile **p_out_start) {
   *p_out_start = NULL;
   GFileType e_type =
      g_file_query_file_type(p_arg, G_FILE_QUERY_INFO_NONE, NULL);
   gboolean b_is_dir = (e_type == G_FILE_TYPE_DIRECTORY);
   if (b_is_dir) {
      *p_out_dir = (GFile *)g_object_ref(p_arg);
   } else {
      *p_out_dir   = g_file_get_parent(p_arg);
      *p_out_start = (GFile *)g_object_ref(p_arg);
   }
   return (b_is_dir);
}

/* Build the new navigator for p_dir, wire it up, and point it at p_start (if
 * any). Also resets the per-folder trash/undo state, since a move or trash
 * undo recorded against the folder just left must not silently apply to the
 * new one. NOTE: navigator_set_current_file() only emits "changed" when the
 * resolved index differs from the navigator's default i_current == 0 (e.g. a
 * file that happens to sort first in its folder never triggers it) -- do not
 * rely on that signal to dismiss the info overlay; the caller handles that
 * unconditionally instead (gu0). */
static void
_open_build_navigator(GgazeWindow *p_win, GFile *p_dir, GFile *p_start,
                      GgazeSort e_sort, gboolean b_wrap, gboolean b_hide_raw) {
   p_win->p_nav = navigator_new(p_dir, e_sort, b_wrap, b_hide_raw);
   viewload_set_navigator(p_win->p_viewload, p_win->p_nav);
   g_clear_pointer(&p_win->p_trash, trash_delete);
   mover_clear_last(p_win->p_mover);
   undo_reset(p_win->p_undo);
   g_signal_connect(p_win->p_nav, "changed", G_CALLBACK(_nav_changed_cb),
                    p_win);
   if (p_start != NULL) {
      navigator_set_current_file(p_win->p_nav, p_start);
   }
}

/* Read the sort/wrap/hide-raw/hide-trashed preferences from settings
 * (defaults if the wrapper is absent) for ggaze_window_open() to apply to
 * the freshly-built navigator and grid. */
static void
_open_read_prefs(GgazeWindow *p_win, GgazeSort *pe_sort, gboolean *pb_wrap,
                 gboolean *pb_hide_raw, gboolean *pb_hide_trashed) {
   *pe_sort         = GGAZE_SORT_NAME;
   *pb_wrap         = TRUE;
   *pb_hide_raw     = TRUE;
   *pb_hide_trashed = FALSE;
   if (p_win->p_settings != NULL) {
      *pe_sort         = settings_get_sort(p_win->p_settings);
      *pb_wrap         = settings_get_wrap(p_win->p_settings);
      *pb_hide_raw     = settings_get_hide_raw(p_win->p_settings);
      *pb_hide_trashed = settings_get_hide_trashed(p_win->p_settings);
   }
}

/* Grid signals -> window actions. The grid reports intent ("navigate",
 * "mark-requested", "mark-all-requested"); only the window knows which
 * win.* action that maps to, so the widget stays usable without one. */
static void
_on_grid_navigate(GgazeGrid *p_grid, gint i_dir, gpointer p_data) {
   (void)p_grid;
   gtk_widget_activate_action(GTK_WIDGET(p_data),
                              (i_dir < 0) ? "win.prev" : "win.next", NULL);
}

static void
_on_grid_mark_requested(GgazeGrid *p_grid, gpointer p_data) {
   (void)p_grid;
   gtk_widget_activate_action(GTK_WIDGET(p_data), "win.mark", NULL);
}

static void
_on_grid_mark_all_requested(GgazeGrid *p_grid, gpointer p_data) {
   (void)p_grid;
   gtk_widget_activate_action(GTK_WIDGET(p_data), "win.mark-all", NULL);
}

/* Replace the "grid" stack page with a fresh GgazeGrid bound to the window's
 * (already-built) navigator, and give the folder a fresh Trash instance. */
static void
_open_rebuild_grid(GgazeWindow *p_win, gboolean b_hide_trashed) {
   GtkWidget *p_old =
      gtk_stack_get_child_by_name(GTK_STACK(p_win->p_stack), "grid");
   if (p_old != NULL) {
      if (GGAZE_IS_GRID(p_old)) {
         ggaze_grid_detach(GGAZE_GRID(p_old));
      }
      gtk_stack_remove(GTK_STACK(p_win->p_stack), p_old);
   }
   GFile *p_navdir = navigator_get_dir(p_win->p_nav);
   p_win->p_trash  = trash_new(p_navdir);
   fileops_set_folder(p_win->p_fileops, p_win->p_nav, p_win->p_trash);
   p_win->p_grid = GGAZE_GRID(ggaze_grid_new(
      p_win->p_nav, p_win->p_thumb, p_win->i_grid_size, b_hide_trashed));
   g_signal_connect(p_win->p_grid, "activate", G_CALLBACK(_on_grid_activate),
                    p_win);
   g_signal_connect(p_win->p_grid, "navigate", G_CALLBACK(_on_grid_navigate),
                    p_win);
   g_signal_connect(p_win->p_grid, "mark-requested",
                    G_CALLBACK(_on_grid_mark_requested), p_win);
   g_signal_connect(p_win->p_grid, "mark-all-requested",
                    G_CALLBACK(_on_grid_mark_all_requested), p_win);
   /* Route every grid/thumbnail selection through the dirty-preview gate
    * (tu0 review round 2, issue 1) instead of letting gridview.c call
    * navigator_set_current_file() directly. */
   ggaze_grid_set_select_func(p_win->p_grid, _grid_select_gate, p_win);
   gtk_stack_add_named(GTK_STACK(p_win->p_stack), GTK_WIDGET(p_win->p_grid),
                       "grid");
}

/* After the folder is open: say when the requested file was not what the
 * user thinks it is -- a path that does not exist, or a file that is not an
 * image of this folder -- instead of silently showing the folder's first
 * image under a title that never mentions the mistake. */
static void
_report_open_target(GgazeWindow *p_win, GFile *p_arg, gboolean b_is_dir) {
   if (b_is_dir || p_win->p_nav == NULL) {
      return;
   }
   char *c_name = g_file_get_basename(p_arg);
   if (!g_file_query_exists(p_arg, NULL)) {
      char *c_msg =
         g_strdup_printf("%s not found \u2014 opened its folder", c_name);
      _show_status(p_win, c_msg);
      g_free(c_msg);
   } else {
      GFile *p_cur = navigator_get_current(p_win->p_nav);
      if (p_cur == NULL || !g_file_equal(p_cur, p_arg)) {
         char *c_msg = g_strdup_printf(
            "%s is not an image in this folder \u2014 opened the folder",
            c_name);
         _show_status(p_win, c_msg);
         g_free(c_msg);
      }
   }
   g_free(c_name);
}

/* The actual open logic (was ggaze_window_open's whole body before tu0 added
 * the dirty-preview gate below). Kept as a separate static function so the
 * public entry point can defer it behind a Save/Discard/Cancel prompt
 * without duplicating any of it. */
static void
_open_now(GgazeWindow *p_win, GFile *p_arg) {
   GFile   *p_dir    = NULL;
   GFile   *p_start  = NULL;
   gboolean b_is_dir = _open_resolve_target(p_arg, &p_dir, &p_start);
   if (p_dir == NULL) {
      /* Resolve failed (e.g. a non-directory GFile whose g_file_get_parent()
       * returns NULL) -- bail out before touching anything so the window
       * keeps showing exactly what it showed before this call, info overlay
       * included (gu0 round-2 review: dismissing here would hide a still-
       * valid overlay for an open that never happened). */
      g_clear_object(&p_start);
      return;
   }

   /* Only now that the open is confirmed to proceed: drop any stale info
    * overlay unconditionally before tearing down/rebuilding the navigator --
    * the "changed" signal wired in _open_build_navigator is NOT a reliable
    * trigger here (see its doc comment), so this must not depend on whether
    * it happens to fire (gu0 fresh-context review). Firing before the
    * teardown/rebuild below avoids a window where new content is already
    * showing but the old overlay is still up. */
   _dismiss_info_for_nav(p_win);
   _slideshow_stop(p_win, NULL);
   p_win->i_esc_at = 0;

   _open_reset_existing_nav(p_win);

   GgazeSort e_sort;
   gboolean  b_wrap;
   gboolean  b_hide_raw;
   gboolean  b_hide_trashed;
   _open_read_prefs(p_win, &e_sort, &b_wrap, &b_hide_raw, &b_hide_trashed);
   _open_build_navigator(p_win, p_dir, p_start, e_sort, b_wrap, b_hide_raw);
   g_clear_object(&p_dir);
   g_clear_object(&p_start);

   _open_rebuild_grid(p_win, b_hide_trashed);

   /* Folder arg → start in the thumbnail grid (folder-to-grid behavior,
    * docs/ui-and-interactions.md 33-47); file arg → large view on that image.
    * _load_current is run either way so the large view is ready when toggled.
    */
   _set_view(p_win, b_is_dir ? GGAZE_VIEW_GRID : GGAZE_VIEW_LARGE);
   _load_current(p_win);
   _update_empty_state(p_win);
   _report_open_target(p_win, p_arg, b_is_dir);
}

typedef struct {
   GgazeWindow *p_win; /* owned ref */
   GFile       *p_arg; /* owned ref */
} _OpenCtx;

static gboolean
_proceed_open(gpointer p_data) {
   _OpenCtx *p_ctx = (_OpenCtx *)p_data;
   _open_now(p_ctx->p_win, p_ctx->p_arg);
   return (G_SOURCE_REMOVE);
}

/* See _move_idx_ctx_free: _maybe_save_then owns the ctx on every exit path. */
static void
_open_ctx_free(gpointer p_data) {
   _OpenCtx *p_ctx = (_OpenCtx *)p_data;
   g_object_unref(p_ctx->p_win);
   g_object_unref(p_ctx->p_arg);
   g_free(p_ctx);
}

/* Open p_arg (File->Open dialog, drag-and-drop, or single-instance
 * re-activation onto an existing window) -- see window.h. If an unsaved
 * (GEGL) enhance preview is active on the CURRENTLY displayed file, prompts
 * Save/Discard/Cancel first (the gu0-class gap this task closes: a plain
 * "changed" signal is not a reliable choke point for this path -- see
 * _open_build_navigator's comment -- so the gate lives here, before any
 * teardown/rebuild, not inside _open_now). Proceeds immediately if nothing
 * is dirty. */
void
ggaze_window_open(GgazeWindow *p_win, GFile *p_arg) {
   g_return_if_fail(GGAZE_IS_WINDOW(p_win));
   g_return_if_fail(G_IS_FILE(p_arg));
   _OpenCtx *p_ctx = g_new(_OpenCtx, 1);
   p_ctx->p_win    = (GgazeWindow *)g_object_ref(p_win);
   p_ctx->p_arg    = (GFile *)g_object_ref(p_arg);
   save_gate_maybe_save_then(p_win->p_save_gate, _proceed_open, p_ctx,
                             _open_ctx_free);
}

/* Several files: open the FIRST one's folder in the grid with it current
 * (decision #27). One file: the usual large-view open. */
static gboolean
_proceed_open_many(gpointer p_data) {
   _OpenCtx *p_ctx = (_OpenCtx *)p_data;
   GFile    *p_dir = g_file_get_parent(p_ctx->p_arg);
   if (p_dir == NULL) {
      _open_now(p_ctx->p_win, p_ctx->p_arg);
      return (G_SOURCE_REMOVE);
   }
   _open_now(p_ctx->p_win, p_dir);
   if (p_ctx->p_win->p_nav != NULL) {
      navigator_set_current_file(p_ctx->p_win->p_nav, p_ctx->p_arg);
   }
   g_object_unref(p_dir);
   return (G_SOURCE_REMOVE);
}

void
ggaze_window_open_files(GgazeWindow *p_win, GFile **pp_files, gint i_n_files) {
   g_return_if_fail(GGAZE_IS_WINDOW(p_win));
   if (pp_files == NULL || i_n_files <= 0) {
      return;
   }
   if (i_n_files == 1) {
      ggaze_window_open(p_win, pp_files[0]);
      return;
   }
   _OpenCtx *p_ctx = g_new(_OpenCtx, 1);
   p_ctx->p_win    = (GgazeWindow *)g_object_ref(p_win);
   p_ctx->p_arg    = (GFile *)g_object_ref(pp_files[0]);
   save_gate_maybe_save_then(p_win->p_save_gate, _proceed_open_many, p_ctx,
                             _open_ctx_free);
}

/* Step the cursor by i_dir and give a cue at the folder edge: "Wrapped to
 * first/last image" when wrap took the user around (easy to miss while
 * culling, and the reason people re-review a folder), "First/Last image"
 * when wrap is off and nothing happened. */
static void
_step(GgazeWindow *p_win, gint i_dir) {
   if (p_win->p_nav == NULL) {
      return;
   }
   gint     i_before = navigator_get_current_index(p_win->p_nav);
   gboolean b_moved =
      (i_dir > 0) ? navigator_next(p_win->p_nav) : navigator_prev(p_win->p_nav);
   gint i_after = navigator_get_current_index(p_win->p_nav);
   if (!b_moved) {
      if (navigator_get_remaining(p_win->p_nav) > 1) {
         _show_status(p_win, (i_dir > 0) ? "Last image" : "First image");
      }
      return;
   }
   if (i_before >= 0 && i_after >= 0 &&
       ((i_dir > 0 && i_after < i_before) ||
        (i_dir < 0 && i_after > i_before))) {
      _show_status(p_win, (i_dir > 0) ? "Wrapped to first image"
                                      : "Wrapped to last image");
   }
}

void
ggaze_window_prev(GgazeWindow *p_win) {
   g_return_if_fail(GGAZE_IS_WINDOW(p_win));
   _step(p_win, -1); /* emits "changed" -> _load_current */
}

void
ggaze_window_next(GgazeWindow *p_win) {
   g_return_if_fail(GGAZE_IS_WINDOW(p_win));
   _step(p_win, 1);
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

void
ggaze_window_clear_texture_cache(GgazeWindow *p_win) {
   g_return_if_fail(GGAZE_IS_WINDOW(p_win));
   viewload_clear_cache(p_win->p_viewload);
}

GtkWidget *
ggaze_window_get_info_label(GgazeWindow *p_win) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), NULL);
   return (info_overlay_get_label(p_win->p_info));
}

GtkWidget *
ggaze_window_get_info_histogram(GgazeWindow *p_win) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), NULL);
   return (info_overlay_get_histogram(p_win->p_info));
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
