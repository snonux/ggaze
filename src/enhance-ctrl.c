/*:*
 * ggaze — Enhance/GEGL UI orchestration controller
 *
 * See enhance-ctrl.h. The preset mask, the in-flight apply/preview/export
 * requests, the cached enhanced texture, the hold-Space flag, the saved
 * flag, the undo history and the side panel live here; the window reaches it
 * through a few action entry points and it reaches the window through
 * EnhanceUIHostOps.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "enhance-ctrl.h"

#include <glib.h>
#include <gtk/gtk.h>
#include <string.h>

#include "edit-history.h"
#include "enhance-ui.h"
#include "enhancer-gegl.h"
#include "preset-strength.h"
#include "preview-scale.h"
#include "transform.h"

/* The snapshot keeps one strength per mask bit, and the mask is a
 * guint32 here, in the snapshot and in the enhancer's chain alike. */
G_STATIC_ASSERT(GGAZE_ENHANCE_MAX_PRESETS == EDIT_SNAPSHOT_PRESETS);
G_STATIC_ASSERT(GGAZE_ENHANCE_MAX_PRESETS <= 32);

/* How long a preview render may be pending before the "Rendering…"
 * indicator shows (8l2): a render of the scaled-down source usually lands
 * well within it, so the indicator only appears when there is something to
 * wait for -- the first source of a colour-managed 64 MP file, a Denoise --
 * and a quick render never flickers it. */
#define _BUSY_DELAY_MS 300

/* The run keys of strength steps (edit_history_push_run): h / l presses on
 * preset i coalesce under i, one drag of its slider under i plus this, so
 * a drag right after a run of presses (or the other way round) is a step
 * of its own. */
#define _SLIDER_RUN GGAZE_ENHANCE_MAX_PRESETS

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
static void     _render(EnhanceCtrl *p_ctrl);
static void     _discard(EnhanceCtrl *p_ctrl);
static void     _throw_away(EnhanceCtrl *p_ctrl);
static void     _destroy(EnhanceCtrl *p_ctrl);
static void     _start_previews(EnhanceCtrl *p_ctrl);
static void     _card_toggle(EnhanceCtrl *p_ctrl, GtkWidget *p_btn);
static void     _scale_changed(GtkRange *p_range, gpointer p_data);
static void     _scale_pressed(GtkGestureClick *p_click, gint i_n, gdouble d_x,
                               gdouble d_y, gpointer p_data);
static void     _sync_history(EnhanceCtrl *p_ctrl);
static gboolean _orig_size(EnhanceCtrl *p_ctrl, gint *p_w, gint *p_h);
static void     _drop_managed(EnhanceCtrl *p_ctrl);
static void     _drop_managed_orig(EnhanceCtrl *p_ctrl);
static void     _fetch_managed_original(EnhanceCtrl *p_ctrl);
static void     _drop_source(EnhanceCtrl *p_ctrl);
static void     _reset_source(EnhanceCtrl *p_ctrl);
static void     _build_source(EnhanceCtrl *p_ctrl, GFile *p_file);
static void     _maybe_start_thumbs(EnhanceCtrl *p_ctrl);
static void     _cancel_thumbs(EnhanceCtrl *p_ctrl);
static void     _sync_busy(EnhanceCtrl *p_ctrl);

/* One preview render's request (_launch): see its definition below. */
typedef struct ApplyReq _Req;
static void             _req_free(_Req *p_req);

/* --- struct --------------------------------------------------------------- */

struct EnhanceCtrl {
   const EnhanceUIHostOps *p_ops;  /* borrowed, for the controller's lifetime */
   gpointer                p_host; /* the window, borrowed */

   Enhancer *p_enhancer;     /* GEGL preset engine (always non-NULL) */
   guint32   u_enhance_mask; /* bit i -> preset row i enabled (layered;
                              * GGAZE_ENHANCE_BIT) */
   gdouble d_strength
      [GGAZE_ENHANCE_MAX_PRESETS]; /* preset i's tunable
                                    * number (8i2), canonical; 0 for a preset
                                    * without one. Part of the edit state like
                                    * the mask: rendered (resolved into the
                                    * preset graphs), exported, snapshotted for
                                    * undo, compared for saved; back to each
                                    * preset's default on another image and on
                                    * a discard */
   gint i_selected;     /* the panel's selected preset row (j / k move
                         * it; Enter, h / l and the slider act on it;
                         * a digit or a card click selects its row).
                         * UI state, not an edit: it survives
                         * navigation and is never undone */
   gboolean b_syncing;  /* _sync_panel is moving the sliders: their
                         * value-changed is not the user's */
   Transform t_xf;      /* the COMMITTED rotate 90 / straighten / crop,
                         * applied after the presets in the same graph
                         * (decision #35); the identity when none. What
                         * `s` exports and dirty/active are judged on */
   Transform t_preview; /* a tool's rendering override (see the header:
                         * the crop tool shows the base) ... */
   gboolean b_preview;  /* ... in effect iff this is set */
   gint     i_orig_w;   /* the ORIGINAL's upright size, from the
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
   gboolean b_have_saved;    /* an export succeeded for this file ... */
   guint32  u_saved_mask;    /* ... with this mask ... */
   gdouble  d_saved_strength[GGAZE_ENHANCE_MAX_PRESETS]; /* ... these
                                                          * strengths ... */
   Transform t_saved_xf;   /* ... and this transform */
   char     *c_saved_name; /* basename of that export, for the panel */
   guint     u_list_gen;   /* bumped whenever a Preferences change moved
                            * the preset rows (_carry_edit): an export
                            * launched before it recorded its mask in the
                            * old rows, so it must not become the saved
                            * state (_save_done_cb) */
   gboolean b_hint_shown;  /* the "Space compares / s saves / a shows the
                            * presets" status line was shown for this file
                            * (it is shown once per file, and only when a
                            * preset is applied with the panel closed) */
   EditHistory *p_history; /* the edit steps of THIS image (7i2): cleared
                            * on another file and on every discard but
                            * x's own (always non-NULL) */
   Transform t_step_base;  /* a tool's starting transform ... */
   gboolean  b_step_base;  /* ... which a step taken under the tool
                            * records in place of t_xf, iff set
                            * (enhance_ctrl_set_step_base) */

   /* The side panel and the widgets in it that change after the build. All
    * NULL while closed (_destroy clears them), so every sync helper can run
    * unconditionally. */
   gboolean   b_thumbnails; /* the open panel has picture cards */
   GtkWidget *p_panel;      /* the panel root, parented in the host's slot */
   GtkWidget *p_original_pic;
   GtkWidget *p_scroll; /* the preset list's scroller (ai2: every row) */
   GtkWidget *p_rows[GGAZE_ENHANCE_MAX_PRESETS];       /* card + slider rows */
   GtkWidget *p_btns[GGAZE_ENHANCE_MAX_PRESETS];       /* preset cards */
   GtkWidget *p_pics[GGAZE_ENHANCE_MAX_PRESETS];       /* their pictures */
   GtkWidget *p_scales[GGAZE_ENHANCE_MAX_PRESETS];     /* strength sliders
                                                        * (NULL: not tunable) */
   GtkWidget     *p_values[GGAZE_ENHANCE_MAX_PRESETS]; /* strength labels */
   GtkWidget     *p_state;                             /* save-state line */
   GtkWidget     *p_save_btn;
   GtkWidget     *p_save_target;  /* "as <name>" the next Save writes */
   GtkWidget     *p_undo_btn;     /* insensitive with nothing to undo ... */
   GtkWidget     *p_redo_btn;     /* ... or to redo */
   GdkFrameClock *p_scroll_clock; /* a scroll of the selected row into view
                                   * waits for this clock's next layout
                                   * (owned ref; NULL = none pending) ... */
   gulong u_scroll_handler;       /* ... on this "layout" handler */

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

   /* The live preview's SOURCE (8l2, decision #53): the current image
    * decoded once and scaled down to what the view shows
    * (enhancer_source_new_async), which every render and the card
    * thumbnails run on; the export alone decodes at full resolution. */
   EnhancerSource *p_source;     /* the landed one (owned; NULL = none) */
   GCancellable   *p_src_cancel; /* the build in flight (NULL = none) ... */
   GFile          *p_build_file; /* ... of this file (owned) */
   guint           u_source_gen; /* bumped by _drop_source: a build that
                                  * lands under an older value is stale */
   guint u_source_count;         /* builds launched (a test seam) */
   gint  i_max_side;             /* test seam: a cap on the source's long
                                  * side (0: none), so a small fixture
                                  * gets a scaled preview too */
   _Req *p_wait_req;             /* a render launched while no source
                                  * served it: it starts when the build
                                  * lands (it counts as pending) */
   gboolean b_thumbs_wanted;     /* a card batch waits for the source and
                                  * for the preview, which goes first */
   gboolean b_thumbs_running;    /* a card batch is in flight */
   guint    u_thumb_launches;    /* card batches really started (a test
                                  * seam: they wait for the preview) */
   guint    u_busy_id;           /* the _BUSY_DELAY_MS timer (0: none) */
   gboolean b_busy;              /* the "Rendering…" indicator is up */
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
   p_ctrl->p_history        = edit_history_new(0);
   transform_init(&p_ctrl->t_xf);
   enhancer_default_strengths(enhancer_get_presets(p_ctrl->p_enhancer),
                              p_ctrl->d_strength);
   return (p_ctrl);
}

/* Every preset back at its default strength (another image, a discard,
 * a new preset list). */
static void
_reset_strengths(EnhanceCtrl *p_ctrl) {
   enhancer_default_strengths(p_ctrl->p_enhancer != NULL
                                 ? enhancer_get_presets(p_ctrl->p_enhancer)
                                 : NULL,
                              p_ctrl->d_strength);
}

/* TRUE iff two strength sets agree on every preset enabled in u_mask
 * (canonical values: exact). A disabled preset's strength renders nothing,
 * so it cannot make a state differ from what was saved. */
static gboolean
_strengths_equal(const gdouble *pd_a, const gdouble *pd_b, guint32 u_mask) {
   for (guint u = 0; u < GGAZE_ENHANCE_MAX_PRESETS; u++) {
      if ((u_mask & GGAZE_ENHANCE_BIT(u)) != 0 && pd_a[u] != pd_b[u]) {
         return (FALSE);
      }
   }
   return (TRUE);
}

/* Preset i_idx of the engine's list (borrowed), or NULL when there is no
 * such addressable preset. */
static const EnhancerPreset *
_preset_at(EnhanceCtrl *p_ctrl, gint i_idx) {
   const GPtrArray *p_presets = p_ctrl->p_enhancer != NULL
                                   ? enhancer_get_presets(p_ctrl->p_enhancer)
                                   : NULL;
   if (p_presets == NULL || i_idx < 0 || (guint)i_idx >= p_presets->len ||
       i_idx >= GGAZE_ENHANCE_MAX_PRESETS) {
      return (NULL);
   }
   return (g_ptr_array_index((GPtrArray *)p_presets, (guint)i_idx));
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

/* Saved iff the committed state renders exactly what the last export
 * wrote (see the header): the same mask, the same strengths on the presets
 * that mask enables (a strength tuned on a preset that is off changes no
 * pixel), the same transform. Re-derived after every state change rather
 * than cleared by it, so coming back to the saved state counts as saved.
 * (The undo history's no-op test, edit_snapshot_equal, still compares
 * every strength: a step that only moved a disabled preset's strength is
 * a step the card shows.) */
static void
_refresh_saved(EnhanceCtrl *p_ctrl) {
   p_ctrl->b_saved =
      p_ctrl->b_have_saved && p_ctrl->u_enhance_mask == p_ctrl->u_saved_mask &&
      _strengths_equal(p_ctrl->d_strength, p_ctrl->d_saved_strength,
                       p_ctrl->u_enhance_mask) &&
      transform_equal(&p_ctrl->t_xf, &p_ctrl->t_saved_xf);
}

/* The edit state an undo would come back to: the mask, the strengths and
 * the committed transform -- or, under a tool, the transform the tool
 * started from (t_step_base), since the straighten tool commits every
 * nudge only to preview it. */
static void
_snapshot(EnhanceCtrl *p_ctrl, EditSnapshot *p_out) {
   edit_snapshot_init(p_out);
   p_out->u_mask = p_ctrl->u_enhance_mask;
   for (guint u = 0; u < GGAZE_ENHANCE_MAX_PRESETS; u++) {
      p_out->d_strength[u] = p_ctrl->d_strength[u];
   }
   p_out->t_xf = p_ctrl->b_step_base ? p_ctrl->t_step_base : p_ctrl->t_xf;
}

/* Record the step from *p_before to the state now (a no-op step is
 * dropped by the history) and bring the panel's Undo / Redo in line. */
static void
_push_step(EnhanceCtrl *p_ctrl, EditStepKind e_kind, const char *c_label,
           const EditSnapshot *p_before) {
   EditSnapshot t_after;
   _snapshot(p_ctrl, &t_after);
   edit_history_push(p_ctrl->p_history, e_kind, c_label, p_before, &t_after);
   _sync_history(p_ctrl);
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
   g_clear_object(&p_ctrl->p_src_cancel);
   g_clear_object(&p_ctrl->p_build_file);
   g_clear_pointer(&p_ctrl->p_source, enhancer_source_delete);
   g_clear_pointer(&p_ctrl->p_wait_req, _req_free);
   g_clear_handle_id(&p_ctrl->u_busy_id, g_source_remove);
   g_clear_pointer(&p_ctrl->p_enhancer, enhancer_delete);
   g_clear_pointer(&p_ctrl->p_history, edit_history_delete);
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
   _drop_source(p_ctrl);  /* and a source build */
   g_clear_pointer(&p_ctrl->p_wait_req, _req_free);
   g_clear_handle_id(&p_ctrl->u_busy_id, g_source_remove);
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

/* The old and new preset lists and where each old row went (enhancer_
 * presets_map): what every state the controller keeps is carried through
 * when Preferences changes the user presets. */
typedef struct {
   const GPtrArray *p_old;
   const GPtrArray *p_new;
   const gint      *pi_map;
} _PresetMove;

/* One (mask, strengths) state carried to the new list in place. */
static guint32
_move_state(const _PresetMove *p_mv, guint32 u_mask, gdouble *pd_strength) {
   gdouble d_new[GGAZE_ENHANCE_MAX_PRESETS];
   guint32 u_out = enhancer_state_remap(p_mv->p_old, p_mv->p_new, p_mv->pi_map,
                                        u_mask, pd_strength, d_new);
   memcpy(pd_strength, d_new, sizeof(d_new));
   return (u_out);
}

/* edit_history_remap's callback: a snapshot of the history, carried. */
static void
_move_snapshot(EditSnapshot *p_s, gpointer p_data) {
   p_s->u_mask = _move_state(p_data, p_s->u_mask, p_s->d_strength);
}

/* The saved state, carried -- and forgotten when what it renders changed
 * with the list (a preset it had on was removed, or its graph edited):
 * the copy on disk is no longer a state the panel can come back to, so
 * nothing may call the screen "saved" by it. */
static void
_move_saved(EnhanceCtrl *p_ctrl, const _PresetMove *p_mv) {
   if (!p_ctrl->b_have_saved) {
      return;
   }
   char *c_was = enhancer_chain_key(p_mv->p_old, p_ctrl->u_saved_mask,
                                    p_ctrl->d_saved_strength);
   p_ctrl->u_saved_mask =
      _move_state(p_mv, p_ctrl->u_saved_mask, p_ctrl->d_saved_strength);
   char *c_now = enhancer_chain_key(p_mv->p_new, p_ctrl->u_saved_mask,
                                    p_ctrl->d_saved_strength);
   if (g_strcmp0(c_was, c_now) != 0) {
      p_ctrl->b_have_saved = FALSE;
      g_clear_pointer(&p_ctrl->c_saved_name, g_free);
   }
   g_free(c_was);
   g_free(c_now);
}

/* The selection follows its preset; a removed one's row goes to the row
 * that took its place (the last, when it was the last). */
static void
_move_selection(EnhanceCtrl *p_ctrl, const _PresetMove *p_mv) {
   gint i_sel = p_ctrl->i_selected;
   gint i_n   = (gint)MIN(p_mv->p_new->len, (guint)GGAZE_ENHANCE_MAX_PRESETS);
   if (i_sel >= 0 && (guint)i_sel < p_mv->p_old->len &&
       i_sel < GGAZE_ENHANCE_MAX_PRESETS && p_mv->pi_map[i_sel] >= 0) {
      i_sel = p_mv->pi_map[i_sel];
   }
   p_ctrl->i_selected = CLAMP(i_sel, 0, MAX(i_n - 1, 0));
}

static void _rebuild_panel(EnhanceCtrl *p_ctrl);

/* Carry the whole edit across a list that changed (see
 * enhance_ctrl_set_user_presets): the state on screen, the saved one, every
 * snapshot undo holds, the selection; then the panel is built again for
 * the new rows, and the render runs again only if the chain it runs
 * changed. */
static void
_carry_edit(EnhanceCtrl *p_ctrl, const _PresetMove *p_mv) {
   char *c_was = enhancer_chain_key(p_mv->p_old, p_ctrl->u_enhance_mask,
                                    p_ctrl->d_strength);
   p_ctrl->u_enhance_mask =
      _move_state(p_mv, p_ctrl->u_enhance_mask, p_ctrl->d_strength);
   char *c_now = enhancer_chain_key(p_mv->p_new, p_ctrl->u_enhance_mask,
                                    p_ctrl->d_strength);
   _move_saved(p_ctrl, p_mv);
   p_ctrl->u_list_gen++; /* an export in flight is in the old rows */
   edit_history_remap(p_ctrl->p_history, _move_snapshot, (gpointer)p_mv);
   _move_selection(p_ctrl, p_mv);
   _refresh_saved(p_ctrl);
   _rebuild_panel(p_ctrl);
   if (g_strcmp0(c_was, c_now) != 0) {
      _render(p_ctrl); /* not _apply_async: Preferences switches no view */
   }
   _update_header(p_ctrl);
   g_free(c_was);
   g_free(c_now);
}

/* A new list arrives on EVERY Preferences change (the window reloads the
 * engine lists whatever key moved). The same list changes nothing -- the
 * common case, and what keeps a tuned strength (8i2 review: resetting the
 * strengths here threw a tuned value away behind the card, the title and
 * the render, and `s` then exported the reset one). A changed list is one
 * Preferences edit of the user presets (ai2): the edit refers to presets
 * by row, so it is carried to the rows they have now rather than left on
 * the old row numbers -- a reorder must not turn on whichever preset slid
 * into an enabled row. Each preset keeps its on / off and strength
 * (enhancer_presets_map says which is which: the same name and graph,
 * else the same name -- its graph edited, the strength clamped into its
 * new range -- else the same graph -- renamed); a removed one is dropped.
 * The undo history is carried the same way rather than cleared, so the
 * steps taken before the Preferences change still undo -- a step that
 * only toggled a removed preset goes with it. The picture is rendered
 * again only when the chain changed (an enabled preset removed or edited,
 * two enabled ones reordered); a saved copy stays saved exactly when it
 * still renders the same. */
void
enhance_ctrl_set_user_presets(EnhanceCtrl *p_ctrl, const GPtrArray *p_pairs) {
   g_return_if_fail(p_ctrl != NULL);
   if (p_ctrl->p_enhancer == NULL) {
      return;
   }
   GPtrArray *p_old =
      g_ptr_array_ref((GPtrArray *)enhancer_get_presets(p_ctrl->p_enhancer));
   enhancer_set_user_presets(p_ctrl->p_enhancer, p_pairs);
   gint        i_map[GGAZE_ENHANCE_MAX_PRESETS];
   _PresetMove t_mv = {p_old, enhancer_get_presets(p_ctrl->p_enhancer), i_map};
   if (!enhancer_presets_map(t_mv.p_old, t_mv.p_new, i_map)) {
      _carry_edit(p_ctrl, &t_mv);
   }
   g_ptr_array_unref(p_old);
}

const GPtrArray *
enhance_ctrl_get_presets(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, NULL);
   return (p_ctrl->p_enhancer != NULL ? enhancer_get_presets(p_ctrl->p_enhancer)
                                      : NULL);
}

guint32
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

/* What hold-Space compares a SCALED preview against (8l2): the preview's
 * own source shown as it is -- the same resolution and the same decode
 * (colour-managed or not) as the render on screen, so only the presets
 * differ and no sharpness step appears between the two. NULL when the
 * source is at full size (the original on screen, or the managed
 * original, is the like-with-like compare then) or belongs to another
 * file. The texture is made on the first ask -- a Space press -- not with
 * the source: ~2 ms for a 64 MP photo's source, and a session that never
 * compares never pays for it (8l2 review). */
static GdkTexture *
_compare_original(EnhanceCtrl *p_ctrl) {
   if (p_ctrl->p_source == NULL || p_ctrl->p_enhance_file == NULL ||
       !g_file_equal(enhancer_source_get_file(p_ctrl->p_source),
                     p_ctrl->p_enhance_file)) {
      return (NULL);
   }
   return (enhancer_source_get_original(p_ctrl->p_source));
}

/* Under hold-Space the compare original (_compare_original, else the
 * managed original once fetched) stands for the original: it is what the
 * screen shows then, so the window's texture choke point keeps it up and
 * the info card (window.c _info_texture_for) plots it as the current
 * file's -- without this the histogram went blank whenever Space showed a
 * managed original. */
GdkTexture *
enhance_ctrl_override_texture(EnhanceCtrl *p_ctrl, GdkTexture *p_tex) {
   if (p_ctrl == NULL || !_has_work(p_ctrl) || p_ctrl->p_enhance_tex == NULL) {
      return (p_tex);
   }
   if (p_ctrl->b_hold_original) {
      GdkTexture *p_cmp = _compare_original(p_ctrl);
      if (p_cmp != NULL) {
         return (p_cmp);
      }
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

/* The panel's card batch waits for a source that nobody is building:
 * _start_previews builds none while the file's decode is still on its way
 * to the viewer (it would decode the file a second time next to it), so
 * the decode's arrival starts it -- from that decode. */
static void
_build_for_waiting_cards(EnhanceCtrl *p_ctrl) {
   GFile *p_cur = _current_file(p_ctrl);
   if (p_ctrl->b_thumbs_wanted && p_ctrl->p_source == NULL &&
       p_ctrl->p_src_cancel == NULL && p_cur != NULL) {
      _build_source(p_ctrl, p_cur);
   }
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
   gboolean b_replaced =
      p_ctrl->p_orig_tex != NULL && p_ctrl->p_orig_tex != p_tex;
   if (b_replaced) {
      /* A NEW decode of the file replaces a known one: the file may have
       * been rewritten since the managed original was fetched from it, so
       * that one (or its fetch in flight) goes too. b_managed stays: it is
       * the last render's, and the next Space press fetches again from the
       * file as it is now (a rewrite into an unmanaged file comes back
       * NULL, which clears it). A same-content reload costs a re-fetch. */
      _drop_managed_orig(p_ctrl);
   }
   g_set_object(&p_ctrl->p_orig_tex, p_tex);
   if (b_replaced) {
      /* The preview source was built from the previous decode: build it
       * again -- from this one -- when a render waits for it, else when
       * one next needs it (8l2). */
      _reset_source(p_ctrl);
   }
   _note_orig_size(p_ctrl, gdk_texture_get_width(p_tex),
                   gdk_texture_get_height(p_tex));
   p_ctrl->p_ops->original_changed(p_ctrl->p_host);
   _build_for_waiting_cards(p_ctrl);
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
      /* A scaled preview compares against its own source
       * (_compare_original: same resolution, same decode). At full size, a
       * colour-managed file compares against its managed original (the
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
      GdkTexture *p_cmp  = _compare_original(p_ctrl);
      GdkTexture *p_orig = p_ctrl->p_managed_orig != NULL
                              ? p_ctrl->p_managed_orig
                              : p_ctrl->p_orig_tex;
      if (p_cmp != NULL) {
         p_orig = p_cmp; /* a scaled preview: its own source (8l2) */
      }
      if (p_orig != NULL) {
         _show_texture(p_ctrl, p_orig);
      }
      if (p_cmp == NULL) {
         _fetch_managed_original(p_ctrl);
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
 * strengths, transform) state the export writes (so the completion records
 * exactly what is on disk, whatever the state did meanwhile), the caller's
 * continuation, and a ref on the host so the completion can run after a dispose
 * without dangling (it then only releases). */
typedef struct {
   gpointer          p_host; /* ref'd window */
   EnhanceCtrl      *p_ctrl; /* borrowed, valid while p_host is alive */
   GFile            *p_out;  /* owned */
   guint32           u_mask;
   gdouble           d_strength[GGAZE_ENHANCE_MAX_PRESETS];
   Transform         t_xf;
   EnhanceSaveDoneFn fn_done;
   gpointer          p_done_data;
   guint             u_list_gen; /* the preset rows u_mask is in */
} _SaveReq;

/* A finished export records the pair it wrote as the saved one; the preview
 * is then saved iff the state still is (or comes back to) that pair -- a
 * mask that moved on while the worker ran stays dirty, as before, but is
 * saved again the moment it is toggled back. An export launched before a
 * Preferences change moved the preset rows (ai2) wrote a state named in
 * the old rows: it is reported but not recorded, so the screen stays
 * unsaved (a prompt too many, never a lost edit). */
static void
_save_done_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   _SaveReq    *p_req  = (_SaveReq *)p_data;
   EnhanceCtrl *p_ctrl = p_req->p_ctrl;
   GError      *p_err  = NULL;
   gboolean     b_ok   = enhancer_export_chain_finish(p_res, &p_err);
   if (!_disposed(p_ctrl)) {
      _save_report(p_ctrl, p_req->p_out, b_ok, p_err);
      if (b_ok && p_req->u_list_gen == p_ctrl->u_list_gen) {
         p_ctrl->b_have_saved = TRUE;
         p_ctrl->u_saved_mask = p_req->u_mask;
         p_ctrl->t_saved_xf   = p_req->t_xf;
         memcpy(p_ctrl->d_saved_strength, p_req->d_strength,
                sizeof(p_ctrl->d_saved_strength));
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
   /* The value written is a point undo must be able to stop at: close
    * the strength run, so l after the save is a step of its own rather
    * than one that undoes past the saved value. */
   edit_history_end_run(p_ctrl->p_history);
   g_cancellable_cancel(p_ctrl->p_save_cancel);
   g_clear_object(&p_ctrl->p_save_cancel);
   p_ctrl->p_save_cancel = g_cancellable_new();
   _SaveReq *p_req       = g_new0(_SaveReq, 1);
   p_req->p_host         = g_object_ref(p_ctrl->p_host);
   p_req->p_ctrl         = p_ctrl;
   p_req->p_out          = p_out;
   p_req->u_mask         = p_ctrl->u_enhance_mask;
   p_req->t_xf           = p_ctrl->t_xf;
   memcpy(p_req->d_strength, p_ctrl->d_strength, sizeof(p_req->d_strength));
   p_req->u_list_gen  = p_ctrl->u_list_gen;
   p_req->fn_done     = fn_done;
   p_req->p_done_data = p_done_data;
   char *c_name       = g_file_get_basename(p_out);
   char *c_msg        = g_strdup_printf("Saving %s…", c_name);
   _show_status(p_ctrl, c_msg);
   g_free(c_msg);
   g_free(c_name);
   /* The strengths go in as resolved graphs; the export snapshots the
    * list, so it is released right away. */
   GPtrArray *p_resolved = enhancer_presets_resolve(
      enhancer_get_presets(p_ctrl->p_enhancer), p_ctrl->d_strength);
   enhancer_export_chain_async(p_ctrl->p_enhance_file, p_resolved,
                               p_ctrl->u_enhance_mask, &p_ctrl->t_xf, p_out,
                               p_ctrl->p_save_cancel, _save_done_cb, p_req);
   g_ptr_array_unref(p_resolved);
}

/* --- panel sync ----------------------------------------------------------- */

/* p_widget has c_class iff b_on. */
static void
_set_class(GtkWidget *p_widget, const char *c_class, gboolean b_on) {
   if (b_on) {
      gtk_widget_add_css_class(p_widget, c_class);
   } else {
      gtk_widget_remove_css_class(p_widget, c_class);
   }
}

/* Card i in line with the state: on / off, selected, and -- for a
 * tunable preset -- its strength on the label and the slider, which shows
 * under the selected card alone. The slider moves under b_syncing, so its
 * value-changed does not read as the user's. */
static void
_sync_card(EnhanceCtrl *p_ctrl, guint i) {
   GtkWidget *p_btn = p_ctrl->p_btns[i];
   if (p_btn == NULL) {
      return;
   }
   gboolean b_selected = (gint)i == p_ctrl->i_selected;
   _set_class(p_btn, "ggaze-enhance-on",
              (p_ctrl->u_enhance_mask & GGAZE_ENHANCE_BIT(i)) != 0);
   _set_class(p_btn, GGAZE_ENHANCE_SELECTED_CLASS, b_selected);
   const EnhancerPreset *p_pr = _preset_at(p_ctrl, (gint)i);
   if (p_pr == NULL || !p_pr->b_tunable) {
      return;
   }
   p_ctrl->b_syncing = TRUE;
   enhance_ui_set_strength(p_ctrl->p_values[i], p_ctrl->p_scales[i],
                           &p_pr->t_strength, p_ctrl->d_strength[i]);
   p_ctrl->b_syncing = FALSE;
   if (p_ctrl->p_scales[i] != NULL) {
      gtk_widget_set_visible(p_ctrl->p_scales[i], b_selected);
   }
}

/* Bring the open panel in line with the state: each preset card
 * (_sync_card: its highlight from the mask, the selection ring, the
 * strength), and the save-state line + Save button from active/saved. A no-op
 * while the panel is closed (every widget pointer is NULL then), so callers
 * never check p_panel first. */
static void
_sync_panel(EnhanceCtrl *p_ctrl) {
   for (guint i = 0; i < G_N_ELEMENTS(p_ctrl->p_btns); i++) {
      _sync_card(p_ctrl, i);
   }
   if (p_ctrl->p_state != NULL) {
      enhance_ui_set_save_state(p_ctrl->p_state, p_ctrl->p_save_btn,
                                _has_work(p_ctrl), p_ctrl->b_saved,
                                p_ctrl->c_saved_name, p_ctrl->b_busy);
   }
   _sync_history(p_ctrl);
}

/* The Undo / Redo buttons are sensitive only while there is something to
 * undo / redo. A no-op while the panel is closed. */
static void
_sync_history(EnhanceCtrl *p_ctrl) {
   if (p_ctrl->p_undo_btn != NULL) {
      gtk_widget_set_sensitive(p_ctrl->p_undo_btn,
                               edit_history_can_undo(p_ctrl->p_history));
   }
   if (p_ctrl->p_redo_btn != NULL) {
      gtk_widget_set_sensitive(p_ctrl->p_redo_btn,
                               edit_history_can_redo(p_ctrl->p_history));
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
 * preset was applied, and this file has not had the hint yet). What the
 * source says about the original -- its size, whether its decode was
 * colour-managed -- is parked here when the render starts
 * (_launch_render) until _apply_landed takes it. */
struct ApplyReq {
   gpointer     p_host; /* ref'd window */
   EnhanceCtrl *p_ctrl; /* borrowed, valid while p_host is alive */
   guint        u_gen;
   gboolean     b_hint;
   gboolean     b_managed; /* the source's decode was colour-managed */
   gint         i_orig_w;  /* the original's upright size (the source's) */
   gint         i_orig_h;
};

static void
_req_free(_Req *p_req) {
   if (p_req == NULL) {
      return;
   }
   g_object_unref(p_req->p_host);
   g_free(p_req);
}

/* A render landed and is still wanted: show it. The source it ran on knows
 * the original's size, so it is known here even before the viewer has
 * shown a decode of it -- after a rewrite in place the render may land BEFORE
 * the reload's decode, and a crop tool laid out on the old size used to draw
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
   GdkTexture  *p_tex  = enhancer_source_render_finish(p_res, &p_err);
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
      _throw_away(p_ctrl); /* back to the original; also bumps the gen and
                            * drops a queued relaunch (it would fail too) */
   } else {
      _apply_landed(p_ctrl, p_req, p_tex, p_req->i_orig_w, p_req->i_orig_h);
      g_object_unref(p_tex);
   }
   if (p_ctrl->b_relaunch) {
      p_ctrl->b_relaunch = FALSE;
      _render(p_ctrl); /* the state moved on meanwhile: once more, latest */
   }
   _sync_busy(p_ctrl);          /* still pending after a relaunch: kept up */
   _maybe_start_thumbs(p_ctrl); /* the cards come after the preview */
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
   g_clear_pointer(&p_ctrl->p_wait_req, _req_free); /* the build goes on */
   g_cancellable_cancel(p_ctrl->p_enhance_cancel);
   g_clear_object(&p_ctrl->p_enhance_cancel);
   p_ctrl->p_enhance_cancel = g_cancellable_new();
   _sync_busy(p_ctrl);
}

/* The viewport the preview is for (the host's large view) with the test
 * seam's cap. */
static void
_view(EnhanceCtrl *p_ctrl, PreviewView *p_out) {
   memset(p_out, 0, sizeof(*p_out));
   p_ctrl->p_ops->get_view(p_ctrl->p_host, &p_out->i_w, &p_out->i_h,
                           &p_out->i_device_scale);
   p_out->i_max_side = p_ctrl->i_max_side;
}

/* TRUE iff the landed source is p_file's and fine enough for the view. */
static gboolean
_source_ready(EnhanceCtrl *p_ctrl, GFile *p_file) {
   PreviewView t_view;
   _view(p_ctrl, &t_view);
   return (p_ctrl->p_source != NULL &&
           enhancer_source_serves(p_ctrl->p_source, p_file, &t_view));
}

/* Start the render of p_req on the landed source with the state as of
 * NOW: the strengths resolved into the graphs (a nudge after this is the
 * next render's, the coalescing slot) and the RENDER transform (a tool's
 * override, else the committed one). The worker snapshots the list, so it
 * is released right away. */
static void
_launch_render(EnhanceCtrl *p_ctrl, _Req *p_req) {
   p_req->b_managed = enhancer_source_is_managed(p_ctrl->p_source);
   enhancer_source_get_orig_size(p_ctrl->p_source, &p_req->i_orig_w,
                                 &p_req->i_orig_h);
   GPtrArray *p_presets = enhancer_presets_resolve(
      enhancer_get_presets(p_ctrl->p_enhancer), p_ctrl->d_strength);
   enhancer_source_render_async(
      p_ctrl->p_source, p_presets, p_ctrl->u_enhance_mask,
      _render_transform(p_ctrl), p_ctrl->p_enhance_cancel, _apply_done_cb,
      p_req);
   g_ptr_array_unref(p_presets);
}

/* A card batch in flight yields to the preview (8l2): it is cancelled --
 * the worker stops between presets -- and started again once the preview
 * has landed (_maybe_start_thumbs). */
static void
_pause_thumbs(EnhanceCtrl *p_ctrl) {
   if (p_ctrl->b_thumbs_running) {
      _cancel_thumbs(p_ctrl);
      p_ctrl->b_thumbs_wanted = TRUE;
   }
}

/* Launch the async apply for the has-render-work case: record which file
 * the preview applies to (see nav_changed's comment for why this is set
 * here, not only in nav_changed) and render it on the preview source --
 * at once when the landed source serves the view, else as soon as the
 * source being built for it lands (p_wait_req; it counts as pending, so
 * the state changes meanwhile coalesce exactly as during a render). */
static void
_launch(EnhanceCtrl *p_ctrl, GFile *p_file) {
   if (p_ctrl->p_enhance_file == NULL ||
       !g_file_equal(p_ctrl->p_enhance_file, p_file)) {
      _drop_managed(p_ctrl); /* another file's */
   }
   g_set_object(&p_ctrl->p_enhance_file, p_file);
   _Req *p_req             = g_new0(_Req, 1);
   p_req->p_host           = g_object_ref(p_ctrl->p_host);
   p_req->p_ctrl           = p_ctrl;
   p_req->u_gen            = p_ctrl->u_enhance_gen;
   p_req->b_hint           = p_ctrl->p_panel == NULL && !p_ctrl->b_hint_shown;
   p_ctrl->b_hint_shown    = p_ctrl->b_hint_shown || p_req->b_hint;
   p_ctrl->b_apply_pending = TRUE;
   p_ctrl->u_render_count++;
   _pause_thumbs(p_ctrl);
   if (_source_ready(p_ctrl, p_file)) {
      _launch_render(p_ctrl, p_req);
   } else {
      p_ctrl->p_wait_req = p_req;
      _build_source(p_ctrl, p_file);
   }
   _sync_busy(p_ctrl);
}

/* --- the preview source (8l2) --------------------------------------------- */

/* A build's context: the host ref keeps the borrowed controller alive (as
 * _Req does), u_gen says whether it is still wanted. */
typedef struct {
   gpointer     p_host; /* ref'd window */
   EnhanceCtrl *p_ctrl; /* borrowed, valid while p_host is alive */
   guint        u_gen;
} _SrcReq;

/* Forget the source and cancel a build in flight (its landing is stale by
 * u_source_gen). A render waiting for it keeps waiting: _reset_source is
 * the caller-facing form that builds again for it. */
static void
_drop_source(EnhanceCtrl *p_ctrl) {
   p_ctrl->u_source_gen++;
   if (p_ctrl->p_src_cancel != NULL) {
      g_cancellable_cancel(p_ctrl->p_src_cancel);
      g_clear_object(&p_ctrl->p_src_cancel);
   }
   g_clear_object(&p_ctrl->p_build_file);
   g_clear_pointer(&p_ctrl->p_source, enhancer_source_delete);
}

/* The source is stale (another decode of the file, another file): drop it,
 * and build the current file's again at once when a render waits for one
 * -- it would wait for ever otherwise. */
static void
_reset_source(EnhanceCtrl *p_ctrl) {
   _drop_source(p_ctrl);
   GFile *p_cur = _current_file(p_ctrl);
   if (p_ctrl->p_wait_req != NULL && p_cur != NULL) {
      _build_source(p_ctrl, p_cur);
   }
}

/* A render waited for a source that could not be built (the file cannot
 * be decoded any more): the same as a failed render -- back to the
 * original, which says what the loader thinks of the file. Without a
 * waiting render only the cards stay empty. */
static void
_source_failed(EnhanceCtrl *p_ctrl, const GError *p_err) {
   _Req *p_wait = g_steal_pointer(&p_ctrl->p_wait_req);
   if (p_wait == NULL) {
      p_ctrl->b_thumbs_wanted = FALSE;
      return;
   }
   _req_free(p_wait);
   p_ctrl->b_apply_pending = FALSE;
   g_warning("ggaze: enhance failed: %s",
             p_err != NULL ? p_err->message : "(no detail)");
   _show_status(p_ctrl, "Enhance failed");
   _throw_away(p_ctrl);
   _sync_busy(p_ctrl);
}

/* The source landed: keep it, and start what waited for it -- the preview
 * first (with the state as of now, which supersedes a queued relaunch),
 * the cards after it (_maybe_start_thumbs runs when it lands). */
static void
_source_landed(EnhanceCtrl *p_ctrl, EnhancerSource *p_src) {
   g_clear_pointer(&p_ctrl->p_source, enhancer_source_delete);
   p_ctrl->p_source = p_src;
   _Req *p_wait     = g_steal_pointer(&p_ctrl->p_wait_req);
   if (p_wait != NULL) {
      p_ctrl->b_relaunch = FALSE;
      _launch_render(p_ctrl, p_wait);
   } else {
      _maybe_start_thumbs(p_ctrl);
   }
}

static void
_source_done_cb(GObject *p_obj, GAsyncResult *p_res, gpointer p_data) {
   (void)p_obj;
   _SrcReq        *p_req  = p_data;
   EnhanceCtrl    *p_ctrl = p_req->p_ctrl;
   GError         *p_err  = NULL;
   EnhancerSource *p_src  = enhancer_source_new_finish(p_res, &p_err);
   if (_disposed(p_ctrl) || p_req->u_gen != p_ctrl->u_source_gen) {
      enhancer_source_delete(p_src);
   } else {
      g_clear_object(&p_ctrl->p_src_cancel);
      g_clear_object(&p_ctrl->p_build_file);
      if (p_src == NULL) {
         _source_failed(p_ctrl, p_err);
      } else {
         _source_landed(p_ctrl, p_src);
      }
   }
   g_clear_error(&p_err);
   g_object_unref(p_req->p_host);
   g_free(p_req);
}

/* Build p_file's source unless one is on its way for it: from the decode
 * the viewer shows when there is one (p_orig_tex, the current file's --
 * no second decode), else from the file. */
static void
_build_source(EnhanceCtrl *p_ctrl, GFile *p_file) {
   if (p_ctrl->p_src_cancel != NULL && p_ctrl->p_build_file != NULL &&
       g_file_equal(p_ctrl->p_build_file, p_file)) {
      return;
   }
   _drop_source(p_ctrl);
   PreviewView t_view;
   _view(p_ctrl, &t_view);
   _SrcReq *p_req       = g_new(_SrcReq, 1);
   p_req->p_host        = g_object_ref(p_ctrl->p_host);
   p_req->p_ctrl        = p_ctrl;
   p_req->u_gen         = p_ctrl->u_source_gen;
   p_ctrl->p_src_cancel = g_cancellable_new();
   p_ctrl->p_build_file = g_object_ref(p_file);
   p_ctrl->u_source_count++;
   enhancer_source_new_async(p_file, p_ctrl->p_orig_tex, &t_view,
                             p_ctrl->p_src_cancel, _source_done_cb, p_req);
}

/* --- the pending indicator (8l2) ------------------------------------------ */

static gboolean
_busy_timeout(gpointer p_data) {
   EnhanceCtrl *p_ctrl = p_data;
   p_ctrl->u_busy_id   = 0;
   p_ctrl->b_busy      = TRUE;
   p_ctrl->p_ops->show_busy(p_ctrl->p_host, TRUE);
   _sync_panel(p_ctrl);
   return (G_SOURCE_REMOVE);
}

/* Bring the indicator in line with b_apply_pending: a render pending for
 * _BUSY_DELAY_MS shows it (the host's overlay on the view, and the panel's
 * state line); the render landing, failing or being dropped takes it
 * down. A relaunch keeps it up (still pending), so a burst of slow renders
 * does not blink it. The timer borrows p_ctrl: dispose removes it. */
static void
_sync_busy(EnhanceCtrl *p_ctrl) {
   if (p_ctrl->b_apply_pending && !_disposed(p_ctrl)) {
      if (!p_ctrl->b_busy && p_ctrl->u_busy_id == 0) {
         p_ctrl->u_busy_id =
            g_timeout_add(_BUSY_DELAY_MS, _busy_timeout, p_ctrl);
      }
      return;
   }
   g_clear_handle_id(&p_ctrl->u_busy_id, g_source_remove);
   if (p_ctrl->b_busy) {
      p_ctrl->b_busy = FALSE;
      p_ctrl->p_ops->show_busy(p_ctrl->p_host, FALSE);
      _sync_panel(p_ctrl);
   }
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
 * round 2, issue 4).
 *
 * Nothing is pending afterwards, so the card batch goes ahead here: it
 * waits for the preview (_maybe_start_thumbs), and a preview dropped
 * before it landed -- x, an undo back to no edit, the gate's Discard, all
 * under a slow render -- never lands to start it, so the cards stayed
 * empty (and a batch paused under that preview lost its pictures). Not in
 * _drop_inflight itself: the _launch that follows it there would pause
 * the batch again at once. */
static void
_restore_original(EnhanceCtrl *p_ctrl) {
   _drop_inflight(p_ctrl);
   _drop_managed(p_ctrl); /* no preview left to compare against */
   p_ctrl->b_hold_original = FALSE;
   g_clear_object(&p_ctrl->p_enhance_tex);
   _load_current(p_ctrl);
   _update_header(p_ctrl);
   _maybe_start_thumbs(p_ctrl);
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
      _reset_strengths(p_ctrl);
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
 * Save/Discard in the navigate-away prompt. The undo history is the
 * callers' business (_throw_away forgets it, x records a step on top of
 * it). Never touches the file on disk -- discarding
 * a preview only drops in-memory state. A crop / straighten session over
 * the preview ends FIRST (abandon_tool): the tool's working copy of the
 * transform would otherwise survive the reset and come back on its next
 * nudge or Enter. */
static void
_discard(EnhanceCtrl *p_ctrl) {
   p_ctrl->p_ops->abandon_tool(p_ctrl->p_host);
   p_ctrl->u_enhance_mask = 0;
   _reset_strengths(p_ctrl);        /* the strengths are edits too */
   transform_init(&p_ctrl->t_xf);   /* a discard drops the turn/crop too */
   p_ctrl->b_preview       = FALSE; /* and a tool's override with it */
   p_ctrl->b_hold_original = FALSE; /* belt-and-braces: _restore_original
                                     * also does this, but _render
                                     * early-returns without a
                                     * navigator/enhancer (issue 4) */
   _drop_managed(p_ctrl);           /* the same belt-and-braces */
   _apply_async(p_ctrl);
}

/* Forget the undo history: the steps belonged to another image, or to an
 * edit that was thrown away. */
static void
_forget_history(EnhanceCtrl *p_ctrl) {
   edit_history_clear(p_ctrl->p_history);
   _sync_history(p_ctrl);
}

/* _discard as a throw-away, not a step back: the gate's Discard (another
 * image follows), the slideshow, a failed render (its steps would only
 * lead back to the state that failed). The history goes with the edit;
 * x alone is undoable (enhance_ctrl_revert_all). */
static void
_throw_away(EnhanceCtrl *p_ctrl) {
   _discard(p_ctrl);
   _forget_history(p_ctrl);
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
 * decode, and enhance_ctrl_texture_shown learns it again from that. The
 * preview source goes with it (a render waiting for one gets a new build:
 * _reset_source). */
static void
_forget_original(EnhanceCtrl *p_ctrl) {
   g_clear_object(&p_ctrl->p_orig_tex);
   _drop_managed(p_ctrl);
   _reset_source(p_ctrl); /* built from what is forgotten */
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
   EditSnapshot t_before;
   _snapshot(p_ctrl, &t_before);
   enhance_ctrl_set_transform(p_ctrl, &t_new);
   _push_step(p_ctrl, EDIT_STEP_ROTATE,
              i_dir > 0 ? "rotate right" : "rotate left", &t_before);
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

guint
enhance_ctrl_get_source_count(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, 0);
   return (p_ctrl->u_source_count);
}

guint
enhance_ctrl_get_thumb_launch_count(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, 0);
   return (p_ctrl->u_thumb_launches);
}

gboolean
enhance_ctrl_is_busy_shown(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   return (p_ctrl->b_busy);
}

gboolean
enhance_ctrl_is_settled(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, TRUE);
   gboolean b_cards = p_ctrl->b_thumbs_running ||
                      (p_ctrl->b_thumbs_wanted && p_ctrl->p_panel != NULL &&
                       p_ctrl->b_thumbnails);
   return (!p_ctrl->b_apply_pending && p_ctrl->p_src_cancel == NULL &&
           !b_cards);
}

void
enhance_ctrl_set_preview_cap(EnhanceCtrl *p_ctrl, gint i_max_side) {
   g_return_if_fail(p_ctrl != NULL);
   p_ctrl->i_max_side = MAX(0, i_max_side);
}

gboolean
enhance_ctrl_get_preview_scale(EnhanceCtrl *p_ctrl, gdouble *pd_scale) {
   g_return_val_if_fail(p_ctrl != NULL && pd_scale != NULL, FALSE);
   if (p_ctrl->p_source == NULL) {
      return (FALSE);
   }
   *pd_scale = enhancer_source_get_scale(p_ctrl->p_source);
   return (TRUE);
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
   _throw_away(p_ctrl);
}

/* The "before" is taken ahead of the discard: under a tool it is the
 * state the tool started from (the discard ends the tool, and a live
 * straighten angle was never applied), so `u` brings back what the image
 * was, not a half-made adjustment. x stacks on the steps before it, and
 * undoing it puts every edit back at once. */
gboolean
enhance_ctrl_revert_all(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   EditSnapshot t_before;
   EditSnapshot t_after;
   _snapshot(p_ctrl, &t_before);
   _discard(p_ctrl);
   _snapshot(p_ctrl, &t_after);
   _push_step(p_ctrl, EDIT_STEP_REVERT, "revert all", &t_before);
   /* No step when nothing the history holds changed: a straighten tool's
    * live, unapplied angle is dropped without one. */
   return (!edit_snapshot_equal(&t_before, &t_after));
}

/* --- undo / redo (7i2) ---------------------------------------------------- */

/* Put *p_s back as THE edit state and render it the way a toggle does
 * (_apply_async: last-write-wins, saved/dirty re-derived, panel and
 * title). A state that renders the same as the screen (the auto-crop flag
 * at 0 degrees, a strength of a preset that is off) only refreshes the
 * cards and names, as enhance_ctrl_set_transform does. */
static void
_restore_snapshot(EnhanceCtrl *p_ctrl, const EditSnapshot *p_s) {
   gboolean b_same =
      p_ctrl->u_enhance_mask == p_s->u_mask &&
      _strengths_equal(p_ctrl->d_strength, p_s->d_strength, p_s->u_mask) &&
      transform_equal(_render_transform(p_ctrl), &p_s->t_xf);
   p_ctrl->u_enhance_mask = p_s->u_mask;
   memcpy(p_ctrl->d_strength, p_s->d_strength, sizeof(p_ctrl->d_strength));
   p_ctrl->t_xf      = p_s->t_xf;
   p_ctrl->b_preview = FALSE;
   if (b_same) {
      _refresh_saved(p_ctrl);
      _sync_panel(p_ctrl);
      _update_header(p_ctrl);
      return;
   }
   _apply_async(p_ctrl);
}

/* Undo (b_redo FALSE) or redo one step and say which. */
static gboolean
_step(EnhanceCtrl *p_ctrl, gboolean b_redo) {
   if (!_has_navigator(p_ctrl) || p_ctrl->p_enhancer == NULL) {
      return (FALSE);
   }
   const char         *c_label = NULL;
   const EditSnapshot *p_s =
      b_redo ? edit_history_redo(p_ctrl->p_history, &c_label)
             : edit_history_undo(p_ctrl->p_history, &c_label);
   if (p_s == NULL) {
      /* The panel stays open across files, and a trash / move always
       * changes file (clearing this history), so "d, oops, u" lands here:
       * say where the file undo lives rather than a bare refusal. */
      _show_status(p_ctrl, b_redo ? "Nothing to redo"
                                  : "No edit to undo \u2014 close the panel "
                                    "(a) and u undoes the last trash / move");
      return (FALSE);
   }
   /* Both borrowed from the history, which nothing below changes. */
   char *c_msg = g_strdup_printf("%s: %s", b_redo ? "Redid" : "Undid", c_label);
   _restore_snapshot(p_ctrl, p_s);
   _sync_history(p_ctrl);
   _show_status(p_ctrl, c_msg);
   g_free(c_msg);
   return (TRUE);
}

gboolean
enhance_ctrl_undo(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   return (_step(p_ctrl, FALSE));
}

gboolean
enhance_ctrl_redo(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   return (_step(p_ctrl, TRUE));
}

gboolean
enhance_ctrl_can_undo(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   return (edit_history_can_undo(p_ctrl->p_history));
}

gboolean
enhance_ctrl_can_redo(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   return (edit_history_can_redo(p_ctrl->p_history));
}

void
enhance_ctrl_set_step_base(EnhanceCtrl *p_ctrl, const Transform *p_base) {
   g_return_if_fail(p_ctrl != NULL);
   p_ctrl->b_step_base = p_base != NULL;
   if (p_base != NULL) {
      p_ctrl->t_step_base = *p_base;
   }
}

/* Called once the tool has left (its session base is cleared), with the
 * applied state already committed. */
void
enhance_ctrl_record_tool_step(EnhanceCtrl *p_ctrl, EditStepKind e_kind,
                              const char *c_label, const Transform *p_before) {
   g_return_if_fail(p_ctrl != NULL && p_before != NULL);
   EditSnapshot t_before;
   _snapshot(p_ctrl, &t_before);
   t_before.t_xf = *p_before;
   _push_step(p_ctrl, e_kind, c_label, &t_before);
}

/* --- panel build / teardown ---------------------------------------------- */

/* Drop a scroll of the selected row that is still waiting for a layout.
 * The clock may have been disposed meanwhile (its window unmapped or
 * destroyed drops every handler), hence the check before the
 * disconnect. */
static void
_cancel_scroll(EnhanceCtrl *p_ctrl) {
   if (p_ctrl->p_scroll_clock != NULL) {
      if (g_signal_handler_is_connected(p_ctrl->p_scroll_clock,
                                        p_ctrl->u_scroll_handler)) {
         g_signal_handler_disconnect(p_ctrl->p_scroll_clock,
                                     p_ctrl->u_scroll_handler);
      }
      g_clear_object(&p_ctrl->p_scroll_clock);
      p_ctrl->u_scroll_handler = 0;
   }
}

/* The frame clock laid the window out: the selected row (with its slider,
 * shown or hidden by the selection that asked for this) has its final
 * place now, so scroll it into view. One-shot. */
static void
_scroll_on_layout(GdkFrameClock *p_clock, gpointer p_data) {
   (void)p_clock;
   EnhanceCtrl *p_ctrl = p_data;
   _cancel_scroll(p_ctrl);
   gint i_sel = p_ctrl->i_selected;
   if (i_sel >= 0 && i_sel < GGAZE_ENHANCE_MAX_PRESETS) {
      enhance_ui_scroll_to_row(p_ctrl->p_scroll, p_ctrl->p_rows[i_sel]);
   }
}

/* Keep the selected row on screen (ai2: j / k reach rows the list has to
 * scroll to). Not now: moving the selection shows one slider and hides
 * another, so the rows' places are only known after the next layout --
 * the scroll runs right after it (connected AFTER the handler that lays
 * the window out) and is requested now. A panel not on screen yet (no
 * frame clock) has nothing to scroll; opening it asks again. */
static void
_scroll_to_selected(EnhanceCtrl *p_ctrl) {
   _cancel_scroll(p_ctrl); /* one pending at most, on the clock of now */
   if (p_ctrl->p_scroll == NULL) {
      return;
   }
   GdkFrameClock *p_clock = gtk_widget_get_frame_clock(p_ctrl->p_scroll);
   if (p_clock == NULL) {
      return;
   }
   p_ctrl->p_scroll_clock   = g_object_ref(p_clock);
   p_ctrl->u_scroll_handler = g_signal_connect_after(
      p_clock, "layout", G_CALLBACK(_scroll_on_layout), p_ctrl);
   gdk_frame_clock_request_phase(p_clock, GDK_FRAME_CLOCK_PHASE_LAYOUT);
}

/* Synchronously take the panel out of the host's slot and clear the
 * now-dangling widget pointers. Safe to call when none is open. Closing it
 * never touches u_enhance_mask -- the preview persists until explicitly
 * discarded -- but it closes a strength run: h / l after the panel comes
 * back is a step of its own, so undo can stop at the value it closed on. */
static void
_destroy(EnhanceCtrl *p_ctrl) {
   _cancel_thumbs(p_ctrl);
   p_ctrl->b_thumbs_wanted = FALSE;
   if (p_ctrl->p_panel == NULL) {
      return;
   }
   if (p_ctrl->p_history != NULL) {
      edit_history_end_run(p_ctrl->p_history);
   }
   _cancel_scroll(p_ctrl);
   GtkWidget *p_panel = p_ctrl->p_panel;
   p_ctrl->p_panel    = NULL;
   p_ctrl->p_scroll   = NULL;
   for (guint i = 0; i < G_N_ELEMENTS(p_ctrl->p_btns); i++) {
      p_ctrl->p_rows[i]   = NULL;
      p_ctrl->p_btns[i]   = NULL;
      p_ctrl->p_pics[i]   = NULL;
      p_ctrl->p_scales[i] = NULL;
      p_ctrl->p_values[i] = NULL;
   }
   p_ctrl->p_original_pic = NULL;
   p_ctrl->p_state        = NULL;
   p_ctrl->p_save_btn     = NULL;
   p_ctrl->p_save_target  = NULL;
   p_ctrl->p_undo_btn     = NULL;
   p_ctrl->p_redo_btn     = NULL;
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
   if (!_disposed(p_ctrl) && p_ctx->u_gen == p_ctrl->u_preview_gen) {
      p_ctrl->b_thumbs_running = FALSE;
   }
   if (!_disposed(p_ctrl) && p_ctx->u_gen == p_ctrl->u_preview_gen &&
       p_ctrl->p_panel != NULL && p_tex != NULL && p_tex->len > 0) {
      GdkTexture *p_original = g_ptr_array_index(p_tex, 0);
      if (p_ctrl->p_original_pic != NULL && p_original != NULL) {
         gtk_picture_set_paintable(GTK_PICTURE(p_ctrl->p_original_pic),
                                   GDK_PAINTABLE(p_original));
      }
      for (guint i = 0; i + 1 < p_tex->len && i < G_N_ELEMENTS(p_ctrl->p_pics);
           i++) {
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

/* Stop the card batch in flight, if any: its landing is stale by
 * u_preview_gen. */
static void
_cancel_thumbs(EnhanceCtrl *p_ctrl) {
   p_ctrl->u_preview_gen++;
   p_ctrl->b_thumbs_running = FALSE;
   g_cancellable_cancel(p_ctrl->p_preview_cancel);
   g_clear_object(&p_ctrl->p_preview_cancel);
}

/* Start the wanted card batch once it may run: the panel is up with
 * picture cards, the current file's source has landed, and no preview is
 * pending -- the preview renders first (8l2), and a batch started under
 * it would compete for the same cores. Counted in u_thumb_launches. */
static void
_maybe_start_thumbs(EnhanceCtrl *p_ctrl) {
   GFile *p_file = _current_file(p_ctrl);
   if (!p_ctrl->b_thumbs_wanted || p_ctrl->p_panel == NULL ||
       !p_ctrl->b_thumbnails || p_ctrl->b_apply_pending ||
       p_ctrl->p_source == NULL || p_file == NULL ||
       !g_file_equal(enhancer_source_get_file(p_ctrl->p_source), p_file)) {
      return;
   }
   p_ctrl->b_thumbs_wanted = FALSE;
   _cancel_thumbs(p_ctrl);
   p_ctrl->p_preview_cancel = g_cancellable_new();
   _PreviewCtx *p_ctx       = g_new(_PreviewCtx, 1);
   p_ctx->p_host            = g_object_ref(p_ctrl->p_host);
   p_ctx->p_ctrl            = p_ctrl;
   p_ctx->u_gen             = p_ctrl->u_preview_gen;
   p_ctrl->b_thumbs_running = TRUE;
   p_ctrl->u_thumb_launches++;
   enhancer_preview_thumbnails_async(
      p_ctrl->p_source, enhancer_get_presets(p_ctrl->p_enhancer),
      p_ctrl->p_preview_cancel, _preview_done_cb, p_ctx);
}

/* Ask for the card batch of the current file (the panel opened, was
 * pointed at another file or rebuilt). A no-op for label-only cards or
 * when there is no current file. Counted (u_preview_count) per request,
 * so the seam measures batches asked for, whenever they then run: a batch
 * runs on the file's preview source, after a pending preview
 * (_maybe_start_thumbs). With no source, one is built -- from the decode
 * the viewer shows; while that decode is still on its way the build waits
 * for it (_learn_original starts it), rather than decoding the file a
 * second time next to the viewer. */
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
   _cancel_thumbs(p_ctrl);
   p_ctrl->b_thumbs_wanted = TRUE;
   gboolean b_have =
      p_ctrl->p_source != NULL &&
      g_file_equal(enhancer_source_get_file(p_ctrl->p_source), p_file);
   if (!b_have && p_ctrl->p_orig_tex != NULL) {
      _build_source(p_ctrl, p_file);
   }
   _maybe_start_thumbs(p_ctrl);
}

/* A slider's handlers: value-changed sets the strength (_scale_changed),
 * and a press -- caught in the capture phase, ahead of the slider's own
 * drag -- closes the strength run, so each drag is one undo step even on
 * the same preset. */
static void
_wire_scale(EnhanceCtrl *p_ctrl, GtkWidget *p_scale) {
   g_signal_connect(p_scale, "value-changed", G_CALLBACK(_scale_changed),
                    p_ctrl);
   GtkGesture *p_click = gtk_gesture_click_new();
   gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(p_click),
                                              GTK_PHASE_CAPTURE);
   g_signal_connect(p_click, "pressed", G_CALLBACK(_scale_pressed), p_ctrl);
   gtk_widget_add_controller(p_scale, GTK_EVENT_CONTROLLER(p_click));
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
   p_ctrl->p_scroll       = ui.p_scroll;
   p_ctrl->p_state        = ui.p_state;
   p_ctrl->p_save_btn     = ui.p_save_btn;
   p_ctrl->p_save_target  = ui.p_save_target;
   p_ctrl->p_undo_btn     = ui.p_undo_btn;
   p_ctrl->p_redo_btn     = ui.p_redo_btn;
   for (guint i = 0; i < ui.u_n_presets; i++) {
      p_ctrl->p_rows[i]   = ui.p_rows[i];
      p_ctrl->p_btns[i]   = ui.p_btns[i];
      p_ctrl->p_pics[i]   = ui.p_pics[i];
      p_ctrl->p_scales[i] = ui.p_scales[i];
      p_ctrl->p_values[i] = ui.p_values[i];
      g_signal_connect_swapped(ui.p_btns[i], "clicked",
                               G_CALLBACK(_card_toggle), p_ctrl);
      if (ui.p_scales[i] != NULL) {
         _wire_scale(p_ctrl, ui.p_scales[i]);
      }
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

/* Build the panel (b_thumbnails says which cards) into the host's slot
 * and bring it in line with the state; the view is the caller's
 * business. */
static void
_open_panel(EnhanceCtrl *p_ctrl) {
   p_ctrl->p_panel = _build_panel(p_ctrl);
   gtk_box_append(GTK_BOX(p_ctrl->p_ops->panel_slot(p_ctrl->p_host)),
                  p_ctrl->p_panel);
   _sync_panel(p_ctrl); /* the save-state line has no build-time text */
   _sync_save_target(p_ctrl);
   _start_previews(p_ctrl);
   _mode_changed(p_ctrl); /* the digits are live now; the hint bar shows */
   _scroll_to_selected(p_ctrl); /* a row past the fold stays selected */
}

/* The preset rows changed under an open panel (a Preferences edit of the
 * user presets, ai2): build it again in place, the same kind of cards, in
 * whatever view it is in -- a panel hidden with the grid stays hidden, and
 * Preferences never switches the view. */
static void
_rebuild_panel(EnhanceCtrl *p_ctrl) {
   if (p_ctrl->p_panel == NULL) {
      return;
   }
   _destroy(p_ctrl);
   _open_panel(p_ctrl);
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
   _open_panel(p_ctrl);
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

/* Select row i_idx. Another row closes a strength run: h / l on the next
 * row is a step of its own. Only the cards change -- and the list
 * scrolls the row into view (ai2: a user preset may be below the fold). */
static void
_select(EnhanceCtrl *p_ctrl, gint i_idx) {
   if (i_idx == p_ctrl->i_selected) {
      return;
   }
   edit_history_end_run(p_ctrl->p_history);
   p_ctrl->i_selected = i_idx;
   for (guint i = 0; i < G_N_ELEMENTS(p_ctrl->p_btns); i++) {
      _sync_card(p_ctrl, i);
   }
   _scroll_to_selected(p_ctrl);
}

/* enhance-N (keys 1-8, routed here only while the panel is open --
 * edit-mode.c), a card click and Enter on the selected card (any row,
 * ai2): toggle preset N on/off (layered), record it as an undoable step
 * named "Auto-fix on" (the status line after an undo names it, "Undid:
 * Auto-fix on"), then re-apply asynchronously. The row becomes the
 * selected one (8i2), so a digit or a click followed by h / l tunes that
 * preset. A row with no preset is a silent no-op: no bit a chain never
 * reads is set (a digit could once toggle "preset 8" past a short list). */
void
enhance_ctrl_toggle_preset(EnhanceCtrl *p_ctrl, gint i_idx) {
   g_return_if_fail(p_ctrl != NULL);
   const EnhancerPreset *p_pr = _preset_at(p_ctrl, i_idx);
   if (p_pr == NULL) {
      return;
   }
   _select(p_ctrl, i_idx);
   EditSnapshot t_before;
   _snapshot(p_ctrl, &t_before);
   p_ctrl->u_enhance_mask ^= GGAZE_ENHANCE_BIT(i_idx);
   gboolean b_on = (p_ctrl->u_enhance_mask & GGAZE_ENHANCE_BIT(i_idx)) != 0;
   char *c_label = g_strdup_printf("%s %s", p_pr->c_name, b_on ? "on" : "off");
   _push_step(p_ctrl, EDIT_STEP_PRESET, c_label, &t_before);
   g_free(c_label);
   _apply_async(p_ctrl);
}

/* --- selection and strength (8i2) ---------------------------------------- */

gint
enhance_ctrl_get_selected(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, 0);
   return (p_ctrl->i_selected);
}

gdouble
enhance_ctrl_get_strength(EnhanceCtrl *p_ctrl, gint i_idx) {
   g_return_val_if_fail(p_ctrl != NULL, 0.0);
   if (i_idx < 0 || i_idx >= GGAZE_ENHANCE_MAX_PRESETS) {
      return (0.0);
   }
   return (p_ctrl->d_strength[i_idx]);
}

const gdouble *
enhance_ctrl_get_strengths(EnhanceCtrl *p_ctrl) {
   g_return_val_if_fail(p_ctrl != NULL, NULL);
   return (p_ctrl->d_strength);
}

/* j / k stop at the first and last row rather than wrap: holding k to get
 * to the top must not land at the bottom. Every row is reachable (ai2),
 * the user presets past the digits' eight included. */
void
enhance_ctrl_select_step(EnhanceCtrl *p_ctrl, gint i_dir) {
   g_return_if_fail(p_ctrl != NULL);
   const GPtrArray *p_presets = enhance_ctrl_get_presets(p_ctrl);
   gint             i_n       = p_presets != NULL ? (gint)p_presets->len : 0;
   i_n                        = MIN(i_n, GGAZE_ENHANCE_MAX_PRESETS);
   if (i_n > 0) {
      _select(p_ctrl, CLAMP(p_ctrl->i_selected + i_dir, 0, i_n - 1));
   }
}

void
enhance_ctrl_toggle_selected(EnhanceCtrl *p_ctrl) {
   g_return_if_fail(p_ctrl != NULL);
   if (_preset_at(p_ctrl, p_ctrl->i_selected) != NULL) {
      enhance_ctrl_toggle_preset(p_ctrl, p_ctrl->i_selected);
   }
}

/* "Brightness +0.6": the step label (the status line after an undo names
 * it, "Undid: Brightness +0.6") and the status line of the change. */
static char *
_strength_text(const EnhancerPreset *p_pr, gdouble d_value) {
   char *c_value = preset_strength_label(&p_pr->t_strength, d_value);
   char *c_text  = g_strdup_printf("%s %s", p_pr->c_name, c_value);
   g_free(c_value);
   return (c_text);
}

/* Set tunable preset i_idx's strength to d_value (canonical) and turn it
 * on, as a step of the run i_key -- a burst of h / l presses, or one
 * slider drag, undoes as one (edit_history_push_run) -- then re-render
 * (coalesced by _render while a render is in flight: holding l costs the
 * render in flight and one more, and the last value always lands). */
static void
_set_strength(EnhanceCtrl *p_ctrl, gint i_idx, gdouble d_value, gint i_key) {
   const EnhancerPreset *p_pr = _preset_at(p_ctrl, i_idx);
   EditSnapshot          t_before;
   EditSnapshot          t_after;
   _snapshot(p_ctrl, &t_before);
   p_ctrl->d_strength[i_idx] = d_value;
   p_ctrl->u_enhance_mask |= GGAZE_ENHANCE_BIT(i_idx);
   _snapshot(p_ctrl, &t_after);
   char *c_text = _strength_text(p_pr, d_value);
   edit_history_push_run(p_ctrl->p_history, EDIT_STEP_STRENGTH, i_key, c_text,
                         &t_before, &t_after);
   _show_status(p_ctrl, c_text);
   g_free(c_text);
   _apply_async(p_ctrl);
}

/* The status line of a nudge that hit the end of the range. */
static void
_say_limit(EnhanceCtrl *p_ctrl, const EnhancerPreset *p_pr, gint i_steps) {
   char *c_value = preset_strength_label(
      &p_pr->t_strength, p_ctrl->d_strength[p_ctrl->i_selected]);
   char *c_msg = g_strdup_printf("%s is at its %s (%s)", p_pr->c_name,
                                 i_steps > 0 ? "maximum" : "minimum", c_value);
   _show_status(p_ctrl, c_msg);
   g_free(c_msg);
   g_free(c_value);
}

gboolean
enhance_ctrl_nudge_strength(EnhanceCtrl *p_ctrl, gint i_steps) {
   g_return_val_if_fail(p_ctrl != NULL, FALSE);
   gint                  i_idx = p_ctrl->i_selected;
   const EnhancerPreset *p_pr  = _preset_at(p_ctrl, i_idx);
   if (!_has_navigator(p_ctrl) || p_pr == NULL) {
      return (FALSE);
   }
   if (!p_pr->b_tunable) {
      char *c_msg = g_strdup_printf("%s has no strength to adjust "
                                    "(on / off only \u2014 Enter toggles it; "
                                    "\u2190/\u2192 or PgUp/PgDn change image)",
                                    p_pr->c_name);
      _show_status(p_ctrl, c_msg);
      g_free(c_msg);
      return (FALSE);
   }
   gdouble d_new = preset_strength_nudge(&p_pr->t_strength,
                                         p_ctrl->d_strength[i_idx], i_steps);
   if (d_new == p_ctrl->d_strength[i_idx] &&
       (p_ctrl->u_enhance_mask & GGAZE_ENHANCE_BIT(i_idx)) != 0) {
      _say_limit(p_ctrl, p_pr, i_steps);
      return (TRUE);
   }
   _set_strength(p_ctrl, i_idx, d_new, i_idx);
   return (TRUE);
}

/* A slider moved (by the user: _sync_card moves it under b_syncing): its
 * value snapped onto the preset's step grid becomes the strength, as one
 * step per drag (the run key _SLIDER_RUN + idx, closed by the next press,
 * _scale_pressed). The slider only shows under the selected card, but its
 * row is made the selected one all the same. */
static void
_scale_changed(GtkRange *p_range, gpointer p_data) {
   EnhanceCtrl *p_ctrl = p_data;
   if (p_ctrl->b_syncing || _disposed(p_ctrl)) {
      return;
   }
   gint i_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(p_range), "idx"));
   const EnhancerPreset *p_pr = _preset_at(p_ctrl, i_idx);
   if (p_pr == NULL || !p_pr->b_tunable) {
      return;
   }
   if (i_idx != p_ctrl->i_selected) {
      edit_history_end_run(p_ctrl->p_history);
      p_ctrl->i_selected = i_idx;
   }
   gdouble d_new =
      preset_strength_snap(&p_pr->t_strength, gtk_range_get_value(p_range));
   if (d_new == p_ctrl->d_strength[i_idx] &&
       (p_ctrl->u_enhance_mask & GGAZE_ENHANCE_BIT(i_idx)) != 0) {
      _sync_panel(p_ctrl); /* back onto the grid, nothing changed */
      return;
   }
   _set_strength(p_ctrl, i_idx, d_new, _SLIDER_RUN + i_idx);
}

/* A press on a slider starts a new drag: close any strength run, so the
 * drag is an undo step of its own. */
static void
_scale_pressed(GtkGestureClick *p_click, gint i_n, gdouble d_x, gdouble d_y,
               gpointer p_data) {
   (void)p_click;
   (void)i_n;
   (void)d_x;
   (void)d_y;
   EnhanceCtrl *p_ctrl = p_data;
   if (!_disposed(p_ctrl)) {
      edit_history_end_run(p_ctrl->p_history);
   }
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
      _reset_strengths(p_ctrl); /* per image, like the mask */
      transform_init(&p_ctrl->t_xf);
      p_ctrl->b_preview = FALSE; /* a tool's override goes with it */
      _drop_inflight(p_ctrl);    /* first: a render waiting for the old
                                  * file's source must not have the new
                                  * file's built for it (_reset_source) --
                                  * the cards build theirs from the decode
                                  * the viewer is about to show */
      _forget_original(p_ctrl);  /* another image, another size */
      p_ctrl->b_saved      = FALSE;
      p_ctrl->b_have_saved = FALSE; /* the saved pair was this file's */
      p_ctrl->b_hint_shown = FALSE;
      _forget_history(p_ctrl);         /* the steps were the other image's */
      p_ctrl->b_hold_original = FALSE; /* mask cleared without going through
                                        * _render, so reset the hold flag
                                        * here too (issue 4) */
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

/* Clicked card: its row (any, ai2) toggles that preset exactly as a digit does
 * (an undoable step, then the re-apply that also refreshes the
 * highlights). Does NOT close the panel -- toggling presets while
 * comparing is the point of the layered design (docs/gegl.md). The
 * Original is a reference, not a card: reverting everything is x / the
 * Revert button (6i2), never a stray click on a thumbnail. */
static void
_card_toggle(EnhanceCtrl *p_ctrl, GtkWidget *p_btn) {
   enhance_ctrl_toggle_preset(
      p_ctrl, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(p_btn), "idx")));
}
