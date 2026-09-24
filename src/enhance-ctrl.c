/*:*
 * ggaze — Enhance/GEGL UI orchestration controller
 *
 * See enhance-ctrl.h. The preset mask, the in-flight apply/preview/export
 * requests, the cached enhanced texture, the hold-Space flag, the saved
 * flag and the side panel live here; the window reaches it through a few
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
#include "transform.h"

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
static void     _sync_panel(EnhanceCtrl *p_ctrl);
static void     _sync_save_target(EnhanceCtrl *p_ctrl);
static void     _apply_async(EnhanceCtrl *p_ctrl);
static void     _discard(EnhanceCtrl *p_ctrl);
static void     _destroy(EnhanceCtrl *p_ctrl);
static void     _start_previews(EnhanceCtrl *p_ctrl);
static void     _card_toggle(EnhanceCtrl *p_ctrl, GtkWidget *p_btn);
static gboolean _orig_size(EnhanceCtrl *p_ctrl, gint *p_w, gint *p_h);
static void     _drop_managed(EnhanceCtrl *p_ctrl);
static void     _drop_managed_orig(EnhanceCtrl *p_ctrl);
static void     _fetch_managed_original(EnhanceCtrl *p_ctrl);

/* --- struct --------------------------------------------------------------- */

struct EnhanceCtrl {
   const EnhanceUIHostOps *p_ops;  /* borrowed, for the controller's lifetime */
   gpointer                p_host; /* the window, borrowed */

   Enhancer *p_enhancer;     /* GEGL preset engine (always non-NULL) */
   guint8    u_enhance_mask; /* bit i -> preset i enabled (layered) */
   Transform t_xf;           /* the COMMITTED rotate 90 / straighten / crop,
                              * applied after the presets in the same graph
                              * (decision #35); the identity when none. What
                              * `s` exports and dirty/active are judged on */
   Transform t_preview;      /* a tool's rendering override (see the header:
                              * the crop tool shows the base) ... */
   gboolean b_preview;       /* ... in effect iff this is set */
   gint     i_orig_w;        /* the ORIGINAL's upright size, from the
                              * decode the viewer showed for it (learned
                              * with p_orig_tex below) or the last landed
                              * apply's (0 = not known yet); every LEARNED
                              * change of it is told to the tool
                              * (original_changed), since
                              * transform_base_size of it is the image the
                              * crop rectangle lives on. _forget_original
                              * zeroes it without telling: the tool's draw
                              * path re-reads it (_rect_on_base) and hides
                              * the rectangle until it is learned again */
   gint     i_orig_h;
   gboolean b_apply_pending; /* an apply is in flight: what is on screen
                              * predates u_enhance_mask/t_xf */
   gboolean b_relaunch;      /* the state changed while that apply ran: render
                              * once more when it lands (the coalescing slot;
                              * never more than one is ever queued) */
   guint u_render_count;     /* launches so far (a test seam) */
   guint u_orig_fetches;     /* managed-original fetches launched so far
                              * (a test seam) */
   gboolean b_disposed;      /* set by enhance_ctrl_dispose */
   gboolean b_saved;         /* the state on screen IS the saved pair below:
                              * active but no longer dirty, so moving on does
                              * not prompt for it (_refresh_saved keeps it
                              * in step with every state change) */
   gboolean  b_have_saved;   /* an export succeeded for this file ... */
   guint8    u_saved_mask;   /* ... with this mask ... */
   Transform t_saved_xf;     /* ... and this transform */
   char     *c_saved_name;   /* basename of that export, for the panel */
   gboolean  b_hint_shown;   /* the "Space compares / s saves / a shows the
                              * presets" status line was shown for this file
                              * (it is shown once per file, and only when a
                              * preset is applied with the panel closed) */

   /* The side panel and the widgets in it that change after the build. All
    * NULL while closed (_destroy clears them), so every sync helper can run
    * unconditionally. */
   gboolean   b_thumbnails; /* the open panel has picture cards */
   GtkWidget *p_panel;      /* the panel root, parented in the host's slot */
   GtkWidget *p_original_pic;
   GtkWidget *p_btns[GGAZE_ENHANCE_MAX_PRESETS]; /* preset cards */
   GtkWidget *p_pics[GGAZE_ENHANCE_MAX_PRESETS]; /* their pictures */
   GtkWidget *p_state;                           /* save-state line */
   GtkWidget *p_save_btn;
   GtkWidget *p_save_target; /* "as <name>" the next Save writes */

   GCancellable *p_preview_cancel; /* thumbnail-preview batch */
   guint         u_preview_gen;    /* invalidates stale batch completions */
   guint         u_preview_count;  /* batches started so far (a test seam:
                                    * an open re-points the panel once) */
   GdkTexture *p_enhance_tex;      /* last-applied modified texture, cached
                                    * so hold-Space can restore it without a
                                    * GEGL recompute */
   GdkTexture *p_orig_tex;         /* the current file's ORIGINAL as the
                                    * viewer last showed it -- learned at the
                                    * window's texture choke point
                                    * (enhance_ctrl_texture_shown) from every
                                    * decoded texture it puts up, so a reload
                                    * that decodes the same file into a new
                                    * object refreshes it (owned ref; NULL =
                                    * nothing shown for this file since the
                                    * last forget): the identity
                                    * enhance_ctrl_is_current_render compares
                                    * the screen against when nothing is
                                    * rendered, and what hold-Space shows.
                                    * Never looked up from the cache, whose
                                    * get stats the file and evicts a stale
                                    * entry (a `touch` on the file made the
                                    * crop overlay vanish; an entry learned
                                    * once and never refreshed refused every
                                    * Enter after a same-file reload) */
   gboolean b_managed;             /* the last landed render of
                                    * p_enhance_file decoded it colour-
                                    * managed (decision #45: a profiled PNG /
                                    * JPEG, profile applied, whatever the
                                    * working space -- CMYK and grey too):
                                    * hold-Space then wants p_managed_orig */
   GdkTexture *p_managed_orig;     /* the ORIGINAL through the render's own
                                    * colour-managed decode (owned ref; NULL
                                    * = not managed, or not fetched yet).
                                    * What hold-Space shows in place of
                                    * p_orig_tex, whose unmanaged decode
                                    * would make the compare show a colour
                                    * shift no preset caused. Fetched LAZILY,
                                    * on the first Space press
                                    * (_fetch_managed_original): a second
                                    * full-size texture outside the texture
                                    * cache's cap, ~100-200 MB at 24-50 MP,
                                    * which a session that never holds Space
                                    * should not pay for. Dropped
                                    * (_drop_managed) on discard / nothing
                                    * left to render, another file, and a
                                    * rewrite of this one */
   GCancellable *p_orig_cancel;    /* the in-flight managed-original fetch
                                    * (NULL = none); cancelled and dropped
                                    * with p_managed_orig */
   guint u_orig_gen;               /* bumped by _drop_managed: a fetch that
                                    * lands under an older value is stale */
   GCancellable *p_enhance_cancel; /* in-flight enhance-apply GTask */
   GCancellable *p_save_cancel;    /* in-flight export (`s`); cancelled on
                                    * dispose so a closing window never gets
                                    * a late completion */
   guint u_enhance_gen;            /* bumped on every apply/discard; a
                                    * completion whose request predates the
                                    * current value is stale and dropped
                                    * (last-write-wins -- GEGL processing
                                    * cannot be aborted mid-flight once
                                    * started); also what tells a finished
                                    * export whether the mask it wrote is
                                    * still the one on screen */
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

/* The panel opened or closed: the host's key mode follows (optional op). */
static void
_mode_changed(EnhanceCtrl *p_ctrl) {
   if (p_ctrl->p_ops->mode_changed != NULL) {
      p_ctrl->p_ops->mode_changed(p_ctrl->p_host);
   }
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
   transform_init(&p_ctrl->t_xf);
   return (p_ctrl);
}

/* TRUE iff there is anything to preview: a preset enabled or a transform
 * that is not the identity. The one definition of "active" every state
 * query, the texture override and the apply path share. */
static gboolean
_has_work(EnhanceCtrl *p_ctrl) {
   return (p_ctrl->u_enhance_mask != 0 ||
           !transform_is_identity(&p_ctrl->t_xf));
}

/* The transform the preview graph renders: a tool's override while one is
 * set, else the committed one. */
static const Transform *
_render_transform(EnhanceCtrl *p_ctrl) {
   return (p_ctrl->b_preview ? &p_ctrl->t_preview : &p_ctrl->t_xf);
}

/* TRUE iff rendering needs the GEGL chain at all -- the crop tool over a
 * crop-only transform has work (_has_work) but renders the plain original. */
static gboolean
_render_has_work(EnhanceCtrl *p_ctrl) {
   return (p_ctrl->u_enhance_mask != 0 ||
           !transform_is_identity(_render_transform(p_ctrl)));
}

/* Saved iff the committed state is exactly the pair the last export wrote
 * (see the header): re-derived after every state change rather than
 * cleared by it, so coming back to the saved state counts as saved. */
static void
_refresh_saved(EnhanceCtrl *p_ctrl) {
   p_ctrl->b_saved = p_ctrl->b_have_saved &&
                     p_ctrl->u_enhance_mask == p_ctrl->u_saved_mask &&
                     transform_equal(&p_ctrl->t_xf, &p_ctrl->t_saved_xf);
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
   g_clear_object(&p_ctrl->p_orig_tex);
   g_clear_object(&p_ctrl->p_managed_orig);
   g_clear_object(&p_ctrl->p_orig_cancel);
   g_clear_object(&p_ctrl->p_enhance_file);
   g_clear_pointer(&p_ctrl->p_enhancer, enhancer_delete);
   g_free(p_ctrl->c_saved_name);
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
   _drop_managed(p_ctrl); /* cancels a managed-original fetch too */
   g_clear_object(&p_ctrl->p_enhance_tex);
   g_clear_object(&p_ctrl->p_orig_tex);
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
enhance_ctrl_is_active(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   return (p_ctrl->p_enhancer != NULL && _has_work(p_ctrl));
}

gboolean
enhance_ctrl_is_dirty(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   return (enhance_ctrl_is_active(p_ctrl) && !p_ctrl->b_saved);
}

gboolean
enhance_ctrl_is_open(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   return (p_ctrl->p_panel != NULL);
}

/* Under hold-Space the managed original, once fetched, stands for the
 * original: it is what the screen shows then, so the window's texture
 * choke point keeps it up and the info card (window.c _info_texture_for)
 * plots it as the current file's -- without this the histogram went blank
 * whenever Space showed a managed original. */
GdkTexture *
enhance_ctrl_override_texture(EnhanceCtrl *p_ctrl, GdkTexture *p_tex) {
   if (p_ctrl == NULL || !_has_work(p_ctrl) || p_ctrl->p_enhance_tex == NULL) {
      return (p_tex);
   }
   if (p_ctrl->b_hold_original) {
      return (p_ctrl->p_managed_orig != NULL ? p_ctrl->p_managed_orig : p_tex);
   }
   return (p_ctrl->p_enhance_tex);
}

/* Remember the original's size. TRUE iff it differs from what was known
 * (including "not known yet", 0x0): the caller tells the tool then, since
 * the base the crop rectangle lives on is derived from it. */
static gboolean
_note_orig_size(EnhanceCtrl *p_ctrl, gint i_w, gint i_h) {
   if (p_ctrl->i_orig_w == i_w && p_ctrl->i_orig_h == i_h) {
      return (FALSE);
   }
   p_ctrl->i_orig_w = i_w;
   p_ctrl->i_orig_h = i_h;
   return (TRUE);
}

/* Remember p_tex as the original's identity, with its size, and tell the
 * tool: a rectangle laid out on the previous object (or on none yet) lays
 * out again on this one, whether or not the size moved -- the overlay is
 * hidden while the object on screen is not the one known here, so the
 * redraw is what brings it back. Every path that learns an identity goes
 * through here (the window's texture choke point, a rescan finding a
 * decode the viewer has not shown), so none can forget to tell it. */
static void
_learn_original(EnhanceCtrl *p_ctrl, GdkTexture *p_tex) {
   if (p_ctrl->p_orig_tex != NULL && p_ctrl->p_orig_tex != p_tex) {
      /* A NEW decode of the file replaces a known one: the file may have
       * been rewritten since the managed original was fetched from it, so
       * that one (or its fetch in flight) goes too. b_managed stays: it is
       * the last render's, and the next Space press fetches again from the
       * file as it is now (a rewrite into an unmanaged file comes back
       * NULL, which clears it). A same-content reload costs a re-fetch. */
      _drop_managed_orig(p_ctrl);
   }
   g_set_object(&p_ctrl->p_orig_tex, p_tex);
   _note_orig_size(p_ctrl, gdk_texture_get_width(p_tex),
                   gdk_texture_get_height(p_tex));
   p_ctrl->p_ops->original_changed(p_ctrl->p_host);
}

/* Where the original's identity (and, with it, its size) is learned in
 * the ordinary course (the other caller of _learn_original is
 * _recheck_original, for a rescan that finds a decode the viewer has not
 * shown yet): the window tells this controller about every decoded
 * texture of the current file it is about to show -- a cache hit, a
 * finished load, a reload after a discard, a rescan's reload of a
 * rewritten file -- and the viewer only ever shows the current file's own
 * decode (viewload's last-write-wins), so that texture IS the original as
 * of now, whether or not a preview is going to be put on screen in its
 * place. Learning it here rather than looking it up from the cache on
 * first need keeps the remembered object in step with the one the viewer
 * holds: a same-file reload without a rescan (the file touched, then a
 * preset discarded) decodes a NEW object that a one-time lookup never saw,
 * and every tool check against the old one failed for good. The
 * controller's own textures teach it nothing: the window passes only what
 * viewload decoded. Nothing here looks anything up. A tool laid out on the
 * previous object is told so it can lay out again (original_changed,
 * _learn_original). */
void
enhance_ctrl_texture_shown(EnhanceCtrl *p_ctrl, GdkTexture *p_tex) {
   if (p_ctrl == NULL || p_tex == NULL || _disposed(p_ctrl) ||
       !_has_navigator(p_ctrl) || p_tex == p_ctrl->p_enhance_tex) {
      return;
   }
   if (p_tex == p_ctrl->p_orig_tex) {
      return; /* the one already known (hold-Space, a cache hit) */
   }
   _learn_original(p_ctrl, p_tex);
}

void
enhance_ctrl_set_hold_original(EnhanceCtrl *p_ctrl, gboolean b_hold) {
   g_return_if_fail(p_ctrl != NULL);
   if (!_has_navigator(p_ctrl) || p_ctrl->p_enhancer == NULL ||
       !_has_work(p_ctrl) || p_ctrl->b_hold_original == b_hold) {
      return;
   }
   p_ctrl->b_hold_original = b_hold;
   if (b_hold) {
      /* A colour-managed file compares against its managed original (the
       * render's own decode, p_managed_orig), so only the presets differ.
       * Otherwise -- and, on the FIRST press on a managed render, until
       * the lazily fetched managed original lands (_managed_orig_done
       * swaps it in if Space is still held) -- the original as the viewer
       * last showed it (p_orig_tex): immediate feedback beats a Space
       * that seems dead for the second a 50 MP re-decode takes, and the
       * swap is the visible cue that the exact compare has arrived. This
       * controller holds its own reference, so neither an LRU eviction nor
       * a `touch` on the file (which stales the cache entry) can turn
       * Space into a no-op. It is NULL only while the file's decode has
       * not landed yet (a rewrite's rescan forgot it, the reload is in
       * flight): a silent no-op then rather than a synchronous re-decode
       * on the main thread. */
      GdkTexture *p_orig = p_ctrl->p_managed_orig != NULL
                              ? p_ctrl->p_managed_orig
                              : p_ctrl->p_orig_tex;
      if (p_orig != NULL) {
         _show_texture(p_ctrl, p_orig);
      }
      _fetch_managed_original(p_ctrl);
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
   char *c_msg   = NULL;
   if (b_ok) {
      c_msg = g_strdup_printf("Saved %s", c_saved);
   } else {
      c_msg = g_strdup_printf("Enhance-save failed: %s",
                              p_err != NULL ? p_err->message : "?");
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
           _has_work(p_ctrl) && p_ctrl->p_enhance_file != NULL);
}

/* Per-export context: the destination (for the report), the (mask,
 * transform) pair the export writes (so the completion records exactly what
 * is on disk, whatever the state did meanwhile), the caller's continuation,
 * and a ref on the host so the completion can run after a dispose without
 * dangling (it then only releases). */
typedef struct {
   gpointer          p_host; /* ref'd window */
   EnhanceCtrl      *p_ctrl; /* borrowed, valid while p_host is alive */
   GFile            *p_out;  /* owned */
   guint8            u_mask;
   Transform         t_xf;
   EnhanceSaveDoneFn fn_done;
   gpointer          p_done_data;
} _SaveReq;

/* A finished export records the pair it wrote as the saved one; the preview
 * is then saved iff the state still is (or comes back to) that pair -- a
 * mask that moved on while the worker ran stays dirty, as before, but is
 * saved again the moment it is toggled back. */
static void
_save_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   _SaveReq    *p_req  = (_SaveReq *)p_data;
   EnhanceCtrl *p_ctrl = p_req->p_ctrl;
   GError      *p_err  = NULL;
   gboolean     b_ok   = enhancer_export_chain_finish(p_res, &p_err);
   if (!_disposed(p_ctrl)) {
      _save_report(p_ctrl, p_req->p_out, b_ok, p_err);
      if (b_ok) {
         p_ctrl->b_have_saved = TRUE;
         p_ctrl->u_saved_mask = p_req->u_mask;
         p_ctrl->t_saved_xf   = p_req->t_xf;
         g_free(p_ctrl->c_saved_name);
         p_ctrl->c_saved_name = g_file_get_basename(p_req->p_out);
         _refresh_saved(p_ctrl);
         _sync_panel(p_ctrl);
         _sync_save_target(p_ctrl); /* that name is taken now: -1, -2 ... */
      }
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
      _show_status(p_ctrl, "Nothing to save (no preset or transform active)");
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
   p_req->u_mask         = p_ctrl->u_enhance_mask;
   p_req->t_xf           = p_ctrl->t_xf;
   p_req->fn_done        = fn_done;
   p_req->p_done_data    = p_done_data;
   char *c_name          = g_file_get_basename(p_out);
   char *c_msg           = g_strdup_printf("Saving %s…", c_name);
   _show_status(p_ctrl, c_msg);
   g_free(c_msg);
   g_free(c_name);
   enhancer_export_chain_async(p_ctrl->p_enhance_file,
                               enhancer_get_presets(p_ctrl->p_enhancer),
                               p_ctrl->u_enhance_mask, &p_ctrl->t_xf, p_out,
                               p_ctrl->p_save_cancel, _save_done_cb, p_req);
}

/* --- panel sync ----------------------------------------------------------- */

/* Bring the open panel in line with the state: each preset card's
 * "ggaze-enhance-on" highlight from the mask, and the save-state line +
 * Save button from active/saved. A no-op while the panel is closed (every
 * widget pointer is NULL then), so callers never check p_panel first. */
static void
_sync_panel(EnhanceCtrl *p_ctrl) {
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
   if (p_ctrl->p_state != NULL) {
      enhance_ui_set_save_state(p_ctrl->p_state, p_ctrl->p_save_btn,
                                _has_work(p_ctrl), p_ctrl->b_saved,
                                p_ctrl->c_saved_name);
   }
}

/* Name the file the next Save would write under the Save button: the
 * export's own non-colliding destination for the current file
 * (enhancer_export_dest_for), so what the button promises is what `s`
 * writes. It probes the folder for a free name, so it runs only when that
 * can change -- the panel opening, another file, a save landing -- not on
 * every state change. A no-op while the panel is closed. */
static void
_sync_save_target(EnhanceCtrl *p_ctrl) {
   if (p_ctrl->p_save_target == NULL) {
      return;
   }
   GFile *p_cur  = _current_file(p_ctrl);
   GFile *p_dest = p_cur != NULL ? enhancer_export_dest_for(p_cur) : NULL;
   char  *c_name = p_dest != NULL ? g_file_get_basename(p_dest) : NULL;
   enhance_ui_set_save_target(p_ctrl->p_save_target, c_name);
   g_free(c_name);
   g_clear_object(&p_dest);
}

/* --- the managed original (hold-Space on a colour-managed render) -------- */

/* Forget the managed original and cancel a fetch in flight (its
 * completion is stale by u_orig_gen). */
static void
_drop_managed_orig(EnhanceCtrl *p_ctrl) {
   p_ctrl->u_orig_gen++;
   if (p_ctrl->p_orig_cancel != NULL) {
      g_cancellable_cancel(p_ctrl->p_orig_cancel);
      g_clear_object(&p_ctrl->p_orig_cancel);
   }
   g_clear_object(&p_ctrl->p_managed_orig);
}

/* _drop_managed_orig(), and forget whether the file decodes managed. */
static void
_drop_managed(EnhanceCtrl *p_ctrl) {
   _drop_managed_orig(p_ctrl);
   p_ctrl->b_managed = FALSE;
}

/* A fetch's context: the host ref keeps the borrowed controller alive (as
 * _Req does), u_gen says whether it is still wanted. */
typedef struct {
   gpointer     p_host; /* ref'd window */
   EnhanceCtrl *p_ctrl; /* borrowed, valid while p_host is alive */
   guint        u_gen;
} _OrigReq;

/* The fetch landed: keep the texture and, while Space is still held, put
 * it up in place of the plain original shown meanwhile. NULL without an
 * error means the file no longer decodes managed (rewritten meanwhile):
 * the plain original IS the compare then, so nothing is fetched again. A
 * failure is logged and costs only the exact compare. */
static void
_managed_orig_done(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   _OrigReq    *p_req  = p_data;
   EnhanceCtrl *p_ctrl = p_req->p_ctrl;
   GError      *p_err  = NULL;
   GdkTexture  *p_tex  = enhancer_managed_original_finish(p_res, &p_err);
   if (!_disposed(p_ctrl) && p_req->u_gen == p_ctrl->u_orig_gen) {
      g_clear_object(&p_ctrl->p_orig_cancel);
      p_ctrl->b_managed = p_tex != NULL;
      g_set_object(&p_ctrl->p_managed_orig, p_tex);
      if (p_err != NULL) {
         g_warning("ggaze: managed original failed: %s", p_err->message);
      }
      if (p_tex != NULL && p_ctrl->b_hold_original) {
         _show_texture(p_ctrl, p_tex);
      }
   }
   g_clear_error(&p_err);
   g_clear_object(&p_tex);
   g_object_unref(p_req->p_host);
   g_free(p_req);
}

/* Start fetching the managed original of p_enhance_file when the last
 * render says it decodes managed and none is held or in flight. */
static void
_fetch_managed_original(EnhanceCtrl *p_ctrl) {
   if (!p_ctrl->b_managed || p_ctrl->p_managed_orig != NULL ||
       p_ctrl->p_orig_cancel != NULL || p_ctrl->p_enhance_file == NULL) {
      return;
   }
   _OrigReq *p_req       = g_new(_OrigReq, 1);
   p_req->p_host         = g_object_ref(p_ctrl->p_host);
   p_req->p_ctrl         = p_ctrl;
   p_req->u_gen          = p_ctrl->u_orig_gen;
   p_ctrl->p_orig_cancel = g_cancellable_new();
   p_ctrl->u_orig_fetches++;
   enhancer_managed_original_async(
      p_ctrl->p_enhance_file, p_ctrl->p_orig_cancel, _managed_orig_done, p_req);
}

/* --- apply / discard ----------------------------------------------------- */

/* Per-request context for _apply_async's async completion: a ref on the host
 * window (so it -- and thus this borrowed controller -- outlives the worker
 * even across a dispose), the generation the request was launched at (for
 * the last-write-wins check in _apply_done_cb), and whether the completion
 * should tell the user how to compare/save (the panel was closed when the
 * preset was applied, and this file has not had the hint yet); the
 * completion parks whether the decode was colour-managed here until
 * _apply_landed takes it. */
typedef struct {
   gpointer     p_host; /* ref'd window */
   EnhanceCtrl *p_ctrl; /* borrowed, valid while p_host is alive */
   guint        u_gen;
   gboolean     b_hint;
   gboolean     b_managed; /* from the finish */
} _Req;

static void
_req_free(_Req *p_req) {
   if (p_req == NULL) {
      return;
   }
   g_object_unref(p_req->p_host);
   g_free(p_req);
}

static void _render(EnhanceCtrl *p_ctrl);

/* A render landed and is still wanted: show it. The worker decoded the
 * original, so its size is known here even before the viewer has shown a
 * decode of it -- after a rewrite in place the render may land BEFORE the
 * reload's decode, and a crop tool laid out on the old size used to draw
 * its rectangle over the new render until that decode taught the
 * controller (the overlay's own base guard, tool-ctrl.c _rect_on_base,
 * would hide it now, but only a relayout puts the rectangle where it
 * belongs). So a size that moved is told the same way a new original is
 * (original_changed), once the render is on screen. The original's
 * IDENTITY is not touched: the render is not it, and hold-Space keeps
 * showing the last decode the viewer showed until the reload lands. */
static void
_apply_landed(EnhanceCtrl *p_ctrl, const _Req *p_req, GdkTexture *p_tex,
              gint i_w, gint i_h) {
   g_set_object(&p_ctrl->p_enhance_tex, p_tex);
   if (!p_req->b_managed) {
      _drop_managed(p_ctrl); /* rewritten into a file with nothing to manage */
   }
   p_ctrl->b_managed = p_req->b_managed;
   _show_texture(p_ctrl, p_tex);
   _update_header(p_ctrl);
   if (_note_orig_size(p_ctrl, i_w, i_h)) {
      p_ctrl->p_ops->original_changed(p_ctrl->p_host);
   }
   if (p_req->b_hint) {
      /* Without the panel nothing on screen says how to compare or keep
       * the result; say it once, when the first preview of a file lands. */
      _show_status(p_ctrl, "Enhanced preview — hold Space to compare, "
                           "s saves a copy, a shows the presets");
   }
}

/* Async apply completion (tu0): last-write-wins via u_enhance_gen -- a
 * discard, a navigation, or the window closing while this was still
 * processing in its worker thread all bump the generation, so a stale result
 * here is dropped instead of clobbering whatever is now current. A newer
 * STATE is not a reason to drop it: the result is shown (the screen catches
 * up one step) and the latest state rendered right after (b_relaunch). */
static void
_apply_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   _Req        *p_req  = (_Req *)p_data;
   EnhanceCtrl *p_ctrl = p_req->p_ctrl;
   GError      *p_err  = NULL;
   gint         i_w    = 0;
   gint         i_h    = 0;
   GdkTexture  *p_tex =
      enhancer_apply_chain_finish(p_res, &i_w, &i_h, &p_req->b_managed, &p_err);
   if (_disposed(p_ctrl) || p_req->u_gen != p_ctrl->u_enhance_gen) {
      g_clear_object(&p_tex);
      g_clear_error(&p_err);
      _req_free(p_req);
      return;
   }
   p_ctrl->b_apply_pending = FALSE;
   if (p_tex == NULL) {
      g_warning("ggaze: enhance failed: %s",
                p_err != NULL ? p_err->message : "(no detail)");
      g_clear_error(&p_err);
      _show_status(p_ctrl, "Enhance failed");
      _discard(p_ctrl); /* back to the original; also bumps the gen and
                         * drops a queued relaunch (it would fail too) */
   } else {
      _apply_landed(p_ctrl, p_req, p_tex, i_w, i_h);
      g_object_unref(p_tex);
   }
   if (p_ctrl->b_relaunch) {
      p_ctrl->b_relaunch = FALSE;
      _render(p_ctrl); /* the state moved on meanwhile: once more, latest */
   }
   _req_free(p_req);
}

/* Invalidate whatever is in flight: bump u_enhance_gen and replace
 * p_enhance_cancel, so a still-running apply's result is recognized as stale
 * and dropped when it eventually completes (GEGL processing itself cannot be
 * aborted mid-flight once started), and forget a queued relaunch. */
static void
_drop_inflight(EnhanceCtrl *p_ctrl) {
   p_ctrl->u_enhance_gen++;
   p_ctrl->b_apply_pending = FALSE;
   p_ctrl->b_relaunch      = FALSE;
   g_cancellable_cancel(p_ctrl->p_enhance_cancel);
   g_clear_object(&p_ctrl->p_enhance_cancel);
   p_ctrl->p_enhance_cancel = g_cancellable_new();
}

/* Launch the async apply for the has-render-work case: record which file
 * the preview applies to (see nav_changed's comment for why this is set
 * here, not only in nav_changed) and hand off to enhancer_apply_chain_async
 * with the RENDER transform (a tool's override, else the committed one). */
static void
_launch(EnhanceCtrl *p_ctrl, GFile *p_file) {
   if (p_ctrl->p_enhance_file == NULL ||
       !g_file_equal(p_ctrl->p_enhance_file, p_file)) {
      _drop_managed(p_ctrl); /* another file's */
   }
   g_set_object(&p_ctrl->p_enhance_file, p_file);
   _Req *p_req          = g_new0(_Req, 1);
   p_req->p_host        = g_object_ref(p_ctrl->p_host);
   p_req->p_ctrl        = p_ctrl;
   p_req->u_gen         = p_ctrl->u_enhance_gen;
   p_req->b_hint        = p_ctrl->p_panel == NULL && !p_ctrl->b_hint_shown;
   p_ctrl->b_hint_shown = p_ctrl->b_hint_shown || p_req->b_hint;
   const GPtrArray *p_presets = enhancer_get_presets(p_ctrl->p_enhancer);
   p_ctrl->b_apply_pending    = TRUE;
   p_ctrl->u_render_count++;
   enhancer_apply_chain_async(p_file, p_presets, p_ctrl->u_enhance_mask,
                              _render_transform(p_ctrl),
                              p_ctrl->p_enhance_cancel, _apply_done_cb, p_req);
}

/* Canonical "nothing to render" site -- every path that clears the state
 * (x / the Revert button, the fourth quarter turn, the
 * easy-to-miss one: toggling the LAST enabled preset back off via
 * win.enhance-N or a card) and the crop tool opening over a crop-only
 * transform funnel through here. Shows the original (texturecache is fast,
 * no GEGL) and drops anything in flight. Forces the hold-compare flag off:
 * set_hold_original no-ops once nothing is active, so a Space RELEASE
 * arriving after the mask was cleared out from under a still-held key would
 * otherwise leave the flag stuck TRUE and swallow the next press (tu0 review
 * round 2, issue 4). */
static void
_restore_original(EnhanceCtrl *p_ctrl) {
   _drop_inflight(p_ctrl);
   _drop_managed(p_ctrl); /* no preview left to compare against */
   p_ctrl->b_hold_original = FALSE;
   g_clear_object(&p_ctrl->p_enhance_tex);
   _load_current(p_ctrl);
   _update_header(p_ctrl);
}

/* Bring the screen in line with the state, off the GTK main thread
 * (enhancer_apply_chain_async: GEGL processing is CPU-heavy, AGENTS.md
 * "Decode runs in GTask threads"). With an apply already in flight the state
 * is only marked for a re-render when it lands (the coalescing slot), so a
 * burst of nudges costs one extra render rather than one per nudge. */
static void
_render(EnhanceCtrl *p_ctrl) {
   if (!_has_navigator(p_ctrl) || p_ctrl->p_enhancer == NULL) {
      return;
   }
   _refresh_saved(p_ctrl);
   _sync_panel(p_ctrl);
   if (!_render_has_work(p_ctrl)) {
      _restore_original(p_ctrl);
      return;
   }
   GFile *p_file = _current_file(p_ctrl);
   if (p_file == NULL) {
      p_ctrl->u_enhance_mask = 0;
      transform_init(&p_ctrl->t_xf);
      p_ctrl->b_preview       = FALSE;
      p_ctrl->b_hold_original = FALSE; /* see _restore_original */
      _drop_inflight(p_ctrl);
      _sync_panel(p_ctrl);
      _update_header(p_ctrl);
      return;
   }
   if (p_ctrl->b_apply_pending) {
      p_ctrl->b_relaunch = TRUE;
      return;
   }
   _drop_inflight(p_ctrl); /* a fresh generation for the new launch */
   _launch(p_ctrl, p_file);
}

/* A preset was toggled or a transform committed: switch to the large view
 * (the preview only makes sense there) and render. Tools use _render
 * directly -- they have the large view already, and an override cleared by
 * a view change must not pull the view back. */
static void
_apply_async(EnhanceCtrl *p_ctrl) {
   if (!_has_navigator(p_ctrl) || p_ctrl->p_enhancer == NULL) {
      return;
   }
   p_ctrl->p_ops->ensure_large_view(p_ctrl->p_host);
   _render(p_ctrl);
}

/* Drop the current enhance preview and go back to showing the unmodified
 * original: clears the mask + cached texture and reloads the original
 * (_apply_async's mask==0 path also invalidates any in-flight apply via
 * u_enhance_gen). Used by x / the Revert button (explicit, no prompt -- Esc no
 * longer discards, 6i2), the slideshow timer, a failed apply, and after
 * Save/Discard
 * in the navigate-away prompt. Never touches the file on disk -- discarding
 * a preview only drops in-memory state. A crop / straighten session over
 * the preview ends FIRST (abandon_tool): the tool's working copy of the
 * transform would otherwise survive the reset and come back on its next
 * nudge or Enter. */
static void
_discard(EnhanceCtrl *p_ctrl) {
   p_ctrl->p_ops->abandon_tool(p_ctrl->p_host);
   p_ctrl->u_enhance_mask = 0;
   transform_init(&p_ctrl->t_xf);   /* a discard drops the turn/crop too */
   p_ctrl->b_preview       = FALSE; /* and a tool's override with it */
   p_ctrl->b_hold_original = FALSE; /* belt-and-braces: _restore_original
                                     * also does this, but _render
                                     * early-returns without a
                                     * navigator/enhancer (issue 4) */
   _drop_managed(p_ctrl);           /* the same belt-and-braces */
   _apply_async(p_ctrl);
}

/* --- the geometric transform --------------------------------------------- */

const Transform *
enhance_ctrl_get_transform(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, NULL);
   return (&p_ctrl->t_xf);
}

/* A crop lying entirely outside its base (committed on one base, then a
 * straighten that shrank the base past it) is committed as it is, not
 * dropped: the chain applies transform_effective_crop, which is empty then
 * and crops nothing, the title says so ("crop (outside view)",
 * transform_describe), and a nudge back over it applies it again --
 * dropping it here lost the rectangle for good after one nudge too far. */
void
enhance_ctrl_set_transform(EnhanceCtrl *p_ctrl, const Transform *p_xf) {
   g_return_if_fail(p_ctrl != NULL && p_xf != NULL);
   Transform t_new   = *p_xf;
   gboolean  b_same  = transform_equal(_render_transform(p_ctrl), &t_new);
   p_ctrl->t_xf      = t_new;
   p_ctrl->b_preview = FALSE; /* a commit ends any tool override */
   if (b_same) {
      /* Nothing to render (e.g. the auto-crop flag at 0 degrees, or a crop
       * tool that committed what was on screen), but the committed state
       * may still have changed in name: keep saved/panel/title in step. */
      _refresh_saved(p_ctrl);
      _sync_panel(p_ctrl);
      _update_header(p_ctrl);
      return;
   }
   _apply_async(p_ctrl);
}

void
enhance_ctrl_set_preview_transform(EnhanceCtrl *p_ctrl, const Transform *p_xf) {
   g_return_if_fail(p_ctrl != NULL);
   Transform t_before = *_render_transform(p_ctrl);
   if (p_xf != NULL) {
      p_ctrl->t_preview = *p_xf;
      p_ctrl->b_preview = TRUE;
   } else {
      p_ctrl->b_preview = FALSE;
   }
   if (!transform_equal(&t_before, _render_transform(p_ctrl))) {
      _render(p_ctrl); /* not _apply_async: never switches views */
   }
}

/* Forget what is known about the original (another file, or this one
 * rewritten in place): the reload that follows shows the file's fresh
 * decode, and enhance_ctrl_texture_shown learns it again from that. */
static void
_forget_original(EnhanceCtrl *p_ctrl) {
   g_clear_object(&p_ctrl->p_orig_tex);
   _drop_managed(p_ctrl);
   p_ctrl->i_orig_w = 0;
   p_ctrl->i_orig_h = 0;
}

/* The original's size, from the texture the viewer showed for it
 * (enhance_ctrl_texture_shown) or from the last landed apply's decode.
 * FALSE when neither has happened yet -- the file's first decode is still
 * in flight, or a rewrite's rescan forgot it and the reload has not landed.
 * A pure read: the tools ask for the base size from the draw path (a
 * rectangle laid out again after a rewrite) as well as from every key, and
 * nothing here may stat a file or touch the cache. */
static gboolean
_orig_size(EnhanceCtrl *p_ctrl, gint *p_w, gint *p_h) {
   if (p_ctrl->i_orig_w <= 0 || p_ctrl->i_orig_h <= 0) {
      return (FALSE);
   }
   *p_w = p_ctrl->i_orig_w;
   *p_h = p_ctrl->i_orig_h;
   return (TRUE);
}

gboolean
enhance_ctrl_get_base_size(EnhanceCtrl *p_ctrl, gint *p_w, gint *p_h) {
   g_return_val_if_fail(p_ctrl != NULL && p_w != NULL && p_h != NULL, FALSE);
   gint i_ow, i_oh;
   if (!_has_navigator(p_ctrl) || !_orig_size(p_ctrl, &i_ow, &i_oh)) {
      return (FALSE);
   }
   gdouble d_w, d_h;
   transform_base_size(&p_ctrl->t_xf, i_ow, i_oh, &d_w, &d_h);
   *p_w = (gint)d_w;
   *p_h = (gint)d_h;
   return (TRUE);
}

void
enhance_ctrl_rotate_quarter(EnhanceCtrl *p_ctrl, gint i_dir) {
   g_return_if_fail(p_ctrl != NULL);
   if (!_has_navigator(p_ctrl) || p_ctrl->p_enhancer == NULL) {
      return;
   }
   Transform t_new = p_ctrl->t_xf;
   gint      i_bw  = 0;
   gint      i_bh  = 0;
   if (t_new.b_crop && !enhance_ctrl_get_base_size(p_ctrl, &i_bw, &i_bh)) {
      /* Cannot turn a crop without knowing the image it sits on; a crop
       * only exists once an apply landed, so this is theoretical -- drop
       * the crop rather than turn it into nonsense. */
      t_new.b_crop = FALSE;
   }
   transform_rotate_quarter(&t_new, i_dir, i_bw, i_bh);
   enhance_ctrl_set_transform(p_ctrl, &t_new);
}

gboolean
enhance_ctrl_get_orig_size(EnhanceCtrl *p_ctrl, gint *p_w, gint *p_h) {
   g_return_val_if_fail(p_ctrl != NULL && p_w != NULL && p_h != NULL, FALSE);
   return (_has_navigator(p_ctrl) && _orig_size(p_ctrl, p_w, p_h));
}

guint
enhance_ctrl_get_render_count(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, 0);
   return (p_ctrl->u_render_count);
}

guint
enhance_ctrl_get_managed_fetch_count(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, 0);
   return (p_ctrl->u_orig_fetches);
}

gboolean
enhance_ctrl_has_managed_original(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   return (p_ctrl->p_managed_orig != NULL);
}

guint
enhance_ctrl_get_preview_count(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, 0);
   return (p_ctrl->u_preview_count);
}

gboolean
enhance_ctrl_is_pending(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   return (p_ctrl->b_apply_pending);
}

/* The exact identity test the tools need (see the header). Two cases: with
 * render work the last landed texture IS the render (window._show_texture
 * puts that very object on screen through enhance_ctrl_override_texture);
 * without any, what the viewer should show is the current file's original,
 * the very object the window handed enhance_ctrl_texture_shown when it
 * showed it, so that identity is the test. Pure comparisons, no cache
 * lookup: this runs on every snapshot and pointer motion of a tool
 * overlay, and the cache's get stats the file and evicts a stale entry (an
 * original not shown since the last forget is simply never current). A
 * pending apply means the screen predates the state whatever it shows. */
gboolean
enhance_ctrl_is_current_render(EnhanceCtrl *p_ctrl, GdkTexture *p_tex) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   if (p_tex == NULL || p_ctrl->b_apply_pending || !_has_navigator(p_ctrl)) {
      return (FALSE);
   }
   if (_render_has_work(p_ctrl)) {
      return (p_tex == p_ctrl->p_enhance_tex);
   }
   return (p_tex == p_ctrl->p_orig_tex);
}

gboolean
enhance_ctrl_is_hold_original(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   return (p_ctrl->b_hold_original);
}

void
enhance_ctrl_discard(EnhanceCtrl *p_ctrl) {
   g_return_if_fail(p_ctrl != NULL);
   _discard(p_ctrl);
}

/* --- panel build / teardown ---------------------------------------------- */

/* Synchronously take the panel out of the host's slot and clear the
 * now-dangling widget pointers. Safe to call when none is open. Closing it
 * never touches u_enhance_mask -- the preview persists until explicitly
 * discarded. */
static void
_destroy(EnhanceCtrl *p_ctrl) {
   p_ctrl->u_preview_gen++;
   g_cancellable_cancel(p_ctrl->p_preview_cancel);
   g_clear_object(&p_ctrl->p_preview_cancel);
   if (p_ctrl->p_panel == NULL) {
      return;
   }
   GtkWidget *p_panel = p_ctrl->p_panel;
   p_ctrl->p_panel    = NULL;
   for (guint i = 0; i < G_N_ELEMENTS(p_ctrl->p_btns); i++) {
      p_ctrl->p_btns[i] = NULL;
      p_ctrl->p_pics[i] = NULL;
   }
   p_ctrl->p_original_pic = NULL;
   p_ctrl->p_state        = NULL;
   p_ctrl->p_save_btn     = NULL;
   p_ctrl->p_save_target  = NULL;
   GtkWidget *p_slot      = gtk_widget_get_parent(p_panel);
   if (GTK_IS_BOX(p_slot)) {
      gtk_box_remove(GTK_BOX(p_slot), p_panel);
   }
   _mode_changed(p_ctrl); /* the panel's keys and hint bar go with it */
}

typedef struct {
   gpointer     p_host; /* ref'd window */
   EnhanceCtrl *p_ctrl; /* borrowed, valid while p_host is alive */
   guint        u_gen;
} _PreviewCtx;

/* The thumbnail batch landed: p_tex[0] is the original, p_tex[1..] one
 * preset each. Dropped when stale (the panel closed, re-previewed for
 * another file, or the window went away meanwhile). */
static void
_preview_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   _PreviewCtx *p_ctx  = (_PreviewCtx *)p_data;
   GError      *p_err  = NULL;
   GPtrArray   *p_tex  = enhancer_preview_thumbnails_finish(p_res, &p_err);
   EnhanceCtrl *p_ctrl = p_ctx->p_ctrl;
   if (!_disposed(p_ctrl) && p_ctx->u_gen == p_ctrl->u_preview_gen &&
       p_ctrl->p_panel != NULL && p_tex != NULL && p_tex->len > 0) {
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
   }
   g_clear_pointer(&p_tex, g_ptr_array_unref);
   g_clear_error(&p_err);
   g_object_unref(p_ctx->p_host);
   g_free(p_ctx);
}

/* Start (or restart) the one cancellable thumbnail batch for the current
 * file. A no-op for label-only cards or when there is no current file.
 * Counted (u_preview_count) only when a batch really starts, so the seam
 * measures work launched, not calls made. */
static void
_start_previews(EnhanceCtrl *p_ctrl) {
   if (!p_ctrl->b_thumbnails) {
      return;
   }
   GFile *p_file = _current_file(p_ctrl);
   if (p_file == NULL) {
      return;
   }
   p_ctrl->u_preview_count++;
   p_ctrl->u_preview_gen++;
   g_cancellable_cancel(p_ctrl->p_preview_cancel);
   g_clear_object(&p_ctrl->p_preview_cancel);
   p_ctrl->p_preview_cancel = g_cancellable_new();
   _PreviewCtx *p_ctx       = g_new(_PreviewCtx, 1);
   p_ctx->p_host            = g_object_ref(p_ctrl->p_host);
   p_ctx->p_ctrl            = p_ctrl;
   p_ctx->u_gen             = p_ctrl->u_preview_gen;
   enhancer_preview_thumbnails_async(
      p_file, enhancer_get_presets(p_ctrl->p_enhancer),
      p_ctrl->p_preview_cancel, _preview_done_cb, p_ctx);
}

/* Build the panel and wire it into the controller's state. The pure widget
 * construction lives in enhance-ui.c (enhance_ui_build_panel); this thin
 * wrapper owns the controller-side glue -- storing the built widgets into
 * p_ctrl's fields and connecting each card's "clicked" to _card_toggle,
 * which owns the mask state. The Transform and Actions buttons need no
 * wiring: they are actionables on the win.* actions their keys fire. */
static GtkWidget *
_build_panel(EnhanceCtrl *p_ctrl) {
   EnhanceUIWidgets ui;
   GtkWidget       *p_panel =
      enhance_ui_build_panel(enhancer_get_presets(p_ctrl->p_enhancer),
                             p_ctrl->u_enhance_mask, p_ctrl->b_thumbnails, &ui);
   p_ctrl->p_original_pic = ui.p_original_pic;
   p_ctrl->p_state        = ui.p_state;
   p_ctrl->p_save_btn     = ui.p_save_btn;
   p_ctrl->p_save_target  = ui.p_save_target;
   for (guint i = 0; i < ui.u_n_presets; i++) {
      p_ctrl->p_btns[i] = ui.p_btns[i];
      p_ctrl->p_pics[i] = ui.p_pics[i];
   }
   for (guint i = 0; i < ui.u_n_presets; i++) {
      g_signal_connect_swapped(ui.p_btns[i], "clicked",
                               G_CALLBACK(_card_toggle), p_ctrl);
   }
   return (p_panel);
}

/* Point the open panel at the (new) current file: drop the previous file's
 * thumbnails so stale ones are never shown against a different image, name
 * the new save target, and start a fresh batch. Called after nav_changed
 * cleared the mask. */
static void
_retarget_panel(EnhanceCtrl *p_ctrl) {
   if (p_ctrl->p_original_pic != NULL) {
      gtk_picture_set_paintable(GTK_PICTURE(p_ctrl->p_original_pic), NULL);
   }
   for (guint i = 0; i < G_N_ELEMENTS(p_ctrl->p_pics); i++) {
      if (p_ctrl->p_pics[i] != NULL) {
         gtk_picture_set_paintable(GTK_PICTURE(p_ctrl->p_pics[i]), NULL);
      }
   }
   _sync_panel(p_ctrl);
   _sync_save_target(p_ctrl);
   _start_previews(p_ctrl);
}

/* --- public action entry points ------------------------------------------ */

/* `a`: open the side panel beside the large view (switching to it first: the
 * cards are about the image on screen), or close it if it is open. Card
 * clicks and hotkeys keep it open so several layered presets can be
 * compared. */
void
enhance_ctrl_toggle_open(EnhanceCtrl *p_ctrl, gboolean b_thumbnails) {
   g_return_if_fail(p_ctrl != NULL);
   if (!_has_navigator(p_ctrl) || p_ctrl->p_enhancer == NULL) {
      return;
   }
   if (p_ctrl->p_panel != NULL) {
      _destroy(p_ctrl);
      return;
   }
   p_ctrl->p_ops->ensure_large_view(p_ctrl->p_host);
   p_ctrl->b_thumbnails = b_thumbnails;
   p_ctrl->p_panel      = _build_panel(p_ctrl);
   gtk_box_append(GTK_BOX(p_ctrl->p_ops->panel_slot(p_ctrl->p_host)),
                  p_ctrl->p_panel);
   _sync_panel(p_ctrl); /* the save-state line has no build-time text */
   _sync_save_target(p_ctrl);
   _start_previews(p_ctrl);
   _mode_changed(p_ctrl); /* the digits are live now; the hint bar shows */
}

gboolean
enhance_ctrl_close(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   if (p_ctrl->p_panel == NULL) {
      return (FALSE);
   }
   _destroy(p_ctrl);
   return (TRUE);
}

/* enhance-N (keys 1-8, routed here only while the panel is open --
 * edit-mode.c; the action itself is also what a card click amounts to):
 * toggle preset N on/off (layered), then re-apply asynchronously.
 * Out-of-range i_idx is a silent no-op. */
void
enhance_ctrl_toggle_preset(EnhanceCtrl *p_ctrl, gint i_idx) {
   g_return_if_fail(p_ctrl != NULL);
   if (p_ctrl->p_enhancer == NULL || i_idx < 0 ||
       i_idx >= (gint)G_N_ELEMENTS(p_ctrl->p_btns)) {
      return;
   }
   p_ctrl->u_enhance_mask ^= (guint8)(1u << i_idx);
   _apply_async(p_ctrl);
}

/* --- choke points -------------------------------------------------------- */

/* A rescan of the SAME file: the folder monitor also fires when the current
 * picture is rewritten in place -- an external edit through `e`, a `!`
 * script -- possibly with another size, under the same identity, and
 * i_orig_w/h used to stay stale then (the crop tool laid its rectangle out
 * on the old size). The texture cache's stamp tells such a rescan from the
 * one a save's "-enhanced" copy causes, with no second stat: its get
 * re-checks the stamp (mtime to the nanosecond, size, inode -- so a
 * same-second rewrite to the same byte count is a rewrite too, fd2) and
 * evicts a stale entry, so the original still describes the file iff the
 * cache still hands it out. The stamp errs only toward "changed": where
 * the inode is not stable across queries (some FUSE mounts) every rescan
 * reads as a rewrite and costs a re-render of an active preview, never a
 * wrong picture; a same-size rewrite within one coarse mtime tick is the
 * one it misses (texturecache.h). Evicted (stale, or aged out of the LRU):
 * forget size and identity -- the reload that follows this signal decodes
 * the file as it is now and shows it, which is where it is learned again
 * -- and say so (TRUE), since a preview rendered from the old contents has
 * to be rendered again. Still cached but not the object known here (a
 * decode the viewer has not shown yet -- a neighbour's prefetch of this
 * very file that landed while its own visible load is still in flight):
 * take it, and tell the tool as the choke point does (_learn_original; a
 * crop tool used to stay laid out on the previous base until the visible
 * load showed its own decode). Either way a managed original fetched from
 * the previous contents goes (_forget_original drops it; _learn_original
 * drops it when it replaces a known decode), so hold-Space never compares
 * a render of the new contents against the old file's managed decode.
 * Unchanged: nothing to do, and a crop tool open over the file keeps its
 * rectangle. This is the one deliberate cache lookup left in this
 * controller, and it runs on a rescan only. */
static gboolean
_recheck_original(EnhanceCtrl *p_ctrl, GFile *p_cur) {
   GdkTexture *p_now = _cached_texture(p_ctrl, p_cur);
   if (p_now == NULL) {
      _forget_original(p_ctrl);
      return (TRUE);
   }
   if (p_now != p_ctrl->p_orig_tex) {
      _learn_original(p_ctrl, p_now);
   }
   return (FALSE);
}

/* The same file, rewritten (its cached original was evicted): a preview
 * rendered from the old contents is stale on screen -- it stayed there,
 * the override winning over the reload, until the next state change --
 * so render the current state from the file as it is now. Only when the
 * state actually renders anything: a crop tool open over a crop-only
 * transform shows the plain original, and the reload the window issues
 * right after this signal brings the new one (rendering here would only
 * issue that same reload twice). The "-enhanced" copy's rescan keeps a
 * fresh entry and never gets here. */
static void
_rerender_rewritten(EnhanceCtrl *p_ctrl) {
   if (_render_has_work(p_ctrl)) {
      _render(p_ctrl);
   }
}

/* "changed" fires for every navigator rescan, not only an actual move to a
 * different current file -- notably, enhance-save writes the "-enhanced" copy
 * into the SAME live-monitored folder, whose GFileMonitor then schedules a
 * debounced rescan that re-emits "changed" a few hundred ms later even though
 * navigator.current never moved. Discovered by tests/test_enhance_flow.c's
 * save-twice-in-a-row subtest: without this check, that incidental rescan
 * silently zeroed u_enhance_mask right after a successful save, discarding the
 * still-active preview the user was not done comparing/adjusting. Only reset
 * when the current file's IDENTITY actually changed; p_enhance_file is updated
 * unconditionally so the next call has an accurate baseline -- the window
 * calls this for an open too (window.c _open_rebuild), so a folder's first
 * rescan already compares against its current file rather than against
 * "nothing" (NULL used to read as an identity change and forget the
 * original under an open crop tool). A same-file
 * rescan is not a no-op either: the file may have been rewritten in place
 * (_recheck_original), and a preview of it is then rendered again
 * (_rerender_rewritten).
 *
 * The panel outlives the navigation: it is re-pointed at the new file (or
 * closed when the folder ran empty), so a whole folder can be worked through
 * with `a` pressed once. */
void
enhance_ctrl_nav_changed(EnhanceCtrl *p_ctrl) {
   g_return_if_fail(p_ctrl != NULL);
   GFile   *p_cur  = _current_file(p_ctrl);
   gboolean b_same = (p_cur != NULL && p_ctrl->p_enhance_file != NULL &&
                      g_file_equal(p_cur, p_ctrl->p_enhance_file));
   if (b_same) {
      if (_recheck_original(p_ctrl, p_cur)) {
         _rerender_rewritten(p_ctrl);
      }
   } else {
      p_ctrl->u_enhance_mask = 0;
      transform_init(&p_ctrl->t_xf);
      p_ctrl->b_preview = FALSE; /* a tool's override goes with it */
      _forget_original(p_ctrl);  /* another image, another size */
      p_ctrl->b_saved         = FALSE;
      p_ctrl->b_have_saved    = FALSE; /* the saved pair was this file's */
      p_ctrl->b_hint_shown    = FALSE;
      p_ctrl->b_hold_original = FALSE; /* mask cleared without going through
                                        * _render, so reset the hold flag
                                        * here too (issue 4) */
      _drop_inflight(p_ctrl);
      g_clear_object(&p_ctrl->p_enhance_tex);
      if (p_ctrl->p_panel != NULL && p_cur == NULL) {
         _destroy(p_ctrl); /* nothing left to enhance */
      } else if (p_ctrl->p_panel != NULL) {
         _retarget_panel(p_ctrl);
      }
   }
   g_set_object(&p_ctrl->p_enhance_file, p_cur);
}

/* --- internal: card toggle (clicked handler for the built cards) --------- */

/* Clicked card: idx 0..7 toggles that preset's bit, then re-apply the
 * (possibly empty) chain, which also refreshes the highlights. Does NOT
 * close the panel -- toggling presets while comparing is the point of the
 * layered design (docs/gegl.md). The Original is a reference, not a card:
 * reverting everything is x / the Revert button (6i2), never a stray click
 * on a thumbnail. */
static void
_card_toggle(EnhanceCtrl *p_ctrl, GtkWidget *p_btn) {
   gint i_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(p_btn), "idx"));
   if (i_idx >= 0 && i_idx < (gint)G_N_ELEMENTS(p_ctrl->p_btns)) {
      p_ctrl->u_enhance_mask ^= (guint8)(1u << i_idx);
   }
   _apply_async(p_ctrl);
}
