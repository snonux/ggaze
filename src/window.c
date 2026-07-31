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

/* One request handed to _maybe_save_then: the continuation to run once the
 * (possible) Save/Discard/Cancel prompt resolves, the data it acts on, and the
 * notify that releases that data. _maybe_save_then OWNS data for the whole
 * flow and releases it through fn_free on EVERY exit path -- including the
 * ones that never run fn (Cancel, dialog dismissal, a failed Save, a parked
 * request dropped because the prompt was cancelled). fn_free is NULL when data
 * is not owned by the request (e.g. the window itself). */
typedef struct {
   GSourceFunc    fn;
   gpointer       data;
   GDestroyNotify fn_free;
} _Request;

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
   GgazeGrid    *p_grid;      /* the thumbnail grid (the "grid" stack child) */
   int           i_grid_size; /* current thumbnail size (64-512, decision T) */
   GtkWidget    *p_overlay; /* GtkOverlay wrapping the stack (for info label) */
   GtkWidget    *p_info_lbl;  /* info overlay label (auto-hides) */
   guint         u_info_hide; /* info auto-hide timeout id (0=none) */
   guint         u_slideshow; /* slideshow timeout id (0=off) */
   gboolean      b_fullscreen;
   guint         u_hdr_hide;      /* fullscreen header auto-hide timeout */
   gboolean      b_disposed;      /* set in dispose; async callbacks check it */
   GCancellable *p_delete_cancel; /* the outstanding `D` delete-confirm
                                   * dialog's GCancellable, and THE record
                                   * that such a dialog is up: non-NULL for
                                   * exactly as long as one is on screen
                                   * (_delete_confirm_ask sets it,
                                   * _delete_confirm_cb clears it, and that
                                   * callback always runs). Two jobs, one
                                   * slot: it answers "is a modal dialog
                                   * outstanding?" for _on_close_request,
                                   * and it lets dispose force the dialog's
                                   * GTask to complete instead of leaving
                                   * the _DeleteCtx it carries -- with its
                                   * deep-copied target list -- hanging.
                                   * Not under GGAZE_HAVE_GEGL: the delete
                                   * confirm exists in every build */
#if GGAZE_HAVE_GEGL
   guint8     u_enhance_mask;      /* bit i -> preset i enabled (layered) */
   Enhancer  *p_enhancer;          /* GEGL preset engine (NULL w/o GEGL) */
   GtkWidget *p_enhance_pop;       /* `a` preset popover (NULL when none) */
   GtkWidget *p_enhance_btns[8];   /* popover preset rows, for highlighting;
                                    * only valid while the popover is open,
                                    * NULL'd out on close */
   GdkTexture *p_enhance_tex;      /* last-applied modified texture, cached
                                    * so hold-Space can restore it without a
                                    * GEGL recompute */
   GCancellable *p_enhance_cancel; /* in-flight enhance-apply GTask */
   guint         u_enhance_gen;    /* bumped on every apply/discard; a
                                    * completion whose request predates the
                                    * current value is stale and dropped
                                    * (last-write-wins -- GEGL processing
                                    * cannot be aborted mid-flight once
                                    * started) */
   gboolean b_hold_original;       /* TRUE while Space is held (hold-compare) */
   gboolean b_save_prompt;         /* TRUE while the modal Save/Discard/Cancel
                                    * prompt is outstanding, so a trigger the
                                    * modal grab cannot swallow (Alt+F4 / the
                                    * WM close button reach "close-request"
                                    * without being input events, and a
                                    * single-instance open arrives over D-Bus)
                                    * cannot stack a second dialog on top of
                                    * it, nor slip its continuation past the
                                    * gate while that dialog is still up */
   GCancellable *p_prompt_cancel;  /* the outstanding prompt's GCancellable
                                    * (NULL when no prompt is up), handed to
                                    * gtk_alert_dialog_choose so dispose can
                                    * force the dialog's GTask to complete
                                    * instead of leaving it (and the _SaveCtx
                                    * it carries) hanging -- see
                                    * _prompt_dispose. Exactly one prompt is
                                    * ever outstanding (b_save_prompt), so this
                                    * single slot always names THAT prompt */
   gboolean b_prompt_quits;        /* TRUE while the outstanding prompt's OWN
                                    * continuation is _proceed_quit, i.e.
                                    * answering it in favour of proceeding
                                    * closes the window. A queued request must
                                    * then be dropped rather than retried into
                                    * a window that is going away (round 4,
                                    * finding r). Cleared with the slot in
                                    * _save_prompt_flush */
   _Request *p_pending;            /* the ONE request parked while the prompt
                                    * above is outstanding (newest wins), so a
                                    * second, different request is not silently
                                    * swallowed: it is retried through the same
                                    * gate once the prompt resolves in favour
                                    * of proceeding, and dropped with a status
                                    * line when it resolves to Cancel (round 3,
                                    * finding i). NULL when nothing is parked */
   GFile *p_enhance_file;          /* file the current mask/preview applies to
                                    * (NULL = none); lets nav_changed_cb tell
                                    * an actual navigation apart from a
                                    * same-file rescan (e.g. the folder's
                                    * GFileMonitor noticing the "-enhanced"
                                    * copy win.enhance-save just wrote next
                                    * to the original) so a save doesn't
                                    * silently discard its own still-active
                                    * preview */
#endif
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
/* Shared by the enhance/open-external/run-script/move popovers (decision O):
 * auto-assigned hotkeys in list order (1-9, 0, a-z) and the keyval->index
 * mapping. Defined once, below, near their first historical use. */
static char _popup_hotkey_char(guint u_idx);
static gint _popup_key_to_index(guint u_keyval);
#if GGAZE_HAVE_GEGL
static void     _enhance_update_highlights(GgazeWindow *p_win);
static void     _enhance_apply_async(GgazeWindow *p_win);
static void     _enhance_discard(GgazeWindow *p_win);
static gboolean _enhance_do_save(GgazeWindow *p_win);
static void     _enhance_destroy(GgazeWindow *p_win);
static gboolean _space_pressed_cb(GtkEventControllerKey *p_c, guint u_keyval,
                                  guint u_kc, GdkModifierType e_state,
                                  gpointer p_data);
static gboolean _space_released_cb(GtkEventControllerKey *p_c, guint u_keyval,
                                   guint u_kc, GdkModifierType e_state,
                                   gpointer p_data);
#endif

/* Ask (if needed) before running fn(data), see the two _maybe_save_then
 * definitions below. See _Request for the ownership rules. */
static void _maybe_save_then(GgazeWindow *p_win, GSourceFunc fn, gpointer data,
                             GDestroyNotify fn_free_data);

/* Give p_req its one chance (run the continuation iff b_proceed), then release
 * its data. The single place that decides "this request is done" -- so no exit
 * path can forget one of the two halves (tu0 review round 2, finding b: Cancel
 * and the dismiss path used to return early and leak the heap ctx, each of
 * which holds an owned window ref and therefore pinned an entire
 * GgazeWindow). */
static void
_request_finish(const _Request *p_req, gboolean b_proceed) {
   if (b_proceed && p_req->fn != NULL) {
      p_req->fn(p_req->data);
   }
   if (p_req->fn_free != NULL) {
      p_req->fn_free(p_req->data);
   }
}

/* Navigation continuations used by _maybe_save_then after the (GEGL) save
 * /discard dialog resolves. They are trivial wrappers over the public
 * navigation API and do not depend on GEGL, so they are always compiled
 * (the _action_prev/next/first/last handlers reference them regardless of
 * the GEGL build configuration). Their data is the GgazeWindow itself (not
 * owned by the continuation -- _SaveCtx holds its own ref for the dialog's
 * lifetime), so they are registered with a NULL fn_free_data. */
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
 * handler. It lives up here with its navigation siblings because the prompt
 * machinery below has to recognise it by identity: a prompt whose own
 * continuation closes the window may not retry a queued request afterwards
 * (round 4, finding r -- see _save_prompt_show/_save_prompt_flush). */
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

/* TRUE iff p_file is STILL an existing file in the folder p_win navigates
 * right now. The single-file counterpart of
 * ggaze_window_delete_targets_still_current, and the last thing checked before
 * a captured target is trashed or deleted.
 *
 * Two independent things can invalidate a target captured at key-press time,
 * and the guard covers both:
 *
 *   1. The FOLDER was replaced (single-instance open / drop). This leg is
 *      defence-in-depth only: since the one-prompt guard is checked before the
 *      "nothing is dirty" fast path (round 3, finding j), every folder-
 *      replacing path is QUEUED behind an outstanding prompt rather than
 *      executed, so it cannot fire today. It stays because what makes acting
 *      on a captured target safe is the invariant, not the current call graph.
 *   2. The FILE was removed externally while the prompt was up. This leg is
 *      live: the folder's GFileMonitor is a plain GSource that keeps firing
 *      behind the input-only modal grab. Without the existence check the
 *      target sailed past the guard and failed deep inside trash_bin /
 *      trash_permanently_delete with a bare g_warning and nothing on screen
 *      (round 4, finding u) -- so callers now report a refusal instead. */
static gboolean
_target_still_in_folder(GgazeWindow *p_win, GFile *p_file) {
   if (p_win->p_nav == NULL || p_file == NULL) {
      return (FALSE);
   }
   GFile   *p_dir    = navigator_get_dir(p_win->p_nav);
   GFile   *p_parent = g_file_get_parent(p_file);
   gboolean b_ok =
      (p_dir != NULL && p_parent != NULL && g_file_equal(p_dir, p_parent));
   g_clear_object(&p_parent);
   return (b_ok && g_file_query_exists(p_file, NULL));
}

/* --- captured target SETS ------------------------------------------------
 *
 * The files a marks-or-current action (`D` delete, `m` move) acts on: every
 * marked file if any are marked, else just the current one
 * (docs/ui-and-interactions.md "Selection & moving").
 *
 * Deriving this ONCE, at key-press time, is the whole point: the
 * marks-vs-current DECISION is as perishable as navigator.current itself.
 * navigator.c's _relist() prunes marks whose file left the listing, and the
 * folder GFileMonitor that triggers it keeps firing behind the modal prompt,
 * so a mark set of 1 can shrink to 0 while the dialog is up -- after which a
 * re-derived "no marks" leg would act on the CURRENT file, one the user
 * neither marked nor chose (round 4, finding p).
 *
 * Transfer full: caller frees with g_list_free_full(..., g_object_unref), or
 * hands the list to _files_ctx_init. */
static GList *
_capture_targets(GgazeWindow *p_win) {
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

/* Independently-owned copy of a captured target list (transfer full), for the
 * one place that has to outlive the _FilesCtx it borrowed from: the >1-target
 * delete confirm dialog. */
static GList *
_files_copy(GList *p_files) {
   GList *p_out = NULL;
   for (GList *p_it = p_files; p_it != NULL; p_it = p_it->next) {
      p_out = g_list_prepend(p_out, g_object_ref(G_FILE(p_it->data)));
   }
   return (g_list_reverse(p_out));
}

/* TRUE iff navigator.current is one of p_files. Computed BEFORE anything is
 * removed, because navigator_mark_removed emits "changed" and can move
 * current (and invalidate the borrowed pointer) as a side effect. */
static gboolean
_files_include_current(GgazeWindow *p_win, GList *p_files) {
   GFile *p_cur = navigator_get_current(p_win->p_nav);
   if (p_cur == NULL) {
      return (FALSE);
   }
   for (GList *p_it = p_files; p_it != NULL; p_it = p_it->next) {
      if (g_file_equal(p_cur, G_FILE(p_it->data))) {
         return (TRUE);
      }
   }
   return (FALSE);
}

/* --- modal alert dialogs: shared plumbing --------------------------------
 *
 * Both of this window's modal GtkAlertDialogs -- the GEGL Save/Discard/Cancel
 * prompt and the >1-target delete confirm -- need the same two things: a ref
 * on the private toplevel GTK put up for them (below), and a GCancellable the
 * window can cancel when it disposes (see _delete_confirm_dispose, and
 * _prompt_dispose in a GEGL build). This lives outside the GEGL block because
 * the delete confirm exists in every build -- so every Save-prompt name
 * mentioned from here on is a GEGL-only counterpart, cited to show the shared
 * pattern, not a symbol a minimal build has.
 */

/* The GtkWindow gtk_alert_dialog_choose() has just presented for p_win,
 * reffed (transfer full). NULL only if GTK ever stops using a plain toplevel
 * for it.
 *
 * GtkAlertDialog keeps that window entirely private -- it exists only as the
 * dialog GTask's task data -- but it IS an ordinary toplevel, appended to
 * gtk_window_get_toplevels() by gtk_window_constructed(), so scanning that
 * model backwards for the newest window transient-for p_win finds it. Callers
 * call this immediately after choose(), when theirs is that newest one by
 * construction. What makes the backwards scan sufficient is that ordering
 * alone, not an enumeration of the alternatives: anything else transient-for
 * p_win (the other alert dialog, and _action_open's GtkFileDialog on the
 * non-portal path, where it really is a local toplevel) can only be OLDER, so
 * scanning from the end reaches ours first whatever else is up.
 *
 * Why a ref is needed at all: the dialog is transient-for p_win with
 * destroy-with-parent set, and GTK wires that to p_win's ::destroy, which
 * GtkWidget emits from dispose. Once dispose cancels the dialog, its GTask
 * completes in an IDLE -- i.e. after dispose has returned, and so after that
 * ::destroy already destroyed the dialog window and the toplevel list dropped
 * its last reference. The GTask still holds that window as a raw pointer (its
 * task data), so gtk_alert_dialog_choose_finish() calls gtk_window_destroy()
 * on it: without a reference of ours that is freed memory -- measured as
 * "assertion 'GTK_IS_WINDOW (window)' failed" the moment a cancel made that
 * callback run at all. With one, the window is merely destroyed-but-alive,
 * and the second gtk_window_destroy() returns at once because the window is
 * no longer in the toplevel list. Measured on gtk 4.22.4. */
static GtkWindow *
_alert_dialog_window(GgazeWindow *p_win) {
   GListModel *p_tops = gtk_window_get_toplevels();
   for (guint u = g_list_model_get_n_items(p_tops); u > 0; u--) {
      GtkWindow *p_top = GTK_WINDOW(g_list_model_get_item(p_tops, u - 1));
      if (gtk_window_get_transient_for(p_top) == GTK_WINDOW(p_win)) {
         return (p_top);
      }
      g_object_unref(p_top);
   }
   return (NULL);
}

#if GGAZE_HAVE_GEGL
/* Split p_base ("IMG_0001.jpg") into a stem ("IMG_0001") and a saver-
 * supported extension (defaults to ".jpg" if p_base's own extension is not
 * one _saver_for_ext's caller set supports, matching the "defaults to the
 * original format" contract in docs/gegl.md). *pc_stem is caller-owned. */
static void
_enhance_split_name(const char *c_base, char **pc_stem, const char **pc_ext) {
   const char *c_dot = strrchr(c_base, '.');
   *pc_ext           = ".jpg";
   if (c_dot != NULL && (g_ascii_strcasecmp(c_dot, ".jpg") == 0 ||
                         g_ascii_strcasecmp(c_dot, ".jpeg") == 0 ||
                         g_ascii_strcasecmp(c_dot, ".png") == 0 ||
                         g_ascii_strcasecmp(c_dot, ".webp") == 0)) {
      *pc_ext = c_dot;
   }
   *pc_stem = (c_dot != NULL && *pc_ext == c_dot)
                 ? g_strndup(c_base, (gsize)(c_dot - c_base))
                 : g_strdup(c_base);
}

/* Build a non-colliding export destination in p_dir: "<stem>-enhanced<ext>",
 * or "<stem>-enhanced-<n><ext>" (n = 1, 2, ...) the first time that name is
 * already taken -- mirroring mover.c's move-collision suffixing (mover_move,
 * decision #26/docs/IMPLEMENTATION.md M9 "collision -1") so the two copy-
 * style flows in this codebase behave the same way. Never overwrites an
 * existing file. */
static GFile *
_enhance_unique_dest(GFile *p_dir, const char *c_stem, const char *c_ext) {
   char  *c_name = g_strdup_printf("%s-enhanced%s", c_stem, c_ext);
   GFile *p_out  = g_file_get_child(p_dir, c_name);
   g_free(c_name);
   for (guint n = 1; g_file_query_exists(p_out, NULL); n++) {
      g_object_unref(p_out);
      c_name = g_strdup_printf("%s-enhanced-%u%s", c_stem, n, c_ext);
      p_out  = g_file_get_child(p_dir, c_name);
      g_free(c_name);
   }
   return (p_out);
}

/* Report the outcome of an export via _show_status (+ g_warning on failure),
 * mirroring _move_report's success/failure split. Frees p_out and p_err. */
static void
_enhance_save_report(GgazeWindow *p_win, GFile *p_out, gboolean b_ok,
                     GError *p_err) {
   char *c_saved = g_file_get_basename(p_out);
   g_object_unref(p_out);
   if (b_ok) {
      char *c_msg = g_strdup_printf("Saved %s", c_saved);
      _show_status(p_win, c_msg);
      g_free(c_msg);
   } else {
      g_warning("ggaze: enhance-save failed: %s",
                p_err != NULL ? p_err->message : "(no detail)");
      char *c_msg = g_strdup_printf("Enhance-save failed: %s",
                                    p_err != NULL ? p_err->message : "?");
      _show_status(p_win, c_msg);
      g_free(c_msg);
   }
   g_free(c_saved);
   g_clear_error(&p_err);
}

/* Compute the non-colliding "<stem>-enhanced[-<n>].<ext>" destination for
 * p_file in its own folder. Caller unrefs. */
static GFile *
_enhance_dest_for(GFile *p_file) {
   char       *c_base = g_file_get_basename(p_file);
   char       *c_stem;
   const char *c_ext;
   _enhance_split_name(c_base, &c_stem, &c_ext);
   GFile *p_dir = g_file_get_parent(p_file);
   GFile *p_out = _enhance_unique_dest(p_dir, c_stem, c_ext);
   g_free(c_stem);
   g_free(c_base);
   g_object_unref(p_dir);
   return (p_out);
}

/* TRUE iff there is actually an enhance preview to export right now. Split out
 * of _enhance_do_save so its callers can tell "nothing to save" apart from
 * "the export failed": _enhance_do_save returns FALSE for both, and treating
 * the first as a failure made the Save button silently do nothing AND cancel
 * the user's action (tu0 review round 3, finding k).
 *
 * The subject is p_enhance_file -- the file the mask/preview was computed FOR
 * (_enhance_launch sets it) -- not a fresh navigator_get_current(). Save runs
 * from a dialog callback, so it is one of the deferred paths that must not
 * re-derive its target: _enhance_nav_changed does clear the mask whenever
 * current's identity changes, which makes the two equal today, but that is an
 * invariant holding a permanent write together rather than a reason to depend
 * on it. Asking the preview which file it belongs to needs no invariant. */
static gboolean
_enhance_can_save(GgazeWindow *p_win) {
   return (p_win->p_nav != NULL && p_win->p_enhancer != NULL &&
           p_win->u_enhance_mask != 0 && p_win->p_enhance_file != NULL);
}

/* Export the previewed image with the enabled-preset chain to a non-colliding
 * "<stem>-enhanced[-<n>].<ext>" in the same folder. Returns TRUE on success.
 * The original file is never touched: enhancer_export_chain reads it
 * (enhancer_load) and writes only to the freshly computed destination. The
 * subject is p_enhance_file, the file the preview belongs to -- see
 * _enhance_can_save. */
static gboolean
_enhance_do_save(GgazeWindow *p_win) {
   if (!_enhance_can_save(p_win)) {
      return (FALSE);
   }
   GFile      *p_file = p_win->p_enhance_file;
   GFile      *p_out  = _enhance_dest_for(p_file);
   GError     *p_err  = NULL;
   GeglBuffer *p_buf  = enhancer_load(p_file, &p_err);
   gboolean    b_ok   = FALSE;
   if (p_buf != NULL) {
      const GPtrArray *p_presets = enhancer_get_presets(p_win->p_enhancer);
      b_ok = enhancer_export_chain(p_win->p_enhancer, p_buf, p_presets,
                                   p_win->u_enhance_mask, p_out, &p_err);
      g_object_unref(p_buf);
   }
   _enhance_save_report(p_win, p_out, b_ok, p_err);
   return (b_ok);
}

typedef struct {
   GgazeWindow *p_win;      /* owned ref: outlives the async dialog (mirrors
                             * _MoveIdxCtx/_OpenCtx/_DeleteCtx's convention) */
   _Request      t_req;     /* the gated request; see _Request for ownership */
   GCancellable *p_cancel;  /* owned ref on the SAME GCancellable the window
                             * parks in p_prompt_cancel. Held here as well so
                             * the callback can ask the object itself whether
                             * this prompt was cancelled, and so the ref is
                             * released on every exit path even after dispose
                             * has already dropped the window's own -- see
                             * _save_prompt_outcome and _prompt_dispose. */
   GtkWindow *p_dlg_window; /* owned ref on the dialog's own toplevel, purely
                             * to keep it from being FREED under the GTask
                             * that still points at it -- see
                             * _alert_dialog_window */
} _SaveCtx;

/* How an outstanding prompt ended. Kept apart from the button index because
 * two of the three outcomes carry no index at all, and because folding them
 * together is what made the dismissal case invisible (task 8w0): the callback
 * used to treat "an error came back" as plain Cancel and say nothing. */
typedef enum {
   _PROMPT_ANSWERED,  /* the user pressed a button; the index says which */
   _PROMPT_CANCELLED, /* ggaze cancelled it: the window is being disposed */
   _PROMPT_DISMISSED  /* closed without an answer, or any other failure */
} _PromptOutcome;

/* Release the parked request p_req without ever running it, and say so. Takes
 * ownership of p_req (both the _Request box and, via _request_finish, the data
 * it carries).
 *
 * b_window_gone picks the channel. While the window is alive the info overlay
 * is the right place. Once it is closing or disposed there is no overlay left
 * to paint on -- and the requests that reach this slot are precisely the ones
 * that did NOT come from this window's keyboard (a single-instance
 * `ggaze other.jpg` over D-Bus, a drop), so the log is where the person who
 * made the request is actually looking. */
static void
_drop_pending(GgazeWindow *p_win, _Request *p_req, gboolean b_window_gone) {
   _request_finish(p_req, FALSE);
   g_free(p_req);
   if (b_window_gone || p_win->b_disposed) {
      g_message("ggaze: queued request dropped — the window is closing");
      return;
   }
   _show_status(p_win, "Queued request dropped");
}

/* Hand the parked request (if any) back to the gate now that the prompt is
 * gone. b_proceed carries the answer: Save or Discard RESOLVED the preview, so
 * the parked request is simply retried through _maybe_save_then and -- the
 * mask now being clear -- runs straight away. Cancel (and a failed Save) means
 * "stay on this image, keep the preview"; running the parked request anyway
 * would do precisely what the user just refused, so it is dropped instead, with
 * a status line so the intent does not vanish unsignalled.
 *
 * The one answer that proceeds and STILL must not retry is a prompt whose own
 * continuation was the quit (b_prompt_quits): by the time the flush runs, that
 * continuation has already called gtk_window_close(), so retrying would rebuild
 * a whole Navigator + GFileMonitor + texture load inside a window that is going
 * away -- observed landing either there or as a silent drop depending on how
 * far dispose had got (round 4, finding r). A closing window can honour no
 * request, so the queue is dropped, deliberately and audibly.
 *
 * Retrying through the gate rather than calling the continuation directly is
 * what keeps this loop-free: the retry either runs immediately (mask clear) or
 * opens ONE fresh prompt that now owns the request as its own continuation, so
 * nothing can be parked twice. */
static void
_save_prompt_flush(GgazeWindow *p_win, gboolean b_proceed) {
   _Request *p_req = p_win->p_pending;
   /* Clear both first: the retry below may park a new request behind a fresh
    * prompt, which then owns the flag as well as the slot. */
   p_win->p_pending      = NULL;
   gboolean b_was_quit   = p_win->b_prompt_quits;
   p_win->b_prompt_quits = FALSE;
   if (p_req == NULL) {
      return;
   }
   if (b_proceed && !b_was_quit) {
      _maybe_save_then(p_win, p_req->fn, p_req->data, p_req->fn_free);
      g_free(p_req);
      return;
   }
   /* b_proceed here means the window is closing, so there is no overlay left
    * to paint on (and its widgets may already be gone); a Cancel leaves the
    * window very much alive, so that one is reported on screen as before. */
   _drop_pending(p_win, p_req, b_proceed);
}

/* Single exit point for the dialog callback: optionally run the continuation
 * (b_proceed), release the continuation's data, then let whatever was parked
 * behind this prompt have its turn. Ordering matters -- fn still needs data
 * and the window, and the flush needs the window too, so the ctx's window ref
 * is dropped last. */
static void
_save_ctx_finish(_SaveCtx *p_ctx, gboolean b_proceed) {
   GgazeWindow *p_win = p_ctx->p_win; /* borrowed until the unref below */
   _request_finish(&p_ctx->t_req, b_proceed);
   g_object_unref(p_ctx->p_cancel);
   g_clear_object(&p_ctx->p_dlg_window);
   g_free(p_ctx);
   _save_prompt_flush(p_win, b_proceed);
   g_object_unref(p_win);
}

/* The Save button. Returns TRUE iff the user's original action may proceed.
 *
 * "Nothing to save" is NOT a failure: the preview can legitimately be gone by
 * the time the prompt is answered (the slideshow timer and the folder's
 * GFileMonitor both run behind a modal dialog, and either can clear the mask),
 * and there is then nothing left to protect. Reporting it and proceeding is
 * the honest outcome; the old code returned FALSE here, which silently wrote
 * nothing, showed nothing, and cancelled the user's action as well (tu0 review
 * round 3, finding k).
 *
 * A real export failure (read-only folder, full disk, unreadable original)
 * does return FALSE: it must not be silently downgraded to Discard -- the mask
 * and preview stay so the user can retry, and the window stays alive so
 * _enhance_save_report's error message is readable rather than painted onto a
 * window the continuation was about to close (round 2, finding a). */
static gboolean
_save_dialog_save(GgazeWindow *p_win) {
   if (!_enhance_can_save(p_win)) {
      _show_status(p_win, "Nothing to save \u2014 the preview is gone");
      return (TRUE);
   }
   return (_enhance_do_save(p_win));
}

/* Classify a finished prompt. p_err is what gtk_alert_dialog_choose_finish()
 * reported (NULL means a button index came back).
 *
 * The CANCELLABLE, not the GError, is the authority on "we cancelled this".
 * GTask's check_cancellable makes g_task_propagate_int() rewrite the result of
 * a cancelled task into G_IO_ERROR_CANCELLED no matter what GTK put in it, so
 * the domain/code alone cannot tell our own dispose-time cancel apart from
 * GTK's GTK_DIALOG_ERROR_CANCELLED -- and that same rewrite is why a cancel
 * issued after a button was pressed comes back looking like a cancel, which
 * tu0's round-3 probe hit and 2w0 re-measured (see _prompt_dispose for the
 * numbers and for why this cancel cannot land in that order). Asking the
 * object we cancelled ourselves is exact.
 *
 * That rewrite also means _PROMPT_ANSWERED is only ever reported when nothing
 * cancelled the prompt, which is what keeps a user's Save or Discard from
 * being silently downgraded on any path that does NOT cancel. */
static _PromptOutcome
_save_prompt_outcome(const _SaveCtx *p_ctx, const GError *p_err) {
   if (p_err == NULL) {
      return (_PROMPT_ANSWERED);
   }
   if (g_cancellable_is_cancelled(p_ctx->p_cancel)) {
      return (_PROMPT_CANCELLED);
   }
   return (_PROMPT_DISMISSED);
}

/* Say so, once, when a prompt ended without an answer. Both outcomes mean
 * "do not proceed", but they are different events and folding them into a
 * silent Cancel is what left task 8w0 with nothing to look at: CANCELLED is
 * ggaze tearing the window down under the dialog, DISMISSED is the dialog
 * going away without a choice.
 *
 * DISMISSED is not reachable while _save_prompt_show configures a cancel
 * button: GTK's response_cb turns a delete-event into that button's index
 * (cancel_return), so an Escape/WM-close arrives as a plain Cancel answer, and
 * GTK_DIALOG_ERROR_DISMISSED is only produced when no cancel button is set
 * (gtk 4.22.4 gtkalertdialog.c). The branch stays because that is a GTK
 * configuration detail, not an invariant of this callback -- and it is also
 * where any genuinely unexpected failure would surface. */
static void
_report_unanswered_prompt(_PromptOutcome e_outcome) {
   if (e_outcome == _PROMPT_CANCELLED) {
      g_message("ggaze: Save/Discard/Cancel prompt cancelled — the window is "
                "going away");
      return;
   }
   g_message("ggaze: Save/Discard/Cancel prompt dismissed — treated as Cancel");
}

/* Alert-dialog response: 0=Cancel, 1=Discard, 2=Save. */
static void
_save_dialog_cb(GObject *p_dlg, GAsyncResult *p_res, gpointer p_data) {
   _SaveCtx    *p_ctx = (_SaveCtx *)p_data;
   GgazeWindow *p_win = p_ctx->p_win;
   GError      *p_err = NULL;
   gint         i_btn =
      gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(p_dlg), p_res, &p_err);
   g_object_unref(GTK_ALERT_DIALOG(p_dlg));
   _PromptOutcome e_outcome = _save_prompt_outcome(p_ctx, p_err);
   g_clear_error(&p_err);
   /* This prompt is gone, and so is its cancellable's job: a later trigger may
    * open a fresh one, which brings its own (see _maybe_save_then). Clearing
    * the window's slot here is safe even after _prompt_dispose already cleared
    * it -- the ctx keeps the object alive until _save_ctx_finish. */
   p_win->b_save_prompt = FALSE;
   g_clear_object(&p_win->p_prompt_cancel);
   if (e_outcome != _PROMPT_ANSWERED) { /* cancelled/dismissed -> as Cancel */
      _report_unanswered_prompt(e_outcome);
      _save_ctx_finish(p_ctx, FALSE);
      return;
   }
   if (i_btn == 2 && !_save_dialog_save(p_win)) { /* Save, but it failed */
      _save_ctx_finish(p_ctx, FALSE);
      return;
   }
   if (i_btn == 1 || i_btn == 2) { /* Discard, or a Save that may proceed */
      _enhance_discard(p_win);
      _save_ctx_finish(p_ctx, TRUE);
      return;
   }
   _save_ctx_finish(p_ctx, FALSE); /* Cancel: keep the preview, do not
                                    * proceed -- but still free the ctx */
}

/* Build and show the modal Save/Discard/Cancel prompt for p_win, handing the
 * request over to _save_dialog_cb. Split out of _maybe_save_then to keep both
 * under the ~30-line convention.
 *
 * b_prompt_quits records whether THIS prompt's own continuation closes the
 * window, which is what _save_prompt_flush needs to know: a queued request
 * must not be retried into a window the answer is about to close (round 4,
 * finding r). Both quit entry points -- win.quit and the native
 * "close-request" -- share _proceed_quit, so comparing against it catches
 * both.
 *
 * The GCancellable is not optional bookkeeping: without one, nothing can ever
 * finish this dialog's GTask except a button press, so a dispose under a live
 * prompt abandoned the _SaveCtx and every ctx the request carries -- 479 bytes
 * in 11 allocations, measured under ASan (task 2w0). Both this ctx and the
 * window hold a ref on it; see _prompt_dispose for the cancelling end.
 *
 * What that does NOT cover is a plain gtk_window_destroy() with the prompt
 * still up, because it never reaches dispose: gtk_window_destroy() drops ONE
 * reference, the toplevel list's, and the _SaveCtx's own window ref keeps the
 * count off zero (measured on gtk 4.22.4: one ref dropped both for a window
 * built with a GtkApplication, as production does, and for one built without,
 * as the test harness does -- the application's window list holds no counted
 * reference of its own; the count then holds steady across 5 s of draining).
 * GTK wires destroy-with-parent to the parent's ::destroy, which GtkWidget
 * emits from dispose -- so the dialog stays up and clickable, and answering it
 * then still resolves everything normally (measured: the answer arrives as the
 * pressed button, refcount back to the caller's own).
 *
 * Nothing in src/ calls gtk_window_destroy(), and _on_close_request now
 * refuses a close while the prompt is outstanding, so no ggaze code path
 * reaches that state. What is left uncovered is a process that EXITS under
 * the dialog (SIGTERM, a session logout, ^C): dispose never runs at all
 * there, so the cancel below never fires and the contexts go down with the
 * process. */
static void
_save_prompt_show(GgazeWindow *p_win, const _Request *p_req) {
   GtkAlertDialog *p_dlg =
      gtk_alert_dialog_new("Save the enhanced copy before leaving this image?");
   static const char *const c_btns[] = {"Cancel", "Discard", "Save", NULL};
   gtk_alert_dialog_set_buttons(p_dlg, c_btns);
   gtk_alert_dialog_set_default_button(p_dlg, 2);
   gtk_alert_dialog_set_cancel_button(p_dlg, 0);
   gtk_alert_dialog_set_modal(p_dlg, TRUE);
   _SaveCtx *p_ctx = g_new(_SaveCtx, 1);
   p_ctx->p_win    = (GgazeWindow *)g_object_ref(p_win);
   p_ctx->t_req    = *p_req;
   p_ctx->p_cancel = g_cancellable_new();
   /* Every field initialised BEFORE choose(), so the ctx is never handed to a
    * callback holding uninitialised memory. The real assignment below happens
    * only after choose() returns (the dialog window does not exist before
    * that); today nothing can run _save_dialog_cb in between -- GTask defers
    * completion to an idle and gtk_window_present() does not iterate the main
    * context -- but _save_ctx_finish's g_clear_object(&p_ctx->p_dlg_window)
    * would read garbage the day either of those changes. */
   p_ctx->p_dlg_window = NULL;
   /* Already NULL -- _maybe_save_then's b_save_prompt guard is what gets us
    * here, and _save_dialog_cb clears the slot with that flag. Cleared anyway
    * so the slot can never silently accumulate a second ref. */
   g_clear_object(&p_win->p_prompt_cancel);
   p_win->p_prompt_cancel = (GCancellable *)g_object_ref(p_ctx->p_cancel);
   p_win->b_save_prompt   = TRUE;
   p_win->b_prompt_quits  = (p_req->fn == _proceed_quit);
   gtk_alert_dialog_choose(p_dlg, GTK_WINDOW(p_win), p_ctx->p_cancel,
                           _save_dialog_cb, p_ctx);
   p_ctx->p_dlg_window = _alert_dialog_window(p_win); /* transfer full */
}

/* Park p_req until the outstanding prompt is answered. Exactly one slot, and
 * the newest request wins: a user who hits Alt+F4 five times is asking for one
 * quit, not five, and the alternative (an unbounded queue) would replay a
 * backlog of actions the user can no longer see the reason for. The request it
 * replaces is released, never run.
 *
 * What keeps the user's second request from LOOKING ignored is not the status
 * line below -- that one is best-effort at best, since the modal dialog is
 * covering the very overlay it paints on and _show_status auto-hides it after
 * a couple of seconds anyway (round 4, finding s). It is what happens AFTER
 * the answer: the request either runs (with whatever visible effect it has of
 * its own) or is dropped with an explicit "Queued request dropped". The line
 * here is kept because a dialog the user has dragged aside does reveal it, and
 * it costs nothing -- but the guarantee lives on the far side of the prompt.
 *
 * p_req->data needs no ref of its own: the window is kept alive by the
 * outstanding prompt's _SaveCtx, and _save_ctx_finish flushes this slot before
 * releasing that ref. */
static void
_save_prompt_queue(GgazeWindow *p_win, const _Request *p_req) {
   if (p_win->p_pending != NULL) { /* displace: newest wins */
      _request_finish(p_win->p_pending, FALSE);
      g_free(p_win->p_pending);
   }
   p_win->p_pending  = g_new(_Request, 1);
   *p_win->p_pending = *p_req;
   _show_status(p_win, "Answer the Save/Discard/Cancel prompt first");
}

/* If an enhance preview is active (unsaved), ask Save / Discard / Cancel
 * before proceeding with fn. If no preview, just run fn.
 *
 * At most ONE prompt is ever outstanding per window. The modal grab swallows
 * further input-driven triggers, but a native close (Alt+F4 / the WM close
 * button) is not an input event, and a single-instance re-activation arrives
 * over D-Bus: three Alt+F4s used to stack three live dialogs, and answering
 * Save on each wrote three separate "-enhanced" copies and fired the
 * continuation three times (round 2, finding d).
 *
 * The one-prompt guard is therefore checked FIRST, ahead of the "nothing is
 * dirty" fast path. Checking it second let a request slip straight through to
 * its continuation the moment something cleared the mask under a live dialog
 * -- two actions out of one visible prompt, which is the very thing the guard
 * exists to prevent (round 3, finding j). */
static void
_maybe_save_then(GgazeWindow *p_win, GSourceFunc fn, gpointer data,
                 GDestroyNotify fn_free_data) {
   _Request t_req = {fn, data, fn_free_data};
   if (p_win->b_save_prompt) {
      _save_prompt_queue(p_win, &t_req);
      return;
   }
   if (p_win->u_enhance_mask == 0 || p_win->p_enhancer == NULL) {
      _request_finish(&t_req, TRUE);
      return;
   }
   _save_prompt_show(p_win, &t_req);
}

/* TRUE while this window's modal Save/Discard/Cancel prompt is outstanding.
 * Exists so _on_close_request -- which lives outside the GEGL block -- can ask
 * the question without reaching into a field that only exists in the GEGL
 * build, the same way it uses ggaze_window_enhance_is_dirty(). */
static gboolean
_save_prompt_outstanding(GgazeWindow *p_win) {
   return (p_win->b_save_prompt);
}

#else /* !GGAZE_HAVE_GEGL */
static void
_maybe_save_then(GgazeWindow *p_win, GSourceFunc fn, gpointer data,
                 GDestroyNotify fn_free_data) {
   (void)p_win;
   _Request t_req = {fn, data, fn_free_data};
   _request_finish(&t_req, TRUE);
}

static gboolean
_save_prompt_outstanding(GgazeWindow *p_win) {
   (void)p_win; /* no prompt exists without GEGL */
   return (FALSE);
}
#endif

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
   return (p_win->p_delete_cancel != NULL);
}

/* TRUE while ANY modal dialog this window owns is outstanding.
 *
 * Task 2w0 gated _on_close_request on b_save_prompt, which answers a narrower
 * question -- "is the Save/Discard/Cancel prompt up?" -- and left the delete
 * confirm, raised AFTER that prompt has resolved, unguarded (task aw0). This
 * is the question the close gate actually wants, so it is asked once, here,
 * and every future modal dialog belongs in it rather than in a third flag. */
static gboolean
_modal_dialog_outstanding(GgazeWindow *p_win) {
   return (_save_prompt_outstanding(p_win) ||
           _delete_confirm_outstanding(p_win));
}

/* Select gate installed on every grid (ggaze_grid_set_select_func, wired in
 * _open_rebuild_grid): gridview.c calls this instead of
 * navigator_set_current_file() directly for every grid/thumbnail selection
 * path (double-click/Enter, middle-click mark, j/k cursor move, toggle-to-
 * large sync), so an active unsaved GEGL enhance preview gets the same
 * Save/Discard/Cancel prompt as h/l/g/G/scroll/quit/d/D/m instead of being
 * silently discarded by nav_changed_cb before the window ever sees the click
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
   _maybe_save_then(p_win, _proceed_grid_select, _file_ctx_new(p_win, p_file),
                    _file_ctx_free);
   return (FALSE);
}

/* --- actions ------------------------------------------------------------- */

static void
_action_prev(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _maybe_save_then(GGAZE_WINDOW(p_data), _proceed_prev, p_data, NULL);
}

static void
_action_next(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _maybe_save_then(GGAZE_WINDOW(p_data), _proceed_next, p_data, NULL);
}

static void
_action_first(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _maybe_save_then(GGAZE_WINDOW(p_data), _proceed_first, p_data, NULL);
}

static void
_action_last(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _maybe_save_then(GGAZE_WINDOW(p_data), _proceed_last, p_data, NULL);
}

/* `q`: quit. If an unsaved (GEGL) enhance preview is active, prompts
 * Save/Discard/Cancel first (docs/gegl.md, IMPLEMENTATION.md M9 "navigate/
 * d/D/m/quit with dirty"); quits immediately if nothing is dirty. */
static void
_action_quit(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   _maybe_save_then(GGAZE_WINDOW(p_data), _proceed_quit, p_data, NULL);
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
 * _save_dialog_cb clears b_save_prompt before running it. */
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
   _maybe_save_then(p_win, _proceed_quit, p_win, NULL);
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

/* Bin p_target (the file `d` was pressed on, captured then -- NOT a fresh read
 * of navigator.current, see _FileCtx) and advance past it. The cursor is only
 * advanced when the binned file really was the current one: after a prompt,
 * current may already have moved on by itself, and advancing again would skip
 * an image the user never looked at. */
static void
_do_trash_now(GgazeWindow *p_win, GFile *p_target) {
   if (p_win->p_nav == NULL || p_win->p_trash == NULL || p_target == NULL) {
      return;
   }
   if (!_target_still_in_folder(p_win, p_target)) {
      /* Either the file was removed externally behind the prompt (the live
       * case) or the folder was replaced (defence-in-depth) -- see
       * _target_still_in_folder. Both mean "the thing `d` was pressed on is
       * not there any more", which the user has to be told: this used to fail
       * silently, or later, inside trash_bin (round 4, finding u). */
      _show_status(p_win, "Nothing trashed \u2014 the file is gone");
      return;
   }
   GFile   *p_cur         = navigator_get_current(p_win->p_nav);
   gboolean b_was_current = (p_cur != NULL && g_file_equal(p_cur, p_target));
   GError  *p_err         = NULL;
   if (trash_bin(p_win->p_trash, p_target, &p_err)) {
      navigator_mark_removed(p_win->p_nav, p_target); /* dim; emits changed */
      if (b_was_current) {
         navigator_next(p_win->p_nav); /* advance; emits changed */
      }
      p_win->e_last_destructive = GGAZE_LAST_TRASH; /* for unified win.undo */
   } else {
      g_warning("ggaze: trash failed: %s", p_err->message);
      g_clear_error(&p_err);
   }
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
   GFile       *p_cur =
      p_win->p_nav != NULL ? navigator_get_current(p_win->p_nav) : NULL;
   _maybe_save_then(p_win, _proceed_trash, _file_ctx_new(p_win, p_cur),
                    _file_ctx_free);
}

/* --- bulk-delete safety: captured, immutable target context -------------
 *
 * The >1-mark delete opens an async GtkAlertDialog. While it is pending, a
 * single-instance open / drop can replace the folder (ggaze_window_open swaps
 * p_nav). The dialog callback must therefore NOT re-read the navigator's
 * marks; it deletes the targets captured here at prompt time, and only if the
 * window still navigates the folder those targets came from
 * (ggaze_window_delete_targets_still_current).
 */
typedef struct {
   GgazeWindow *p_win; /* owned ref: outlives the async dialog */
   GFile       *p_dir; /* the captured targets' own parent folder (owned) */
   GList *p_files;     /* captured target GFile* list (owned, transfer full) */
   GCancellable *p_cancel;  /* owned: created here, and the SAME object the
                             * window parks a SECOND ref to in p_delete_cancel.
                             * This is the ref handed to
                             * gtk_alert_dialog_choose(), and the one that keeps
                             * the object alive for the whole life of the ctx --
                             * released on every exit path, and independent of
                             * the window's slot, which _delete_confirm_cb
                             * clears first thing and _delete_confirm_dispose
                             * may clear earlier still. The callback never asks
                             * this object anything (unlike _save_prompt_outcome
                             * for the Save prompt): _delete_confirm_answered_yes
                             * classifies on the button index and the GError
                             * alone */
   GtkWindow *p_dlg_window; /* owned ref on the dialog's own toplevel, purely
                             * to keep it from being FREED under the GTask
                             * that still points at it -- see
                             * _alert_dialog_window */
} _DeleteCtx;

static void
_delete_ctx_free(_DeleteCtx *p_ctx) {
   if (p_ctx == NULL) {
      return;
   }
   g_clear_object(&p_ctx->p_dir);
   g_list_free_full(p_ctx->p_files, (GDestroyNotify)g_object_unref);
   g_clear_object(&p_ctx->p_cancel);
   g_clear_object(&p_ctx->p_dlg_window);
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

/* Permanently delete each file in p_files (the captured target set). A NULL
 * list is a no-op rather than a bare cursor advance -- there is nothing to
 * skip past when nothing was deleted.
 *
 * The cursor is advanced only when one of the deleted files really was the
 * current one, exactly as _do_trash_now does: the targets are captured at
 * key-press time, so by the time a Save/Discard/Cancel prompt is answered
 * current may have moved on by itself, and advancing again would skip an
 * image the user never saw (round 4, finding q -- the same bug _do_trash_now
 * already fixed, left standing in the delete twin).
 *
 * A failure is reported on screen as well as logged: with the targets
 * captured earlier, an individual file can legitimately have vanished between
 * capture and delete, and that must not be a silent no-op (finding u). */
static void
_do_delete_files(GgazeWindow *p_win, GList *p_files) {
   if (p_files == NULL) {
      return;
   }
   gboolean b_was_current = _files_include_current(p_win, p_files);
   guint    u_failed      = 0;
   for (GList *p_it = p_files; p_it != NULL; p_it = p_it->next) {
      GFile  *p_f   = G_FILE(p_it->data);
      GError *p_err = NULL;
      if (trash_permanently_delete(p_win->p_trash, p_f, &p_err)) {
         navigator_mark_removed(p_win->p_nav, p_f);
      } else {
         g_warning("ggaze: delete failed: %s", p_err->message);
         g_clear_error(&p_err);
         u_failed++;
      }
   }
   if (u_failed > 0) {
      char *c_msg = g_strdup_printf("Delete failed for %u file%s", u_failed,
                                    u_failed == 1 ? "" : "s");
      _show_status(p_win, c_msg);
      g_free(c_msg);
   }
   if (b_was_current) {
      navigator_next(p_win->p_nav); /* skip removed entries -> next live (or
                                     * park at -1 when none remain, which
                                     * clears the viewer via "changed") */
   }
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

/* Button indices of the confirm dialog, in the order _delete_confirm_ask
 * hands them to gtk_alert_dialog_set_buttons(). */
enum {
   _DELETE_BTN_CANCEL = 0,
   _DELETE_BTN_DELETE = 1
};

/* Did the user really press "Delete"? i_btn and p_err are what
 * gtk_alert_dialog_choose_finish() returned and reported.
 *
 * This is a guard, not bookkeeping. choose_finish returns -1 AND sets an error
 * for every non-answer -- measured on gtk 4.22.4: a GCancellable cancel gives
 * G_IO_ERROR_CANCELLED, closing the dialog without choosing gives
 * GTK_DIALOG_ERROR_DISMISSED -- and -1 read as a gboolean is TRUE. The old
 * `gboolean b_ok = gtk_alert_dialog_choose_finish(...)` therefore took every
 * such non-answer as a confirmed PERMANENT delete of the captured targets
 * (task aw0). So: the error is checked first, and only the "Delete" button's
 * own index counts as a yes.
 *
 * A cancelled or dismissed confirm is deliberately silent. It means "no files
 * were touched", exactly like pressing Cancel, which says nothing either. */
static gboolean
_delete_confirm_answered_yes(int i_btn, const GError *p_err) {
   return (p_err == NULL && i_btn == _DELETE_BTN_DELETE);
}

static void
_delete_confirm_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   GtkAlertDialog *p_dlg = GTK_ALERT_DIALOG(p_src);
   _DeleteCtx     *p_ctx = (_DeleteCtx *)p_data;
   GError         *p_err = NULL;
   int             i_btn = gtk_alert_dialog_choose_finish(p_dlg, p_res, &p_err);
   /* This dialog is gone, and so is its cancellable's job: a later `D` may
    * open a fresh one, which brings its own. Clearing the window's slot here
    * is safe even after _delete_confirm_dispose already cleared it -- the ctx
    * keeps the cancellable, and the window, alive until _delete_ctx_free. */
   g_clear_object(&p_ctx->p_win->p_delete_cancel);
   if (_delete_confirm_answered_yes(i_btn, p_err)) {
      /* Delete the captured targets (NOT a re-read of p_nav marks): if the
       * folder was replaced while this dialog was pending, the safety check
       * inside ggaze_window_delete_captured refuses and no files are touched.
       */
      ggaze_window_delete_captured(p_ctx->p_win, p_ctx->p_dir, p_ctx->p_files);
   }
   g_clear_error(&p_err);
   _delete_ctx_free(p_ctx);
}

/* The context _delete_confirm_cb will run against. p_dir is transfer-full;
 * p_files is BORROWED and deep-copied here.
 *
 * Every field is initialised before the caller's gtk_alert_dialog_choose(), so
 * the ctx is never handed to a callback holding uninitialised memory --
 * p_dlg_window included, which the caller can only fill in AFTER choose()
 * because the dialog window does not exist before it (same reasoning, and the
 * same GTask-completes-in-an-idle assumption, as the Save prompt's
 * _save_prompt_show -- which, unlike this ctx, only exists in a GEGL build).
 */
static _DeleteCtx *
_delete_ctx_new(GgazeWindow *p_win, GFile *p_dir, GList *p_files) {
   _DeleteCtx *p_ctx   = g_new(_DeleteCtx, 1);
   p_ctx->p_win        = (GgazeWindow *)g_object_ref(p_win);
   p_ctx->p_dir        = p_dir;                /* transfer full */
   p_ctx->p_files      = _files_copy(p_files); /* owned by the context now */
   p_ctx->p_cancel     = g_cancellable_new();
   p_ctx->p_dlg_window = NULL;
   return (p_ctx);
}

/* Ask before permanently deleting the >1 captured targets p_files (decision
 * #38). p_files is BORROWED (its owner is the _FilesCtx `D` captured it into,
 * which _maybe_save_then frees as soon as the continuation returns), so the
 * dialog's own _DeleteCtx takes a deep copy: the async callback must still
 * hold the exact file set the prompt counted, long after that ctx is gone.
 *
 * The GCancellable is not optional bookkeeping, for the same two reasons the
 * Save prompt's is (task 2w0, extended to this dialog by aw0): nothing but the
 * dialog itself can finish its GTask, so a dispose underneath it would abandon
 * the _DeleteCtx and the deep copy it carries; and while it is outstanding the
 * window must refuse a native close, which _on_close_request asks
 * _delete_confirm_outstanding -- i.e. this very slot. */
static void
_delete_confirm_ask(GgazeWindow *p_win, GList *p_files) {
   if (_delete_confirm_outstanding(p_win)) {
      /* One confirm at a time, so the slot always names THE dialog on screen.
       * Unreachable through the UI (the dialog is modal and `D` is input-
       * driven), but it is what makes that invariant structural rather than
       * incidental -- and 2w0's finding was precisely that "modal" does not
       * cover the paths which are not input events. No status line: the
       * overlay it would paint on is behind the dialog. */
      return;
   }
   /* The guard folder comes from the CAPTURED targets, not from the live
    * navigator: the question _delete_confirm_cb has to answer is "do these
    * targets still belong to the folder they came from", and deriving it
    * from navigator_get_dir would only answer "did the folder change since
    * this dialog opened" -- true today merely because of call ordering
    * elsewhere, not by construction (round 5, finding y2). */
   GFile *p_dir = g_file_get_parent(G_FILE(p_files->data)); /* owned */
   if (p_dir == NULL) {
      /* A target with no parent (a filesystem root) cannot be guarded, so
       * refuse out loud rather than silently, like every other refusal leg
       * (round 4, finding u). */
      _show_status(p_win, "Nothing deleted — the folder is gone");
      return;
   }
   GtkAlertDialog *p_dlg = gtk_alert_dialog_new(
      "Permanently delete %u marked images?", g_list_length(p_files));
   gtk_alert_dialog_set_buttons(p_dlg,
                                (const char *[]){"Cancel", "Delete", NULL});
   /* Escape / the dialog's own WM close then arrive as a plain Cancel answer
    * instead of GTK_DIALOG_ERROR_DISMISSED (GTK's response_cb turns a delete-
    * event into cancel_return). Belt and braces with
    * _delete_confirm_answered_yes: on a destructive dialog, "the user got rid
    * of the question" must mean no. */
   gtk_alert_dialog_set_cancel_button(p_dlg, _DELETE_BTN_CANCEL);
   /* No gtk_alert_dialog_set_default_button() on purpose. GtkAlertDialog
    * initialises default_button to -1 and only calls
    * gtk_dialog_set_default_response() for the button whose index matches (gtk
    * 4.22.4 gtkalertdialog.c:82 and :658), so with none set Enter activates
    * nothing and a stray keypress cannot confirm a PERMANENT delete. If a
    * default is ever added it MUST be _DELETE_BTN_CANCEL -- never
    * _DELETE_BTN_DELETE, which would make Enter the destructive answer. */
   _DeleteCtx *p_ctx = _delete_ctx_new(p_win, p_dir, p_files);
   /* The slot is NULL here -- the guard at the top of this function is what
    * got us past, and _delete_confirm_cb clears it -- so this assignment can
    * never drop a ref on the floor. Written BEFORE choose(), so the gate is
    * already closed by the time anything can react to the dialog. */
   p_win->p_delete_cancel = (GCancellable *)g_object_ref(p_ctx->p_cancel);
   gtk_alert_dialog_choose(p_dlg, GTK_WINDOW(p_win), p_ctx->p_cancel,
                           _delete_confirm_cb, p_ctx);
   p_ctx->p_dlg_window = _alert_dialog_window(p_win); /* transfer full */
   g_object_unref(p_dlg);
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
 * is checked against _target_still_in_folder first, so a target that vanished
 * behind the prompt is refused out loud rather than failing silently. */
static void
_do_delete_now(GgazeWindow *p_win, GList *p_files) {
   if (p_win->p_nav == NULL || p_win->p_trash == NULL || p_files == NULL) {
      return;
   }
   if (p_files->next != NULL) { /* >1 captured target */
      _delete_confirm_ask(p_win, p_files);
      return;
   }
   if (!_target_still_in_folder(p_win, G_FILE(p_files->data))) {
      _show_status(p_win, "Nothing deleted — the file is gone");
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
   _maybe_save_then(p_win, _proceed_delete,
                    _files_ctx_new(p_win, _capture_targets(p_win)),
                    _files_ctx_free);
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
      return;
   }
   /* Leaving the grid: sync navigator.current to the highlighted cell so the
    * large view opens the selected image. Since tu0 that sync goes through
    * _grid_select_gate, and its return value is meaningful (round 2, finding
    * c): TRUE means current really moved, in which case nav_changed_cb has
    * ALREADY run _load_current and repeating it here would only be a
    * redundant second paint. FALSE means either a no-op (the highlighted cell
    * is already current) or that a dirty enhance preview deferred the change
    * behind the Save/Discard/Cancel prompt -- then nothing loaded it, so load
    * it here; _show_texture keeps an unanswered preview on screen. */
   gboolean b_moved =
      p_win->p_grid != NULL && ggaze_grid_sync_current(p_win->p_grid);
   gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack), "large");
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
#if GGAZE_HAVE_GEGL
   if (p_win->u_enhance_mask != 0 && p_win->p_enhancer != NULL) {
      /* Esc's first job, ahead of every other contextual-back rung below: an
       * active enhance preview is discarded outright, no Save/Discard/Cancel
       * prompt (docs/ui-and-interactions.md "Quick enhance": Esc/re-press
       * always discards directly). One contextual step per press, same as
       * the mark-clear/fullscreen-exit rungs. */
      _enhance_discard(p_win);
      return;
   }
#endif
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
/* Per-request context for _enhance_apply_async's async completion: the
 * window (ref'd, so it outlives the worker even across a dispose) and the
 * generation the request was launched at, for the last-write-wins check in
 * _enhance_apply_done_cb. */
typedef struct {
   GgazeWindow *p_win;
   guint        u_gen;
} _EnhanceReq;

static void
_enhance_req_free(_EnhanceReq *p_req) {
   if (p_req == NULL) {
      return;
   }
   g_object_unref(p_req->p_win);
   g_free(p_req);
}

/* Update each popover preset row's "ggaze-enhance-on" highlight from the
 * mask. A no-op when the popover is closed (rows are NULL'd by
 * _enhance_destroy), so callers never need to check p_enhance_pop first. */
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

/* Async apply completion (tu0): last-write-wins via u_enhance_gen -- a
 * newer apply, a discard, or the window closing while this was still
 * processing in its worker thread all bump the generation, so a stale
 * result here is dropped instead of clobbering whatever is now current. */
static void
_enhance_apply_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   _EnhanceReq *p_req = (_EnhanceReq *)p_data;
   GgazeWindow *p_win = p_req->p_win;
   GError      *p_err = NULL;
   GdkTexture  *p_tex = enhancer_apply_chain_finish(p_res, &p_err);
   if (p_win->b_disposed || p_req->u_gen != p_win->u_enhance_gen) {
      g_clear_object(&p_tex);
      g_clear_error(&p_err);
      _enhance_req_free(p_req);
      return;
   }
   if (p_tex == NULL) {
      g_warning("ggaze: enhance failed: %s",
                p_err != NULL ? p_err->message : "(no detail)");
      g_clear_error(&p_err);
      _show_status(p_win, "Enhance failed");
      _enhance_discard(p_win); /* back to the original; also bumps the gen */
   } else {
      g_set_object(&p_win->p_enhance_tex, p_tex);
      _show_texture(p_win, p_tex);
      g_object_unref(p_tex);
      _update_header(p_win);
   }
   _enhance_req_free(p_req);
}

/* Switch to large view (if not already there) and start a fresh generation:
 * bump u_enhance_gen and replace p_enhance_cancel, so a still-in-flight
 * older apply's result is recognized as stale and dropped when it
 * eventually completes (last-write-wins; GEGL processing itself cannot be
 * aborted mid-flight once started). Split out of _enhance_apply_async to
 * keep it under the ~30-line convention. */
static void
_enhance_apply_begin(GgazeWindow *p_win) {
   const char *c_cur =
      gtk_stack_get_visible_child_name(GTK_STACK(p_win->p_stack));
   if (g_strcmp0(c_cur, "large") != 0) {
      gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack), "large");
   }
   p_win->u_enhance_gen++;
   g_cancellable_cancel(p_win->p_enhance_cancel);
   g_clear_object(&p_win->p_enhance_cancel);
   p_win->p_enhance_cancel = g_cancellable_new();
}

/* Launch the async apply for the non-empty mask case: record which file the
 * preview applies to (see _enhance_nav_changed's comment for why this is
 * set here, not only in nav_changed_cb) and hand off to
 * enhancer_apply_chain_async. Split out of _enhance_apply_async (30-line
 * convention). */
static void
_enhance_launch(GgazeWindow *p_win, GFile *p_file) {
   g_set_object(&p_win->p_enhance_file, p_file);
   _EnhanceReq *p_req         = g_new(_EnhanceReq, 1);
   p_req->p_win               = (GgazeWindow *)g_object_ref(p_win);
   p_req->u_gen               = p_win->u_enhance_gen;
   const GPtrArray *p_presets = enhancer_get_presets(p_win->p_enhancer);
   enhancer_apply_chain_async(p_win->p_enhancer, p_file, p_presets,
                              p_win->u_enhance_mask, p_win->p_enhance_cancel,
                              _enhance_apply_done_cb, p_req);
}

/* Apply the enabled-preset chain (u_enhance_mask) to the current image as a
 * live preview, off the GTK main thread (enhancer_apply_chain_async: GEGL
 * processing is CPU-heavy, AGENTS.md "Decode runs in GTask threads"). An
 * empty mask restores the original synchronously (texturecache is cheap, no
 * GEGL involved). */
static void
_enhance_apply_async(GgazeWindow *p_win) {
   if (p_win->p_nav == NULL || p_win->p_enhancer == NULL) {
      return;
   }
   _enhance_apply_begin(p_win);
   if (p_win->u_enhance_mask == 0) {
      /* Canonical "mask went empty" site -- every path that clears it
       * (Esc/discard, the popover's "0 Original" row, and the easy-to-miss
       * one: toggling the LAST enabled preset back off via win.enhance-N or
       * a popover row) funnels through here. Force the hold-compare flag
       * off: ggaze_window_set_hold_original no-ops once nothing is dirty, so
       * a Space RELEASE arriving after the mask was cleared out from under a
       * still-held key would otherwise leave the flag stuck TRUE and swallow
       * the next press (tu0 review round 2, issue 4). */
      p_win->b_hold_original = FALSE;
      g_clear_object(&p_win->p_enhance_tex);
      _load_current(p_win); /* restore original (texturecache is fast) */
      _update_header(p_win);
      return;
   }
   GFile *p_file = navigator_get_current(p_win->p_nav);
   if (p_file == NULL) {
      p_win->u_enhance_mask  = 0;
      p_win->b_hold_original = FALSE; /* see the mask==0 branch above */
      _enhance_update_highlights(p_win);
      _update_header(p_win);
      return;
   }
   _enhance_launch(p_win, p_file);
}

/* Drop the current enhance preview and go back to showing the unmodified
 * original: clears the mask + cached texture and reloads the original
 * (_enhance_apply_async's mask==0 path also invalidates any in-flight apply
 * via u_enhance_gen). Used by Esc (explicit discard, no prompt), the
 * popover's "0 Original" row/hotkey, and after Save/Discard in the
 * navigate-away prompt. Never touches the file on disk -- discarding a
 * preview only drops in-memory state. */
static void
_enhance_discard(GgazeWindow *p_win) {
   p_win->u_enhance_mask  = 0;
   p_win->b_hold_original = FALSE; /* belt-and-braces: _enhance_apply_async's
                                    * mask==0 branch below also does this,
                                    * but it early-returns without a
                                    * navigator/enhancer (issue 4) */
   _enhance_update_highlights(p_win);
   _enhance_apply_async(p_win);
}

/* Clicked row: idx 0..7 toggles that preset's bit; idx -1 (Original) discards
 * the whole preview. Then refresh highlights + re-apply the (possibly empty)
 * chain. Does NOT close the popover -- toggling presets while comparing is
 * the point of the layered design (docs/gegl.md). */
static void
_enhance_row_toggle(GgazeWindow *p_win, GtkWidget *p_btn) {
   gint i_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(p_btn), "idx"));
   if (i_idx < 0) {
      _enhance_discard(p_win);
      return;
   }
   if (i_idx < (gint)G_N_ELEMENTS(p_win->p_enhance_btns)) {
      p_win->u_enhance_mask ^= (guint8)(1u << i_idx);
   }
   _enhance_update_highlights(p_win);
   _enhance_apply_async(p_win);
}

/* Synchronously tear down the current enhance popover (unparent + clear the
 * field and its now-dangling row pointers). Safe to call when none is open.
 * The popover's "closed" handler (autohide / outside-click / Esc) also
 * routes here. Closing the popover never touches u_enhance_mask -- the
 * preview persists until explicitly discarded (Esc when no popover is open,
 * or the "0 Original" row/hotkey). */
static void
_enhance_destroy(GgazeWindow *p_win) {
   if (p_win->p_enhance_pop == NULL) {
      return;
   }
   GtkWidget *p_pop     = p_win->p_enhance_pop;
   p_win->p_enhance_pop = NULL;
   for (guint i = 0; i < G_N_ELEMENTS(p_win->p_enhance_btns); i++) {
      p_win->p_enhance_btns[i] = NULL;
   }
   gtk_widget_unparent(p_pop);
}

static void
_enhance_closed_cb(GtkPopover *p_pop, gpointer p_data) {
   (void)p_pop;
   _enhance_destroy(GGAZE_WINDOW(p_data));
}

/* Popover key controller: Esc closes the popover only (the preview, if any,
 * stays -- Esc discards it via win.back's dedicated rung when the popover is
 * NOT open, see _action_back); '0' discards the whole preview outright
 * (the "0 Original" row's hotkey); any other bound digit/letter toggles that
 * preset's bit without closing the popover. Modified keys are propagated. */
static gboolean
_enhance_key_pressed_cb(GtkEventControllerKey *p_c, guint u_keyval, guint u_kc,
                        GdkModifierType e_state, gpointer p_data) {
   (void)p_c;
   (void)u_kc;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (u_keyval == GDK_KEY_Escape) {
      _enhance_destroy(p_win);
      return (GDK_EVENT_STOP);
   }
   if (e_state != 0) {
      return (GDK_EVENT_PROPAGATE);
   }
   if (u_keyval == GDK_KEY_0) {
      _enhance_discard(p_win);
      return (GDK_EVENT_STOP);
   }
   gint i_idx = _popup_key_to_index(u_keyval);
   if (i_idx < 0 || i_idx >= (gint)G_N_ELEMENTS(p_win->p_enhance_btns)) {
      return (GDK_EVENT_PROPAGATE);
   }
   p_win->u_enhance_mask ^= (guint8)(1u << i_idx);
   _enhance_update_highlights(p_win);
   _enhance_apply_async(p_win);
   return (GDK_EVENT_STOP);
}

/* Build the popover's title row: "Enhance <basename>:" (or a bare "Enhance:"
 * if the current file can't be named). Split out of _enhance_build_box to
 * keep it under the ~30-line convention. */
static void
_enhance_build_title(GgazeWindow *p_win, GtkWidget *p_box) {
   char  *c_title = NULL;
   GFile *p_cur   = navigator_get_current(p_win->p_nav);
   if (p_cur != NULL) {
      char *c_name = g_file_get_basename(p_cur);
      c_title      = g_strdup_printf("Enhance %s:", c_name);
      g_free(c_name);
   }
   GtkWidget *p_lbl = gtk_label_new(c_title != NULL ? c_title : "Enhance:");
   gtk_widget_set_halign(p_lbl, GTK_ALIGN_START);
   gtk_box_append(GTK_BOX(p_box), p_lbl);
   g_free(c_title);
}

/* Append one toggle row per preset (capped at the 8-bit mask's width) plus
 * the "0 Original" reset row and the "s" save hint, wiring each preset row's
 * hotkey + current highlight. Split out of _enhance_build_box (30-line
 * convention). */
static void
_enhance_build_rows(GgazeWindow *p_win, GtkWidget *p_box,
                    const GPtrArray *p_presets) {
   guint u_n = p_presets != NULL ? p_presets->len : 0;
   if (u_n > G_N_ELEMENTS(p_win->p_enhance_btns)) {
      u_n = G_N_ELEMENTS(p_win->p_enhance_btns); /* the mask is 8 bits wide */
   }
   for (guint i = 0; i < u_n; i++) {
      const EnhancerPreset *p_pr = g_ptr_array_index((GPtrArray *)p_presets, i);
      char                  c_hk = _popup_hotkey_char(i);
      char                 *c_lbl =
         g_strdup_printf("%c  %s", c_hk != 0 ? c_hk : ' ',
                         p_pr->c_name != NULL ? p_pr->c_name : "(unnamed)");
      GtkWidget *p_btn = gtk_button_new_with_label(c_lbl);
      gtk_widget_set_halign(p_btn, GTK_ALIGN_START);
      g_object_set_data(G_OBJECT(p_btn), "idx", GINT_TO_POINTER((gint)i));
      g_signal_connect_swapped(p_btn, "clicked",
                               G_CALLBACK(_enhance_row_toggle), p_win);
      if ((p_win->u_enhance_mask & (guint8)(1u << i)) != 0) {
         gtk_widget_add_css_class(p_btn, "ggaze-enhance-on");
      }
      gtk_box_append(GTK_BOX(p_box), p_btn);
      p_win->p_enhance_btns[i] = p_btn;
      g_free(c_lbl);
      if (i == 0) {
         gtk_widget_grab_focus(p_btn); /* ensure the popover gets keys */
      }
   }
   GtkWidget *p_btn0 = gtk_button_new_with_label("0  Original");
   gtk_widget_set_halign(p_btn0, GTK_ALIGN_START);
   g_object_set_data(G_OBJECT(p_btn0), "idx", GINT_TO_POINTER(-1));
   g_signal_connect_swapped(p_btn0, "clicked", G_CALLBACK(_enhance_row_toggle),
                            p_win);
   gtk_box_append(GTK_BOX(p_box), p_btn0);

   GtkWidget *p_hint = gtk_label_new("s  Save enhanced copy");
   gtk_widget_set_halign(p_hint, GTK_ALIGN_START);
   gtk_widget_set_margin_top(p_hint, 8);
   gtk_widget_add_css_class(p_hint, "dim-label");
   gtk_box_append(GTK_BOX(p_box), p_hint);
}

/* Build the popover's content box (title + preset rows). Split out of
 * _action_enhance to keep that under ~30 lines, mirroring _move_build_box. */
static GtkWidget *
_enhance_build_box(GgazeWindow *p_win, const GPtrArray *p_presets) {
   GtkWidget *p_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
   gtk_widget_set_margin_start(p_box, 8);
   gtk_widget_set_margin_end(p_box, 8);
   gtk_widget_set_margin_top(p_box, 8);
   gtk_widget_set_margin_bottom(p_box, 8);
   _enhance_build_title(p_win, p_box);
   _enhance_build_rows(p_win, p_box, p_presets);
   return (p_box);
}

/* win.enhance (key 'a'): open the preset popover (same GtkPopover +
 * hotkey-assignment pattern as `m`/`e`/`!`, docs/gegl.md). Toggles closed on
 * a second press, same as the other popups. Unlike them, a row click/hotkey
 * does NOT close the popover -- presets are layered toggles, and staying
 * open lets you compare combinations before closing (Esc / outside click /
 * re-press `a`). */
static void
_action_enhance(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL || p_win->p_enhancer == NULL) {
      return;
   }
   if (p_win->p_enhance_pop != NULL) {
      _enhance_destroy(p_win);
      return;
   }
   GtkWidget *p_pop = gtk_popover_new();
   gtk_popover_set_position(GTK_POPOVER(p_pop), GTK_POS_TOP);
   gtk_popover_set_pointing_to(GTK_POPOVER(p_pop),
                               &(const GdkRectangle){0, 0, 1, 1});
   g_signal_connect(GTK_POPOVER(p_pop), "closed",
                    G_CALLBACK(_enhance_closed_cb), p_win);
   GtkEventController *p_kc = gtk_event_controller_key_new();
   gtk_event_controller_set_propagation_phase(p_kc, GTK_PHASE_CAPTURE);
   g_signal_connect(p_kc, "key-pressed", G_CALLBACK(_enhance_key_pressed_cb),
                    p_win);
   gtk_widget_add_controller(p_pop, p_kc);

   const GPtrArray *p_presets = enhancer_get_presets(p_win->p_enhancer);
   gtk_popover_set_child(GTK_POPOVER(p_pop),
                         _enhance_build_box(p_win, p_presets));
   gtk_widget_set_parent(p_pop, p_win->p_stack);
   p_win->p_enhance_pop = p_pop;
   gtk_popover_popup(GTK_POPOVER(p_pop));
}

/* win.enhance-N (keys 1-8, always live -- not gated on the popover being
 * open): toggle preset N on/off (layered), then re-apply asynchronously. */
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
   _enhance_apply_async(p_win);
}

/* win.enhance-save (key 's'): export the current image with the enabled-preset
 * chain to a non-colliding <stem>-enhanced[-<n>].<ext>. Never overwrites the
 * original or an existing enhanced copy. No-op (with a status line) when no
 * preset is enabled. */
static void
_action_enhance_save(GSimpleAction *p_a, GVariant *p_v, gpointer p_data) {
   (void)p_a;
   (void)p_v;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   if (p_win->p_nav == NULL || p_win->p_enhancer == NULL) {
      return;
   }
   if (p_win->u_enhance_mask == 0) {
      _show_status(p_win, "Nothing to save (no enhance preset enabled)");
      return;
   }
   _enhance_do_save(p_win);
}

/* Hold-Space compare (see window.h ggaze_window_set_hold_original): TRUE
 * shows the cached original from texturecache (cheap, no GEGL recompute);
 * FALSE restores the cached modified texture. No-op if nothing is dirty, or
 * the requested state is already in effect (GTK's key-repeat re-fires
 * key-pressed while a key is held down without an intervening release). */
void
ggaze_window_set_hold_original(GgazeWindow *p_win, gboolean b_hold) {
   g_return_if_fail(GGAZE_IS_WINDOW(p_win));
   if (p_win->p_nav == NULL || p_win->p_enhancer == NULL ||
       p_win->u_enhance_mask == 0 || p_win->b_hold_original == b_hold) {
      return;
   }
   p_win->b_hold_original = b_hold;
   if (b_hold) {
      GFile      *p_cur = navigator_get_current(p_win->p_nav);
      GdkTexture *p_orig =
         p_cur != NULL ? texturecache_get(p_win->p_cache, p_cur) : NULL;
      /* p_orig should always be cached: it was shown before any preset was
       * toggled on, and the LRU (cap 4) comfortably outlives an idle
       * hold-Space session on the same image. If it was ever evicted, this
       * is a silent no-op rather than a synchronous re-decode on the main
       * thread. */
      if (p_orig != NULL) {
         _show_texture(p_win, p_orig);
      }
   } else if (p_win->p_enhance_tex != NULL) {
      _show_texture(p_win, p_win->p_enhance_tex);
   }
}

gboolean
ggaze_window_enhance_is_dirty(GgazeWindow *p_win) {
   g_return_val_if_fail(GGAZE_IS_WINDOW(p_win), FALSE);
   return (p_win->p_enhancer != NULL && p_win->u_enhance_mask != 0);
}

/* Space key controller (window-level, see ggaze_window_init): press shows
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
      if (p_win->u_enhance_mask != 0 && p_win->p_enhancer != NULL) {
         _enhance_discard(p_win);
      }
#endif
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

/* Hide the info overlay in response to the current file changing. Reached
 * from two call sites: (1) nav_changed_cb, the choke point that prev/next/
 * first/last, slideshow auto-advance, grid selection, trash/delete/move
 * advancing past a target, and rescan-after-undo all funnel through via
 * Navigator's "changed" signal; and (2) ggaze_window_open(), called directly
 * (not via that signal) because opening a new folder/file into an existing
 * window doesn't reliably emit "changed" -- see the comment on
 * _open_build_navigator() for why. The overlay must never keep showing a
 * PREVIOUS file's EXIF/dimensions after a new one is displayed, so this
 * cancels any pending auto-hide timer (there is nothing left for it to hide)
 * and hides the label unconditionally; a subsequent _show_status call later
 * in the same handler chain (e.g. move/undo's status line) re-shows it with
 * its own fresh timer, so this never fights a legitimate immediate re-show.
 */
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
      _maybe_save_then(p_win, _proceed_next, p_win, NULL);
   } else {
      _maybe_save_then(p_win, _proceed_prev, p_win, NULL);
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

/* The marks-or-current target set a move acts on is the same one `D` uses --
 * see _capture_targets, which is where that rule (and why it must be resolved
 * at key-press time) now lives for both. */

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

/* Move the target set p_files (borrowed; the caller frees) to destination
 * u_idx. Split out of ggaze_window_move_index so the `m` popup can CAPTURE its
 * targets when the row is clicked and still move exactly those once the
 * Save/Discard/Cancel prompt resolves -- re-deriving them at answer time would
 * move whatever the slideshow/GFileMonitor made current in the meantime
 * (round 3, finding h; the same discipline as _FileCtx and _DeleteCtx). */
static gboolean
_move_captured(GgazeWindow *p_win, guint u_idx, GList *p_files) {
   if (p_win->p_nav == NULL || p_win->p_mover == NULL || p_files == NULL) {
      return (FALSE);
   }
   const GPtrArray *p_dests = mover_get_dests(p_win->p_mover);
   if (p_dests == NULL || u_idx >= p_dests->len) {
      return (FALSE);
   }
   const MoverDest *p_dest = g_ptr_array_index((GPtrArray *)p_dests, u_idx);
   guint            u_n    = g_list_length(p_files);
   /* Asked BEFORE mover_move/_move_mark_removed run: navigator_mark_removed
    * emits "changed" and can move current (and invalidate the borrowed
    * pointer) as a side effect, so afterwards the answer is no longer the
    * one the user's key press was about. */
   gboolean b_was_current = _files_include_current(p_win, p_files);
   GError  *p_err         = NULL;
   gboolean b_ok          = mover_move(p_win->p_mover, p_files, p_dest, &p_err);
   guint    u_moved       = _move_mark_removed(p_win, p_files);
   if (u_moved > 0) {
      p_win->e_last_destructive = GGAZE_LAST_MOVE;
   }
   /* Advance only when one of the moved files really was the current one,
    * exactly as _do_trash_now and _do_delete_files do: the targets are
    * captured at click time, so by the time a Save/Discard/Cancel prompt is
    * answered current may have moved on by itself, and advancing anyway
    * would take the user off the image they were looking at and skip the
    * next one unseen (round 5, finding x -- the last member of the trio
    * still carrying the bug the other two already fixed). */
   if (u_moved > 0 && b_was_current) {
      navigator_next(p_win->p_nav); /* advance past the moved set */
   }
   _move_report(p_win, p_dest, b_ok, u_moved, u_n, p_err);
   g_clear_error(&p_err);
   return (b_ok);
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
   _move_destroy(p_win);
   _MoveIdxCtx *p_ctx = g_new(_MoveIdxCtx, 1);
   _files_ctx_init(&p_ctx->t_base, p_win, _capture_targets(p_win));
   p_ctx->u_idx = u_idx;
   _maybe_save_then(p_win, _proceed_move_idx, p_ctx, _move_idx_ctx_free);
}

/* Row click (mouse): move to that destination, then close the popover. */
static void
_move_row_clicked_cb(GtkButton *p_btn, gpointer p_data) {
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
   guint u_idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(p_btn), "idx"));
   _move_go(p_win, u_idx);
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
   _move_go(p_win, (guint)i_idx);
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
   GList *p_targets = _capture_targets(p_win);
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

#if GGAZE_HAVE_GEGL
/* "changed" fires for every navigator rescan, not only an actual move to a
 * different current file -- notably, win.enhance-save writes the
 * "-enhanced" copy into the SAME live-monitored folder, whose GFileMonitor
 * then schedules a debounced rescan that re-emits "changed" a few hundred ms
 * later even though navigator.current never moved. Discovered by
 * tests/test_enhance_flow.c's save-twice-in-a-row subtest: without this
 * check, that incidental rescan silently zeroed u_enhance_mask right after a
 * successful save, discarding the still-active preview the user was not
 * done comparing/adjusting. Only reset when the current file's IDENTITY
 * actually changed; p_enhance_file is updated unconditionally so the next
 * call has an accurate baseline. */
static void
_enhance_nav_changed(GgazeWindow *p_win) {
   GFile *p_cur =
      p_win->p_nav != NULL ? navigator_get_current(p_win->p_nav) : NULL;
   gboolean b_same = (p_cur != NULL && p_win->p_enhance_file != NULL &&
                      g_file_equal(p_cur, p_win->p_enhance_file));
   if (!b_same) {
      p_win->u_enhance_mask  = 0;
      p_win->b_hold_original = FALSE; /* mask cleared without going through
                                       * _enhance_apply_async, so reset the
                                       * hold flag here too (issue 4) */
      p_win->u_enhance_gen++;
      g_cancellable_cancel(p_win->p_enhance_cancel);
      g_clear_object(&p_win->p_enhance_tex);
      _enhance_update_highlights(p_win);
   }
   g_set_object(&p_win->p_enhance_file, p_cur);
}
#endif

static void
nav_changed_cb(Navigator *p_nav, gpointer p_data) {
   (void)p_nav;
   GgazeWindow *p_win = GGAZE_WINDOW(p_data);
#if GGAZE_HAVE_GEGL
   _enhance_nav_changed(p_win);
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
   gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack), "large");
   _load_current(p_win);
}

/* --- load current into the viewer ---------------------------------------- */

#if GGAZE_HAVE_GEGL
/* Which texture must actually be on screen, given p_tex as the "natural"
 * candidate the caller computed. An active, unanswered enhance preview wins
 * over any original-image texture: while the mask is set, the preview IS what
 * `s` would export, so showing the plain original underneath it would lie
 * about the window's state.
 *
 * hold-Space deliberately bypasses this (b_hold_original) -- that flag is
 * exactly the user asking to see the original -- and so does an empty
 * mask/absent preview texture, which is the overwhelmingly common case. */
static GdkTexture *
_preview_override(GgazeWindow *p_win, GdkTexture *p_tex) {
   if (p_win->u_enhance_mask == 0 || p_win->p_enhance_tex == NULL ||
       p_win->b_hold_original) {
      return (p_tex);
   }
   return (p_win->p_enhance_tex);
}
#endif

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
    * Point-patching _on_grid_activate was not enough: _load_current only
    * paints synchronously on a texturecache HIT, so on a miss the async
    * _load_finish_cb landed after the restore and the plain original won the
    * race; and the view toggle (`t`, `t`) never had the patch at all. */
#if GGAZE_HAVE_GEGL
   p_tex = _preview_override(p_win, p_tex);
#endif
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
      /* Through _show_texture, not straight to the viewer, so a partial of
       * the original cannot flash over an active enhance preview either. */
      _show_texture(p_pi->p_win, p_pi->p_tex);
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

#if GGAZE_HAVE_GEGL
/* Tear down the Save/Discard/Cancel prompt machinery: the request parked
 * behind the prompt, and the prompt itself.
 *
 * Order matters. The parked request goes first, and its slot is cleared BEFORE
 * it is released, because a parked _OpenCtx drops an owned window ref from
 * inside this very dispose. The prompt is cancelled last, so that whichever
 * way the dialog's callback is delivered -- and GTask completes it in an idle,
 * so in practice after dispose has returned -- it finds an empty queue and
 * simply releases itself.
 *
 * Cancelling is what makes that callback happen at all. Nothing else can
 * finish the dialog's GTask (see _save_prompt_show), so before this the
 * _SaveCtx and every ctx it carries were simply abandoned. The cancel resolves
 * the prompt as "do not proceed", which is the only honest answer here: by the
 * time dispose runs, the preview, the enhancer and the navigator a Save or a
 * continuation would need are already gone.
 *
 * The cancel CAN override an answer the user has already given, and that was
 * measured, not assumed: clicking Discard and disposing in the same main-loop
 * turn -- before GTask's completion idle runs -- came back as CANCELLED with
 * no button index, exactly as tu0's round-3 probe reported. Drain that one
 * idle first and the same click comes back as the button it was. This cancel
 * is safe from that only because dispose is refcount-driven: the _SaveCtx's
 * own window ref keeps the refcount off zero for exactly as long as the prompt
 * is outstanding, and only that callback ever releases it, so an ordinary
 * unref cannot get here ahead of an answer. Only a forced
 * g_object_run_dispose() reaches this with a live dialog.
 *
 * Which is the honest scope of this cancel: nothing in src/ forces a dispose,
 * so the only caller that reaches it with a prompt up is the subtest in
 * tests/test_enhance_flow.c. It is not the fix for a shutdown under a prompt
 * -- gtk_application_shutdown() (gtk 4.22.4 gtkapplication.c:348) neither
 * destroys nor disposes windows, and a process that simply exits runs no
 * dispose at all. The reachable hazard, a native close arriving while the
 * prompt is up, is refused in _on_close_request instead. This is what makes a
 * forced dispose SAFE (and what an embedder or a future teardown path can
 * rely on), not what keeps the app from leaking on the way out. */
static void
_prompt_dispose(GgazeWindow *p_win) {
   if (p_win->p_pending != NULL) {
      _Request *p_req  = p_win->p_pending;
      p_win->p_pending = NULL;
      _drop_pending(p_win, p_req, TRUE);
   }
   p_win->b_prompt_quits = FALSE;
   g_cancellable_cancel(p_win->p_prompt_cancel);
   g_clear_object(&p_win->p_prompt_cancel);
}

/* Tear down every piece of enhance state the window owns, so
 * ggaze_window_dispose itself stays a flat list of one-liners (and under the
 * 50-line convention). The enhancer engine itself is released later in
 * dispose, after the widgets, because _enhance_destroy only unparents the
 * popover. */
static void
_enhance_dispose(GgazeWindow *p_win) {
   _enhance_destroy(p_win);
   g_cancellable_cancel(p_win->p_enhance_cancel);
   g_clear_object(&p_win->p_enhance_cancel);
   g_clear_object(&p_win->p_enhance_tex);
   g_clear_object(&p_win->p_enhance_file);
   _prompt_dispose(p_win);
}
#endif

/* Cancel the `D` delete-confirm dialog, for the same reason _prompt_dispose
 * cancels the Save prompt: nothing but the dialog itself can finish its GTask,
 * so without this the _DeleteCtx -- and the deep copy of the captured target
 * list it carries -- was simply abandoned when the window went away.
 *
 * The cancel resolves the dialog as "not confirmed", which is the only honest
 * answer here and the safe one twice over: _delete_confirm_answered_yes sees
 * the cancel's error and refuses, and dispose has already cleared p_nav, so
 * ggaze_window_delete_captured would refuse as well.
 *
 * Not under GGAZE_HAVE_GEGL, unlike _prompt_dispose: this dialog exists in
 * every build. Cancelling a NULL cancellable is a no-op, so no guard is
 * needed for the (usual) case where no confirm is up. */
static void
_delete_confirm_dispose(GgazeWindow *p_win) {
   g_cancellable_cancel(p_win->p_delete_cancel);
   g_clear_object(&p_win->p_delete_cancel);
}

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
   _delete_confirm_dispose(p_win);
#if GGAZE_HAVE_GEGL
   _enhance_dispose(p_win);
#endif
   if (p_win->u_slideshow != 0) {
      g_source_remove(p_win->u_slideshow);
      p_win->u_slideshow = 0;
   }
   _info_cancel_timer(p_win); /* just cancels the pending timer, no widget
                                  touch -- safe to call during dispose */
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

#if GGAZE_HAVE_GEGL
/* Enhance-related field init + the hold-Space compare key controller,
 * extracted out of ggaze_window_init to keep it under CLAUDE.md's 50-line
 * hard limit (tu0 review round 2, issue 5). Hold-Space (docs/ui-and-
 * interactions.md "Compare original vs modified") needs press AND release,
 * which shortcuts.c's GtkShortcutController (action triggers fire on press
 * only) cannot express, so this is a dedicated key controller straight on
 * the window, capture phase like the popovers' own controllers. Space is
 * not bound to anything else, so ordering relative to shortcuts_install's
 * GLOBAL-scope controller does not matter here. */
static void
_init_enhance_state(GgazeWindow *p_win) {
   p_win->u_enhance_mask   = 0; /* start on the original */
   p_win->p_enhancer       = enhancer_new();
   p_win->p_enhance_tex    = NULL;
   p_win->p_enhance_cancel = g_cancellable_new();
   p_win->u_enhance_gen    = 0;
   p_win->b_hold_original  = FALSE;
   p_win->b_save_prompt    = FALSE;
   p_win->p_pending        = NULL;
   p_win->p_enhance_file   = NULL;

   GtkEventController *p_space_kc = gtk_event_controller_key_new();
   gtk_event_controller_set_propagation_phase(p_space_kc, GTK_PHASE_CAPTURE);
   g_signal_connect(p_space_kc, "key-pressed", G_CALLBACK(_space_pressed_cb),
                    p_win);
   g_signal_connect(p_space_kc, "key-released", G_CALLBACK(_space_released_cb),
                    p_win);
   gtk_widget_add_controller(GTK_WIDGET(p_win), p_space_kc);
}
#endif

/* Cancellables, texture/thumbnail caches, per-folder trash/grid placeholders,
 * and the configured engines (mover/opener/runner, plus the GEGL enhancer
 * when built in) fed from GSettings. Split out of ggaze_window_init to keep
 * it under CLAUDE.md's 50-line hard limit (tu0 review round 2, issue 5). */
static void
_init_engines_and_settings(GgazeWindow *p_win) {
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
   _init_enhance_state(p_win); /* must run before _load_engine_lists below,
                                * which feeds settings into p_win->p_enhancer
                                * once it exists */
#endif
   /* Feed the configured a(ss) lists into the engines now that all of them
    * (incl. the GEGL enhancer) exist. */
   _load_engine_lists(p_win);
}

/* Wrap p_win->p_stack (built already) in a GtkOverlay with the auto-hiding
 * info label floating on top, and make it the window's child. Split out of
 * _init_stack_and_viewer to keep it under the ~30-line convention. */
static void
_init_info_overlay(GgazeWindow *p_win) {
   p_win->p_overlay = gtk_overlay_new();
   gtk_overlay_set_child(GTK_OVERLAY(p_win->p_overlay), p_win->p_stack);
   p_win->p_info_lbl = gtk_label_new("");
   gtk_widget_add_css_class(p_win->p_info_lbl, "ggaze-info");
   gtk_widget_set_margin_start(p_win->p_info_lbl, 12);
   gtk_widget_set_margin_top(p_win->p_info_lbl, 12);
   gtk_widget_set_visible(p_win->p_info_lbl, FALSE);
   gtk_overlay_add_overlay(GTK_OVERLAY(p_win->p_overlay), p_win->p_info_lbl);
   gtk_widget_set_hexpand(p_win->p_overlay, TRUE);
   gtk_widget_set_vexpand(p_win->p_overlay, TRUE);
   gtk_window_set_child(GTK_WINDOW(p_win), p_win->p_overlay);
}

/* Header bar, the grid/large GtkStack (+ its info overlay), and the viewer
 * widget. Split out of ggaze_window_init to keep it under CLAUDE.md's
 * 50-line hard limit (tu0 review round 2, issue 5). */
static void
_init_stack_and_viewer(GgazeWindow *p_win) {
   /* Header bar (libadwaita, decision #29). */
   GtkWidget *p_header = adw_header_bar_new();
   gtk_window_set_titlebar(GTK_WINDOW(p_win), p_header);

   /* Two-view stack: "grid" is created on open; placeholder until then. */
   p_win->p_stack = gtk_stack_new();
   gtk_stack_set_transition_type(GTK_STACK(p_win->p_stack),
                                 GTK_STACK_TRANSITION_TYPE_CROSSFADE);
   _init_info_overlay(p_win);

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
}

static void
ggaze_window_init(GgazeWindow *p_win) {
   _ensure_css();
   _init_engines_and_settings(p_win);
   _init_stack_and_viewer(p_win);

   /* Actions + keybindings (decision #10/#12). */
   g_action_map_add_action_entries(G_ACTION_MAP(p_win), ACTIONS,
                                   G_N_ELEMENTS(ACTIONS), p_win);
   shortcuts_install(GTK_WIDGET(p_win));

   /* File/folder drag-and-drop (decision #27). */
   GtkDropTarget *p_drop =
      gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
   g_signal_connect(p_drop, "drop", G_CALLBACK(drop_cb), p_win);
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
   g_clear_pointer(&p_win->p_trash, trash_delete);
   mover_clear_last(p_win->p_mover);
   p_win->e_last_destructive = GGAZE_LAST_NONE;
   g_signal_connect(p_win->p_nav, "changed", G_CALLBACK(nav_changed_cb), p_win);
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
   p_win->p_grid   = GGAZE_GRID(ggaze_grid_new(
      p_win->p_nav, p_win->p_thumb, p_win->i_grid_size, b_hide_trashed));
   g_signal_connect(p_win->p_grid, "activate", G_CALLBACK(_on_grid_activate),
                    p_win);
   /* Route every grid/thumbnail selection through the dirty-preview gate
    * (tu0 review round 2, issue 1) instead of letting gridview.c call
    * navigator_set_current_file() directly. */
   ggaze_grid_set_select_func(p_win->p_grid, _grid_select_gate, p_win);
   gtk_stack_add_named(GTK_STACK(p_win->p_stack), GTK_WIDGET(p_win->p_grid),
                       "grid");
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
   gtk_stack_set_visible_child_name(GTK_STACK(p_win->p_stack),
                                    b_is_dir ? "grid" : "large");
   _load_current(p_win);
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
   _maybe_save_then(p_win, _proceed_open, p_ctx, _open_ctx_free);
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

void
ggaze_window_clear_texture_cache(GgazeWindow *p_win) {
   g_return_if_fail(GGAZE_IS_WINDOW(p_win));
   if (p_win->p_cache != NULL) {
      texturecache_clear(p_win->p_cache);
   }
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
