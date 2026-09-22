/*:*
 * ggaze — Enhance/GEGL UI orchestration controller
 *
 * See enhance-ctrl.h. The preset mask, the in-flight apply/preview/export
 * requests, the cached enhanced texture, the hold-Space flag and the gallery
 * window / compact popover live here; the window reaches it through a few
 * action entry points and it reaches the window through EnhanceUIHostOps.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "enhance-ctrl.h"

#include <glib.h>
#include <gtk/gtk.h>

#include "enhance-ui.h"
#include "enhancer-gegl.h"
#include "pathutil.h"
#include "popup_list.h"

/* Thin wrappers over the host vtable so the body reads like the old
 * window.c code (which called _show_texture / _update_header / ... directly).
 * p_ctrl->p_host is the window, borrowed for the controller's lifetime. */
static void        _show_texture(EnhanceCtrl *p_ctrl, GdkTexture *p_tex);
static void        _update_header(EnhanceCtrl *p_ctrl);
static void        _show_status(EnhanceCtrl *p_ctrl, const char *c_msg);
static void        _load_current(EnhanceCtrl *p_ctrl);
static GFile      *_current_file(EnhanceCtrl *p_ctrl);
static GdkTexture *_cached_texture(EnhanceCtrl *p_ctrl, GFile *p_file);
static gboolean    _disposed(EnhanceCtrl *p_ctrl);
static gboolean    _has_navigator(EnhanceCtrl *p_ctrl);

/* Forward decls of the internal state-machine functions. */
static void _update_highlights(EnhanceCtrl *p_ctrl);
static void _sync_current_card(EnhanceCtrl *p_ctrl);
static void _apply_async(EnhanceCtrl *p_ctrl);
static void _discard(EnhanceCtrl *p_ctrl);
static void _destroy(EnhanceCtrl *p_ctrl);
static void _start_previews(EnhanceCtrl *p_ctrl, const GPtrArray *p_presets);
static void _row_toggle(EnhanceCtrl *p_ctrl, GtkWidget *p_btn);

/* --- struct --------------------------------------------------------------- */

struct EnhanceCtrl {
   const EnhanceUIHostOps *p_ops;  /* borrowed, for the controller's lifetime */
   gpointer                p_host; /* the window, borrowed */

   Enhancer  *p_enhancer;     /* GEGL preset engine (always non-NULL) */
   guint8     u_enhance_mask; /* bit i -> preset i enabled (layered) */
   gboolean   b_disposed;     /* set by enhance_ctrl_dispose */
   gboolean   b_previews;     /* the open UI is the gallery window */
   GtkWidget *p_ui;           /* `a` gallery window or compact popover */
   GtkWidget *p_btns[GGAZE_ENHANCE_MAX_PRESETS]; /* preset rows, for
                                                  * highlighting; NULL'd on
                                                  * close */
   GtkWidget *p_pics[GGAZE_ENHANCE_MAX_PRESETS]; /* optional per-preset
                                                  * preview pictures */
   GtkWidget *p_original_pic;      /* optional Original preview picture */
   GtkWidget *p_current_pic;       /* optional "Current" (layered chain) preview
                                    * picture; shows the SAME texture the viewer
                                    * displays, so it costs no extra GEGL work --
                                    * see _sync_current_card */
   GtkWidget    *p_gallery;        /* responsive preview flow box */
   GtkWidget    *p_scroll;         /* gallery viewport sized to the window */
   GCancellable *p_preview_cancel; /* separate thumbnail-preview request */
   guint         u_preview_gen;    /* invalidates stale gallery completions */
   GdkTexture   *p_enhance_tex;    /* last-applied modified texture, cached
                                    * so hold-Space can restore it without a
                                    * GEGL recompute */
   GCancellable *p_enhance_cancel; /* in-flight enhance-apply GTask */
   GCancellable *p_save_cancel;    /* in-flight export (`s`); cancelled on
                                    * dispose so a closing window never gets
                                    * a late completion */
   guint u_enhance_gen;            /* bumped on every apply/discard; a
                                    * completion whose request predates the
                                    * current value is stale and dropped
                                    * (last-write-wins -- GEGL processing
                                    * cannot be aborted mid-flight once
                                    * started) */
   gboolean b_hold_original;       /* TRUE while Space is held (hold-compare) */
   GFile   *p_enhance_file;        /* file the current mask/preview applies to
                                    * (NULL = none); lets nav_changed tell an
                                    * actual navigation apart from a same-file
                                    * rescan (e.g. the folder's GFileMonitor
                                    * noticing the "-enhanced" copy enhance-save
                                    * just wrote next to the original) so a save
                                    * doesn't silently discard its own
                                    * still-active preview */
};

/* --- host-op wrappers ---------------------------------------------------- */

static void
_show_texture(EnhanceCtrl *p_ctrl, GdkTexture *p_tex) {
   p_ctrl->p_ops->show_texture(p_ctrl->p_host, p_tex);
}

static void
_update_header(EnhanceCtrl *p_ctrl) {
   p_ctrl->p_ops->update_header(p_ctrl->p_host);
}

static void
_show_status(EnhanceCtrl *p_ctrl, const char *c_msg) {
   p_ctrl->p_ops->show_status(p_ctrl->p_host, c_msg);
}

static void
_load_current(EnhanceCtrl *p_ctrl) {
   p_ctrl->p_ops->load_current(p_ctrl->p_host);
}

static GFile *
_current_file(EnhanceCtrl *p_ctrl) {
   return (p_ctrl->p_ops->get_current_file(p_ctrl->p_host));
}

static GdkTexture *
_cached_texture(EnhanceCtrl *p_ctrl, GFile *p_file) {
   return (p_ctrl->p_ops->get_cached_texture(p_ctrl->p_host, p_file));
}

static gboolean
_disposed(EnhanceCtrl *p_ctrl) {
   return (p_ctrl->b_disposed);
}

static gboolean
_has_navigator(EnhanceCtrl *p_ctrl) {
   return (p_ctrl->p_ops->has_navigator(p_ctrl->p_host));
}

/* --- struct + lifecycle --------------------------------------------------- */

EnhanceCtrl *
enhance_ctrl_new(const EnhanceUIHostOps *p_ops, gpointer p_host) {
   g_return_val_if_fail(p_ops != NULL, NULL);
   EnhanceCtrl *p_ctrl      = g_new0(EnhanceCtrl, 1);
   p_ctrl->p_ops            = p_ops;
   p_ctrl->p_host           = p_host;
   p_ctrl->p_enhancer       = enhancer_new();
   p_ctrl->p_enhance_cancel = g_cancellable_new();
   return (p_ctrl);
}

void
enhance_ctrl_delete(EnhanceCtrl *p_ctrl) {
   if (p_ctrl == NULL) {
      return;
   }
   /* dispose should already have run; clear any leftover defensively. */
   g_clear_object(&p_ctrl->p_enhance_cancel);
   g_clear_object(&p_ctrl->p_save_cancel);
   g_clear_object(&p_ctrl->p_preview_cancel);
   g_clear_object(&p_ctrl->p_enhance_tex);
   g_clear_object(&p_ctrl->p_enhance_file);
   g_clear_pointer(&p_ctrl->p_enhancer, enhancer_delete);
   g_free(p_ctrl);
}

void
enhance_ctrl_dispose(EnhanceCtrl *p_ctrl) {
   if (p_ctrl == NULL) {
      return;
   }
   p_ctrl->b_disposed = TRUE;
   _destroy(p_ctrl);
   g_cancellable_cancel(p_ctrl->p_enhance_cancel);
   g_clear_object(&p_ctrl->p_enhance_cancel);
   g_cancellable_cancel(p_ctrl->p_save_cancel);
   g_clear_object(&p_ctrl->p_save_cancel);
   g_clear_object(&p_ctrl->p_enhance_tex);
   g_clear_object(&p_ctrl->p_enhance_file);
   /* The enhancer engine is released here (in dispose, after the widgets),
    * mirroring the old window.c order: _destroy closes the UI, then the
    * engine goes. In-flight apply/preview asyncs were cancelled above, so
    * their completion callbacks see _disposed() TRUE and early-return
    * before touching the engine. */
   g_clear_pointer(&p_ctrl->p_enhancer, enhancer_delete);
}

/* --- engine presets ------------------------------------------------------ */

void
enhance_ctrl_set_user_presets(EnhanceCtrl *p_ctrl, const GPtrArray *p_pairs) {
   g_return_if_fail(p_ctrl != NULL);
   if (p_ctrl->p_enhancer != NULL) {
      enhancer_set_user_presets(p_ctrl->p_enhancer, p_pairs);
   }
}

const GPtrArray *
enhance_ctrl_get_presets(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, NULL);
   return (p_ctrl->p_enhancer != NULL ? enhancer_get_presets(p_ctrl->p_enhancer)
                                      : NULL);
}

guint8
enhance_ctrl_get_mask(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, 0);
   return (p_ctrl->u_enhance_mask);
}

/* --- state queries ------------------------------------------------------- */

gboolean
enhance_ctrl_is_dirty(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   return (p_ctrl->p_enhancer != NULL && p_ctrl->u_enhance_mask != 0);
}

GdkTexture *
enhance_ctrl_override_texture(EnhanceCtrl *p_ctrl, GdkTexture *p_tex) {
   if (p_ctrl == NULL || p_ctrl->u_enhance_mask == 0 ||
       p_ctrl->p_enhance_tex == NULL || p_ctrl->b_hold_original) {
      return (p_tex);
   }
   return (p_ctrl->p_enhance_tex);
}

void
enhance_ctrl_set_hold_original(EnhanceCtrl *p_ctrl, gboolean b_hold) {
   g_return_if_fail(p_ctrl != NULL);
   if (!_has_navigator(p_ctrl) || p_ctrl->p_enhancer == NULL ||
       p_ctrl->u_enhance_mask == 0 || p_ctrl->b_hold_original == b_hold) {
      return;
   }
   p_ctrl->b_hold_original = b_hold;
   if (b_hold) {
      GFile      *p_cur = _current_file(p_ctrl);
      GdkTexture *p_orig =
         p_cur != NULL ? _cached_texture(p_ctrl, p_cur) : NULL;
      /* p_orig should always be cached: it was shown before any preset was
       * toggled on, and the LRU (cap 4) comfortably outlives an idle
       * hold-Space session on the same image. If it was ever evicted, this
       * is a silent no-op rather than a synchronous re-decode on the main
       * thread. */
      if (p_orig != NULL) {
         _show_texture(p_ctrl, p_orig);
      }
   } else if (p_ctrl->p_enhance_tex != NULL) {
      _show_texture(p_ctrl, p_ctrl->p_enhance_tex);
   }
}

/* --- save / export ------------------------------------------------------- */

/* Report the outcome of an export via _show_status (+ g_warning on failure),
 * mirroring mover.c's success/failure split. */
static void
_save_report(EnhanceCtrl *p_ctrl, GFile *p_out, gboolean b_ok,
             const GError *p_err) {
   char *c_saved = g_file_get_basename(p_out);
   char *c_msg   = b_ok ? g_strdup_printf("Saved %s", c_saved)
                        : g_strdup_printf("Enhance-save failed: %s",
                                          p_err != NULL ? p_err->message : "?");
   if (!b_ok) {
      g_warning("ggaze: %s", c_msg);
   }
   _show_status(p_ctrl, c_msg);
   g_free(c_msg);
   g_free(c_saved);
}

/* TRUE iff there is actually an enhance preview to export right now. Split
 * out so the Save/Discard/Cancel gate can tell "nothing to save" apart from
 * "the export failed": treating the first as a failure made the Save button
 * silently do nothing AND cancel the user's action.
 *
 * The subject is p_enhance_file -- the file the mask/preview was computed FOR
 * (_launch sets it) -- not a fresh _current_file(). Save runs from a dialog
 * callback, so it is one of the deferred paths that must not re-derive its
 * target: nav_changed does clear the mask whenever current's identity
 * changes, which makes the two equal today, but that is an invariant holding
 * a permanent write together rather than a reason to depend on it. */
gboolean
enhance_ctrl_can_save(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   return (_has_navigator(p_ctrl) && p_ctrl->p_enhancer != NULL &&
           p_ctrl->u_enhance_mask != 0 && p_ctrl->p_enhance_file != NULL);
}

/* Per-export context: the destination (for the report), the caller's
 * continuation, and a ref on the host so the completion can run after a
 * dispose without dangling (it then only releases). */
typedef struct {
   gpointer          p_host; /* ref'd window */
   EnhanceCtrl      *p_ctrl; /* borrowed, valid while p_host is alive */
   GFile            *p_out;  /* owned */
   EnhanceSaveDoneFn fn_done;
   gpointer          p_done_data;
} _SaveReq;

static void
_save_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   _SaveReq    *p_req  = (_SaveReq *)p_data;
   EnhanceCtrl *p_ctrl = p_req->p_ctrl;
   GError      *p_err  = NULL;
   gboolean     b_ok   = enhancer_export_chain_finish(p_res, &p_err);
   if (!_disposed(p_ctrl)) {
      _save_report(p_ctrl, p_req->p_out, b_ok, p_err);
   }
   if (p_req->fn_done != NULL) {
      p_req->fn_done(b_ok, p_req->p_done_data);
   }
   g_clear_error(&p_err);
   g_object_unref(p_req->p_out);
   g_object_unref(p_req->p_host);
   g_free(p_req);
}

/* Export the previewed image with the enabled-preset chain to a non-colliding
 * "<stem>-enhanced[-<n>].<ext>" in the same folder, in a worker. The original
 * file is never touched: the worker reads it (enhancer_load) and writes only
 * to the freshly computed destination. The subject is p_enhance_file, the
 * file the preview belongs to -- see enhance_ctrl_can_save. */
void
enhance_ctrl_save_async(EnhanceCtrl *p_ctrl, EnhanceSaveDoneFn fn_done,
                        gpointer p_done_data) {
   g_return_if_fail(p_ctrl != NULL);
   if (!enhance_ctrl_can_save(p_ctrl)) {
      _show_status(p_ctrl, "Nothing to save (no enhance preset enabled)");
      if (fn_done != NULL) {
         fn_done(FALSE, p_done_data);
      }
      return;
   }
   GFile *p_out = enhancer_export_dest_for(p_ctrl->p_enhance_file);
   if (p_out == NULL) {
      _show_status(p_ctrl, "Enhance-save failed: no free file name");
      if (fn_done != NULL) {
         fn_done(FALSE, p_done_data);
      }
      return;
   }
   g_cancellable_cancel(p_ctrl->p_save_cancel);
   g_clear_object(&p_ctrl->p_save_cancel);
   p_ctrl->p_save_cancel = g_cancellable_new();
   _SaveReq *p_req       = g_new0(_SaveReq, 1);
   p_req->p_host         = g_object_ref(p_ctrl->p_host);
   p_req->p_ctrl         = p_ctrl;
   p_req->p_out          = p_out;
   p_req->fn_done        = fn_done;
   p_req->p_done_data    = p_done_data;
   char *c_name          = g_file_get_basename(p_out);
   char *c_msg           = g_strdup_printf("Saving %s\u2026", c_name);
   _show_status(p_ctrl, c_msg);
   g_free(c_msg);
   g_free(c_name);
   enhancer_export_chain_async(p_ctrl->p_enhance_file,
                               enhancer_get_presets(p_ctrl->p_enhancer),
                               p_ctrl->u_enhance_mask, p_out,
                               p_ctrl->p_save_cancel, _save_done_cb, p_req);
}

/* --- apply / discard ----------------------------------------------------- */

/* Update each popover preset row's "ggaze-enhance-on" highlight from the
 * mask. A no-op when the popover is closed (rows are NULL'd by _destroy), so
 * callers never need to check p_ui first. */
static void
_update_highlights(EnhanceCtrl *p_ctrl) {
   for (guint i = 0; i < G_N_ELEMENTS(p_ctrl->p_btns); i++) {
      GtkWidget *p_btn = p_ctrl->p_btns[i];
      if (p_btn == NULL) {
         continue;
      }
      if ((p_ctrl->u_enhance_mask & (guint8)(1u << i)) != 0) {
         gtk_widget_add_css_class(p_btn, "ggaze-enhance-on");
      } else {
         gtk_widget_remove_css_class(p_btn, "ggaze-enhance-on");
      }
   }
}

/* Point the "Current" card at whatever the large view is showing: the layered
 * chain result when any preset is on, else the Original preview. See the
 * struct comment on p_current_pic for why this reuses p_enhance_tex (no
 * extra GEGL pass, cannot disagree with the large view). With an empty mask
 * there is no chain result and "current" means the original, so the Original
 * card's paintable is mirrored rather than left blank. */
static void
_sync_current_card(EnhanceCtrl *p_ctrl) {
   if (p_ctrl->p_current_pic == NULL) {
      return;
   }
   if (p_ctrl->p_enhance_tex != NULL) {
      gtk_picture_set_paintable(GTK_PICTURE(p_ctrl->p_current_pic),
                                GDK_PAINTABLE(p_ctrl->p_enhance_tex));
      return;
   }
   GdkPaintable *p_orig =
      p_ctrl->p_original_pic != NULL
         ? gtk_picture_get_paintable(GTK_PICTURE(p_ctrl->p_original_pic))
         : NULL;
   gtk_picture_set_paintable(GTK_PICTURE(p_ctrl->p_current_pic), p_orig);
}

/* Per-request context for _apply_async's async completion: a ref on the host
 * window (so it -- and thus this borrowed controller -- outlives the worker
 * even across a dispose) and the generation the request was launched at, for
 * the last-write-wins check in _apply_done_cb. */
typedef struct {
   gpointer     p_host; /* ref'd window */
   EnhanceCtrl *p_ctrl; /* borrowed, valid while p_host is alive */
   guint        u_gen;
} _Req;

static void
_req_free(_Req *p_req) {
   if (p_req == NULL) {
      return;
   }
   g_object_unref(p_req->p_host);
   g_free(p_req);
}

/* Async apply completion (tu0): last-write-wins via u_enhance_gen -- a newer
 * apply, a discard, or the window closing while this was still processing in
 * its worker thread all bump the generation, so a stale result here is
 * dropped instead of clobbering whatever is now current. */
static void
_apply_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   _Req        *p_req  = (_Req *)p_data;
   EnhanceCtrl *p_ctrl = p_req->p_ctrl;
   GError      *p_err  = NULL;
   GdkTexture  *p_tex  = enhancer_apply_chain_finish(p_res, &p_err);
   if (_disposed(p_ctrl) || p_req->u_gen != p_ctrl->u_enhance_gen) {
      g_clear_object(&p_tex);
      g_clear_error(&p_err);
      _req_free(p_req);
      return;
   }
   if (p_tex == NULL) {
      g_warning("ggaze: enhance failed: %s",
                p_err != NULL ? p_err->message : "(no detail)");
      g_clear_error(&p_err);
      _show_status(p_ctrl, "Enhance failed");
      _discard(p_ctrl); /* back to the original; also bumps the gen */
   } else {
      g_set_object(&p_ctrl->p_enhance_tex, p_tex);
      _show_texture(p_ctrl, p_tex);
      g_object_unref(p_tex);
      _sync_current_card(p_ctrl); /* after the cache, before the header */
      _update_header(p_ctrl);
   }
   _req_free(p_req);
}

/* Switch to large view (if not already there) and start a fresh generation:
 * bump u_enhance_gen and replace p_enhance_cancel, so a still-in-flight older
 * apply's result is recognized as stale and dropped when it eventually
 * completes (last-write-wins; GEGL processing itself cannot be aborted
 * mid-flight once started). */
static void
_apply_begin(EnhanceCtrl *p_ctrl) {
   p_ctrl->p_ops->ensure_large_view(p_ctrl->p_host);
   p_ctrl->u_enhance_gen++;
   g_cancellable_cancel(p_ctrl->p_enhance_cancel);
   g_clear_object(&p_ctrl->p_enhance_cancel);
   p_ctrl->p_enhance_cancel = g_cancellable_new();
}

/* Launch the async apply for the non-empty mask case: record which file the
 * preview applies to (see nav_changed's comment for why this is set here, not
 * only in nav_changed) and hand off to enhancer_apply_chain_async. */
static void
_launch(EnhanceCtrl *p_ctrl, GFile *p_file) {
   g_set_object(&p_ctrl->p_enhance_file, p_file);
   _Req *p_req                = g_new(_Req, 1);
   p_req->p_host              = g_object_ref(p_ctrl->p_host);
   p_req->p_ctrl              = p_ctrl;
   p_req->u_gen               = p_ctrl->u_enhance_gen;
   const GPtrArray *p_presets = enhancer_get_presets(p_ctrl->p_enhancer);
   enhancer_apply_chain_async(p_file, p_presets, p_ctrl->u_enhance_mask,
                              p_ctrl->p_enhance_cancel, _apply_done_cb, p_req);
}

/* Apply the enabled-preset chain (u_enhance_mask) to the current image as a
 * live preview, off the GTK main thread (enhancer_apply_chain_async: GEGL
 * processing is CPU-heavy, AGENTS.md "Decode runs in GTask threads"). An empty
 * mask restores the original synchronously (texturecache is cheap, no GEGL
 * involved). */
static void
_apply_async(EnhanceCtrl *p_ctrl) {
   if (!_has_navigator(p_ctrl) || p_ctrl->p_enhancer == NULL) {
      return;
   }
   _apply_begin(p_ctrl);
   if (p_ctrl->u_enhance_mask == 0) {
      /* Canonical "mask went empty" site -- every path that clears it
       * (Esc/discard, the popover's "0 Original" row, and the easy-to-miss
       * one: toggling the LAST enabled preset back off via win.enhance-N or
       * a popover row) funnels through here. Force the hold-compare flag
       * off: set_hold_original no-ops once nothing is dirty, so a Space
       * RELEASE arriving after the mask was cleared out from under a
       * still-held key would otherwise leave the flag stuck TRUE and swallow
       * the next press (tu0 review round 2, issue 4). */
      p_ctrl->b_hold_original = FALSE;
      g_clear_object(&p_ctrl->p_enhance_tex);
      _load_current(p_ctrl); /* restore original (texturecache is fast) */
      /* Mask empty => "current" is the original again. Must run AFTER the
       * clear above, so the card falls back to the Original card's paintable
       * instead of redisplaying the stale chain result. */
      _sync_current_card(p_ctrl);
      _update_header(p_ctrl);
      return;
   }
   GFile *p_file = _current_file(p_ctrl);
   if (p_file == NULL) {
      p_ctrl->u_enhance_mask  = 0;
      p_ctrl->b_hold_original = FALSE; /* see the mask==0 branch above */
      _update_highlights(p_ctrl);
      _update_header(p_ctrl);
      return;
   }
   _launch(p_ctrl, p_file);
}

/* Drop the current enhance preview and go back to showing the unmodified
 * original: clears the mask + cached texture and reloads the original
 * (_apply_async's mask==0 path also invalidates any in-flight apply via
 * u_enhance_gen). Used by Esc (explicit discard, no prompt), the popover's
 * "0 Original" row/hotkey, the slideshow timer, and after Save/Discard in the
 * navigate-away prompt. Never touches the file on disk -- discarding a
 * preview only drops in-memory state. */
static void
_discard(EnhanceCtrl *p_ctrl) {
   p_ctrl->u_enhance_mask  = 0;
   p_ctrl->b_hold_original = FALSE; /* belt-and-braces: _apply_async's
                                     * mask==0 branch below also does this,
                                     * but it early-returns without a
                                     * navigator/enhancer (issue 4) */
   _update_highlights(p_ctrl);
   _apply_async(p_ctrl);
}

void
enhance_ctrl_discard(EnhanceCtrl *p_ctrl) {
   g_return_if_fail(p_ctrl != NULL);
   _discard(p_ctrl);
}

/* --- UI build / teardown -------------------------------------------------- */

/* Synchronously tear down the current enhance UI and clear its now-dangling
 * row pointers. Safe to call when none is open. Closing it never touches
 * u_enhance_mask -- the preview persists until explicitly discarded. */
static void
_destroy(EnhanceCtrl *p_ctrl) {
   p_ctrl->u_preview_gen++;
   g_cancellable_cancel(p_ctrl->p_preview_cancel);
   g_clear_object(&p_ctrl->p_preview_cancel);
   if (p_ctrl->p_ui == NULL) {
      return;
   }
   GtkWidget *p_ui = p_ctrl->p_ui;
   p_ctrl->p_ui    = NULL;
   for (guint i = 0; i < G_N_ELEMENTS(p_ctrl->p_btns); i++) {
      p_ctrl->p_btns[i] = NULL;
      p_ctrl->p_pics[i] = NULL;
   }
   p_ctrl->p_original_pic = NULL;
   p_ctrl->p_current_pic  = NULL;
   p_ctrl->p_gallery      = NULL;
   p_ctrl->p_scroll       = NULL;
   if (GTK_IS_WINDOW(p_ui)) {
      gtk_window_destroy(GTK_WINDOW(p_ui));
   } else {
      gtk_widget_unparent(p_ui);
   }
}

static void
_closed_cb(GtkPopover *p_pop, gpointer p_data) {
   (void)p_pop;
   _destroy((EnhanceCtrl *)p_data);
}

static gboolean
_window_close_cb(GtkWindow *p_window, gpointer p_data) {
   (void)p_window;
   _destroy((EnhanceCtrl *)p_data);
   return (TRUE);
}

/* Enhance-UI key controller: Esc closes the gallery/popover only (the preview,
 * if any, stays); '0' discards the whole preview outright; a bound digit /
 * letter toggles that preset without closing the UI. Space is hold-compare
 * (press shows the original, release restores). Everything else: the keys
 * the window binds (`s` save, `a` close, h/l, q, ...) propagate to it -- the
 * compact popover shares the window's root, and the gallery window has the
 * window's shortcut table bound onto it (bind_shortcuts) -- but any OTHER
 * unmodified key stops here, so a stray letter cannot trash or quit under
 * the chooser. Lock modifiers (Caps Lock) are ignored. */
static gboolean
_key_pressed_cb(GtkEventControllerKey *p_c, guint u_keyval, guint u_kc,
                GdkModifierType e_state, gpointer p_data) {
   (void)p_c;
   (void)u_kc;
   EnhanceCtrl *p_ctrl = (EnhanceCtrl *)p_data;
   if (u_keyval == GDK_KEY_Escape) {
      _destroy(p_ctrl);
      return (GDK_EVENT_STOP);
   }
   if ((e_state & gtk_accelerator_get_default_mod_mask() & ~GDK_SHIFT_MASK) !=
       0) {
      return (GDK_EVENT_PROPAGATE); /* chords belong to the window */
   }
   guint u_low = gdk_keyval_to_lower(u_keyval);
   if (u_low == GDK_KEY_space) {
      enhance_ctrl_set_hold_original(p_ctrl, TRUE);
      return (GDK_EVENT_STOP);
   }
   if (u_low == GDK_KEY_0) {
      _discard(p_ctrl);
      return (GDK_EVENT_STOP);
   }
   gint i_idx = popup_list_key_to_index(u_low);
   if (i_idx >= 0 && i_idx < (gint)G_N_ELEMENTS(p_ctrl->p_btns)) {
      p_ctrl->u_enhance_mask ^= (guint8)(1u << i_idx);
      _update_highlights(p_ctrl);
      _apply_async(p_ctrl);
      return (GDK_EVENT_STOP);
   }
   /* Window-owned keys that make sense with the chooser open. */
   static const guint PASS[] = {
      GDK_KEY_s,         GDK_KEY_a,        GDK_KEY_h,     GDK_KEY_l,
      GDK_KEY_q,         GDK_KEY_Left,     GDK_KEY_Right, GDK_KEY_Page_Up,
      GDK_KEY_Page_Down, GDK_KEY_question, GDK_KEY_F1};
   for (gsize u = 0; u < G_N_ELEMENTS(PASS); u++) {
      if (u_low == PASS[u]) {
         return (GDK_EVENT_PROPAGATE);
      }
   }
   return (GDK_EVENT_STOP);
}

static gboolean
_key_released_cb(GtkEventControllerKey *p_c, guint u_keyval, guint u_kc,
                 GdkModifierType e_state, gpointer p_data) {
   (void)p_c;
   (void)u_kc;
   (void)e_state;
   if (u_keyval == GDK_KEY_space) {
      enhance_ctrl_set_hold_original((EnhanceCtrl *)p_data, FALSE);
   }
   return (GDK_EVENT_PROPAGATE);
}

typedef struct {
   gpointer     p_host; /* ref'd window */
   EnhanceCtrl *p_ctrl; /* borrowed, valid while p_host is alive */
   guint        u_gen;
} _PreviewCtx;

static void
_preview_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   _PreviewCtx *p_ctx  = (_PreviewCtx *)p_data;
   GError      *p_err  = NULL;
   GPtrArray   *p_tex  = enhancer_preview_thumbnails_finish(p_res, &p_err);
   EnhanceCtrl *p_ctrl = p_ctx->p_ctrl;
   if (!_disposed(p_ctrl) && p_ctx->u_gen == p_ctrl->u_preview_gen &&
       p_ctrl->p_ui != NULL && p_tex != NULL && p_tex->len > 0) {
      GdkTexture *p_original = g_ptr_array_index(p_tex, 0);
      if (p_ctrl->p_original_pic != NULL && p_original != NULL) {
         gtk_picture_set_paintable(GTK_PICTURE(p_ctrl->p_original_pic),
                                   GDK_PAINTABLE(p_original));
      }
      for (guint i = 0; i + 1 < p_tex->len && i < 8; i++) {
         GdkTexture *p_one = g_ptr_array_index(p_tex, i + 1);
         if (p_ctrl->p_pics[i] != NULL && p_one != NULL) {
            gtk_picture_set_paintable(GTK_PICTURE(p_ctrl->p_pics[i]),
                                      GDK_PAINTABLE(p_one));
         }
      }
      /* The Original card just gained its paintable, which is what the
       * Current card mirrors while the mask is empty. */
      _sync_current_card(p_ctrl);
   }
   g_clear_pointer(&p_tex, g_ptr_array_unref);
   g_clear_error(&p_err);
   g_object_unref(p_ctx->p_host);
   g_free(p_ctx);
}

static void
_start_previews(EnhanceCtrl *p_ctrl, const GPtrArray *p_presets) {
   if (!p_ctrl->b_previews) {
      return;
   }
   GFile *p_file = _current_file(p_ctrl);
   if (p_file == NULL) {
      return;
   }
   p_ctrl->u_preview_gen++;
   g_cancellable_cancel(p_ctrl->p_preview_cancel);
   g_clear_object(&p_ctrl->p_preview_cancel);
   p_ctrl->p_preview_cancel = g_cancellable_new();
   _PreviewCtx *p_ctx       = g_new(_PreviewCtx, 1);
   p_ctx->p_host            = g_object_ref(p_ctrl->p_host);
   p_ctx->p_ctrl            = p_ctrl;
   p_ctx->u_gen             = p_ctrl->u_preview_gen;
   enhancer_preview_thumbnails_async(
      p_file, p_presets, p_ctrl->p_preview_cancel, _preview_done_cb, p_ctx);
}

/* Build the popover's content box (title + preset rows) and wire it into the
 * controller's state. The pure widget construction lives in enhance-ui.c
 * (enhance_ui_build_content); this thin wrapper owns the controller-side glue
 * -- storing the built widgets into p_ctrl's fields and connecting each row's
 * "clicked" signal to _row_toggle, which owns the mask state. */
static GtkWidget *
_build_box(EnhanceCtrl *p_ctrl, const GPtrArray *p_presets) {
   char  *c_basename = NULL;
   GFile *p_cur      = _current_file(p_ctrl);
   if (p_cur != NULL) {
      c_basename = g_file_get_basename(p_cur);
   }
   gboolean         b_previews = p_ctrl->b_previews;
   EnhanceUIWidgets ui;
   GtkWidget       *p_box = enhance_ui_build_content(
      p_presets, c_basename, p_ctrl->u_enhance_mask, b_previews, &ui);
   g_free(c_basename);
   p_ctrl->p_original_pic = ui.p_original_pic;
   p_ctrl->p_current_pic  = ui.p_current_pic;
   p_ctrl->p_gallery      = ui.p_gallery;
   p_ctrl->p_scroll       = ui.p_scroll;
   for (guint i = 0; i < ui.u_n_presets; i++) {
      p_ctrl->p_btns[i] = ui.p_btns[i];
      p_ctrl->p_pics[i] = ui.p_pics[i];
   }
   if (ui.p_original_btn != NULL) {
      g_signal_connect_swapped(ui.p_original_btn, "clicked",
                               G_CALLBACK(_row_toggle), p_ctrl);
   }
   for (guint i = 0; i < ui.u_n_presets; i++) {
      g_signal_connect_swapped(ui.p_btns[i], "clicked", G_CALLBACK(_row_toggle),
                               p_ctrl);
   }
   return (p_box);
}

static GtkWidget *
_build_gallery_window(EnhanceCtrl *p_ctrl, const GPtrArray *p_presets) {
   GtkWidget *p_window = gtk_window_new();
   GtkWindow *p_parent = p_ctrl->p_ops->transient_parent(p_ctrl->p_host);
   gtk_window_set_title(GTK_WINDOW(p_window), "Enhance previews");
   gtk_window_set_transient_for(GTK_WINDOW(p_window), p_parent);
   gtk_window_set_destroy_with_parent(GTK_WINDOW(p_window), TRUE);
   gtk_window_set_resizable(GTK_WINDOW(p_window), TRUE);
   int i_width  = MAX(500, gtk_widget_get_width(GTK_WIDGET(p_parent)));
   int i_height = MAX(500, gtk_widget_get_height(GTK_WIDGET(p_parent)));
   gtk_window_set_default_size(GTK_WINDOW(p_window), i_width, i_height);
   gtk_window_set_child(GTK_WINDOW(p_window), _build_box(p_ctrl, p_presets));
   /* Choose the column count once, now that the cells exist and the size the
    * gallery will open at is known. The item count cannot change while the
    * gallery is open -- every picture is created up front and only its
    * paintable arrives later -- so nothing has to recompute this. +2 for the
    * Original and Current cards. */
   guint u_presets = p_presets != NULL ? p_presets->len : 0;
   if (u_presets > G_N_ELEMENTS(p_ctrl->p_btns)) {
      u_presets = G_N_ELEMENTS(p_ctrl->p_btns);
   }
   enhance_ui_apply_grid_columns(
      p_ctrl->p_gallery != NULL ? GTK_FLOW_BOX(p_ctrl->p_gallery) : NULL,
      (int)u_presets + 2, i_width, i_height);
   g_signal_connect(p_window, "close-request", G_CALLBACK(_window_close_cb),
                    p_ctrl);
   /* The gallery is its own GtkRoot: without this the window's `s` / `a` /
    * h / l / q shortcuts did nothing while it had focus, although its own
    * hint said "s  Save enhanced copy". */
   p_ctrl->p_ops->bind_shortcuts(p_ctrl->p_host, p_window);
   return (p_window);
}

/* --- public action entry points ------------------------------------------ */

/* `a`: thumbnail mode opens a resizable gallery window; compact mode retains
 * the anchored popover used by the other chooser UIs. A second press closes
 * either form. Row clicks and hotkeys keep it open so several layered presets
 * can be compared. */
void
enhance_ctrl_toggle_open(EnhanceCtrl *p_ctrl, gboolean b_previews) {
   g_return_if_fail(p_ctrl != NULL);
   if (!_has_navigator(p_ctrl) || p_ctrl->p_enhancer == NULL) {
      return;
   }
   if (p_ctrl->p_ui != NULL) {
      _destroy(p_ctrl);
      return;
   }
   p_ctrl->b_previews         = b_previews;
   const GPtrArray *p_presets = enhancer_get_presets(p_ctrl->p_enhancer);
   GtkWidget       *p_ui      = NULL;
   if (b_previews) {
      p_ui = _build_gallery_window(p_ctrl, p_presets);
   } else {
      p_ui = gtk_popover_new();
      gtk_popover_set_position(GTK_POPOVER(p_ui), GTK_POS_TOP);
      gtk_popover_set_pointing_to(GTK_POPOVER(p_ui),
                                  &(const GdkRectangle){0, 0, 1, 1});
      g_signal_connect(GTK_POPOVER(p_ui), "closed", G_CALLBACK(_closed_cb),
                       p_ctrl);
      gtk_popover_set_child(GTK_POPOVER(p_ui), _build_box(p_ctrl, p_presets));
      gtk_widget_set_parent(p_ui,
                            p_ctrl->p_ops->popover_parent(p_ctrl->p_host));
   }
   GtkEventController *p_kc = gtk_event_controller_key_new();
   gtk_event_controller_set_propagation_phase(p_kc, GTK_PHASE_CAPTURE);
   g_signal_connect(p_kc, "key-pressed", G_CALLBACK(_key_pressed_cb), p_ctrl);
   g_signal_connect(p_kc, "key-released", G_CALLBACK(_key_released_cb), p_ctrl);
   gtk_widget_add_controller(p_ui, p_kc);
   p_ctrl->p_ui = p_ui;
   /* No initial sizing pass: the cells expand into whatever the gallery
    * window's layout gives them, so they are correct from the first frame and
    * stay correct through every later resize without anything measuring. */
   /* Opening on an ALREADY-enhanced image: p_enhance_tex exists right now, so
    * fill the Current card immediately rather than leaving it blank until the
    * preview batch lands (that batch only produces the original and the eight
    * single-preset previews -- never the chain). */
   _sync_current_card(p_ctrl);
   _start_previews(p_ctrl, p_presets);
   if (b_previews) {
      gtk_window_present(GTK_WINDOW(p_ui));
   } else {
      /* Focuses the first preset row itself -- see the "POPOVER KEYBOARD
       * FOCUS" comment in window.c. */
      gtk_popover_popup(GTK_POPOVER(p_ui));
   }
}

/* enhance-N (keys 1-8, always live -- not gated on the popover being open):
 * toggle preset N on/off (layered), then re-apply asynchronously. Out-of-range
 * i_idx is a silent no-op. */
void
enhance_ctrl_toggle_preset(EnhanceCtrl *p_ctrl, gint i_idx) {
   g_return_if_fail(p_ctrl != NULL);
   if (p_ctrl->p_enhancer == NULL || i_idx < 0 ||
       i_idx >= (gint)G_N_ELEMENTS(p_ctrl->p_btns)) {
      return;
   }
   p_ctrl->u_enhance_mask ^= (guint8)(1u << i_idx);
   _update_highlights(p_ctrl);
   _apply_async(p_ctrl);
}

/* --- choke points -------------------------------------------------------- */

/* "changed" fires for every navigator rescan, not only an actual move to a
 * different current file -- notably, enhance-save writes the "-enhanced" copy
 * into the SAME live-monitored folder, whose GFileMonitor then schedules a
 * debounced rescan that re-emits "changed" a few hundred ms later even though
 * navigator.current never moved. Discovered by tests/test_enhance_flow.c's
 * save-twice-in-a-row subtest: without this check, that incidental rescan
 * silently zeroed u_enhance_mask right after a successful save, discarding the
 * still-active preview the user was not done comparing/adjusting. Only reset
 * when the current file's IDENTITY actually changed; p_enhance_file is updated
 * unconditionally so the next call has an accurate baseline. */
void
enhance_ctrl_nav_changed(EnhanceCtrl *p_ctrl) {
   g_return_if_fail(p_ctrl != NULL);
   GFile   *p_cur  = _current_file(p_ctrl);
   gboolean b_same = (p_cur != NULL && p_ctrl->p_enhance_file != NULL &&
                      g_file_equal(p_cur, p_ctrl->p_enhance_file));
   if (!b_same) {
      _destroy(p_ctrl);
      p_ctrl->u_enhance_mask  = 0;
      p_ctrl->b_hold_original = FALSE; /* mask cleared without going through
                                        * _apply_async, so reset the hold flag
                                        * here too (issue 4) */
      p_ctrl->u_enhance_gen++;
      g_cancellable_cancel(p_ctrl->p_enhance_cancel);
      g_clear_object(&p_ctrl->p_enhance_tex);
      _update_highlights(p_ctrl);
   }
   g_set_object(&p_ctrl->p_enhance_file, p_cur);
}

/* --- internal: row toggle (clicked handler for the built buttons) ------- */

/* Forward-declared above via _build_box's G_CALLBACK; defined here. Clicked
 * row: idx 0..7 toggles that preset's bit; idx -1 (Original) discards the
 * whole preview. Then refresh highlights + re-apply the (possibly empty)
 * chain. Does NOT close the popover -- toggling presets while comparing is
 * the point of the layered design (docs/gegl.md). */
static void
_row_toggle(EnhanceCtrl *p_ctrl, GtkWidget *p_btn) {
   gint i_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(p_btn), "idx"));
   if (i_idx < 0) {
      _discard(p_ctrl);
      return;
   }
   if (i_idx < (gint)G_N_ELEMENTS(p_ctrl->p_btns)) {
      p_ctrl->u_enhance_mask ^= (guint8)(1u << i_idx);
   }
   _update_highlights(p_ctrl);
   _apply_async(p_ctrl);
}