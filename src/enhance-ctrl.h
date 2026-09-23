#ifndef GGAZE_ENHANCE_CTRL_H
#define GGAZE_ENHANCE_CTRL_H

/*:*
 * ggaze — Enhance/GEGL UI orchestration controller
 *
 * Owns the entire GEGL "enhance" feature's state and orchestration: the
 * preset mask, the geometric Transform (rotate 90 / straighten / crop,
 * decision #35 -- the same live preview graph, so a turn and a preset
 * compose and one Save exports both), the in-flight apply/preview
 * cancellables and generation counters, the cached enhanced texture, the
 * hold-Space compare flag, the file the preview applies to, the
 * saved-already flag, the enhance side panel and every card/picture it is
 * built of, and the Enhancer engine itself. window.c forwards only the
 * a/s/digit/Space/0/Esc/[/] actions and a few choke-point calls
 * (is_dirty, texture_shown, override_texture, nav_changed); the
 * interactive crop and straighten tools (tool-ctrl.c) edit the Transform
 * through enhance_ctrl_set_transform; every other enhance concern lives
 * here (SRP: window.c is layout + action routing, this module is the
 * enhance feature).
 *
 * The panel sits BESIDE the large view, inside the window's own widget tree
 * (the host hands over a slot to put it in), so the image keeps the whole
 * viewer while the presets are small cards down the side, and every window
 * key -- Space hold-compare, the digits, s, h/l, Esc -- keeps working with
 * no second GtkRoot to bind anything onto. It stays open across navigation
 * and re-previews the new image, so a folder can be worked through with
 * `a` pressed once.
 *
 * Saving: `s` exports a copy and marks the preview SAVED; a saved preview is
 * no longer dirty, so moving on does not prompt for it. What was saved is
 * remembered as the (mask, transform) pair the export wrote: any state that
 * differs from it is unsaved, and a state that comes back to exactly it (a
 * tool cancelled back to it, a preset toggled off and on) is saved again --
 * the file on disk is that state, whichever way it was reached. The gate's
 * prompt therefore only ever asks about work that has not been written
 * anywhere.
 *
 * Two transforms: the COMMITTED one (enhance_ctrl_set_transform) is what
 * `s` exports, what the title names and what dirty/active are judged on;
 * a tool may additionally set a PREVIEW override
 * (enhance_ctrl_set_preview_transform) that only changes what the graph
 * renders -- the crop tool shows the base image without its crop while the
 * rectangle is edited, and the committed crop must keep counting as work
 * meanwhile (or `s` in the tool would find nothing to save and navigating
 * away would skip the prompt and lose it).
 *
 * Rendering is coalesced: at most one apply is in flight, and a state that
 * arrives while one runs is remembered as "render again when this lands",
 * so holding `l` in the straighten tool costs one extra render, not one
 * full decode per repeat. The result on screen is always the latest state
 * (last-write-wins), reached in at most two renders.
 *
 * The controller is a plain struct (not a GtkWidget), mirroring SaveGate /
 * DeleteConfirm: it reaches the window through a host vtable (EnhanceUIHostOps)
 * for the window-side operations it needs (show a texture, refresh the
 * title, show a status line, reload the current file, make the large view
 * visible, the current file, a cached texture, and the panel slot). It
 * tracks its own disposed state. The pure widget construction is delegated
 * to enhance-ui.c (enhance_ui_build_panel); this module owns the built
 * widgets and wires their signals.
 *
 * Compiled only when GEGL is enabled (alongside enhancer.c / enhance-ui.c):
 * every caller is under #if GGAZE_HAVE_GEGL, and there is no enhance feature
 * without GEGL. The controller touches GtkWidgets and GEGL buffers, so it has
 * no unit-test safety net; the test_enhance_flow integration suite is its
 * net.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <glib.h>
#include <gtk/gtk.h>

#include "enhancer.h"  /* Enhancer, EnhancerPreset, GPtrArray of presets */
#include "transform.h" /* the rotate/straighten/crop state (decision #35) */

G_BEGIN_DECLS

typedef struct EnhanceCtrl EnhanceCtrl;

/* Window-side operations the controller calls back through. p_host is the
 * window, borrowed for the duration of each call. Every getter returns a
 * borrowed reference unless noted. The ops say what the controller NEEDS
 * ("make the large view visible", "a box to put the panel in"), not how the
 * window is laid out. */
typedef struct {
   /* Show a texture in the large viewer. The apply-completion, hold-Space
    * restore, and mask-empty restore all funnel through here. */
   void (*show_texture)(gpointer p_host, GdkTexture *p_tex);
   /* Refresh the window title (enhancer_describe_mask gives the suffix). */
   void (*update_header)(gpointer p_host);
   /* Transient status line (the window's info overlay). */
   void (*show_status)(gpointer p_host, const char *c_msg);
   /* Reload the current file's original into the viewer (the mask-empty
    * restore path -- texturecache is cheap, no GEGL). */
   void (*load_current)(gpointer p_host);
   /* Bring the large view on screen (a preset was toggled, or the panel is
    * opening -- it only makes sense beside the large view). */
   void (*ensure_large_view)(gpointer p_host);

   /* The navigator's current file (NULL if no folder is open or the folder
    * is empty). Safe to call in any state. */
   GFile *(*get_current_file)(gpointer p_host);
   /* The cached texture for p_file (NULL if evicted or stale). Used on a
    * same-file rescan only, to tell a rewrite of the current file from the
    * rescan a save's "-enhanced" copy causes (the cache's stamp answers it
    * with no second stat). */
   GdkTexture *(*get_cached_texture)(gpointer p_host, GFile *p_file);
   /* The GtkBox beside the large view that the panel is appended to while
    * open (and removed from when closed). The window shows it only in the
    * large view. */
   GtkWidget *(*panel_slot)(gpointer p_host);
   /* TRUE iff a folder is open (the navigator is non-NULL). The apply path
    * guards on this (NOT on get_current_file) so an EMPTY folder -- navigator
    * present, current NULL -- still reaches the mask-reset branch rather than
    * early-returning. */
   gboolean (*has_navigator)(gpointer p_host);
   /* The preview is about to be discarded (Esc, `0` / the Original card, a
    * failed apply, the gate's Discard, the slideshow): end a crop /
    * straighten session over it first, without restoring anything -- the
    * discard resets the transform and a tool's override together. A tool
    * left running kept its working angle / turn and re-applied the
    * discarded state on the next nudge or Enter. */
   void (*abandon_tool)(gpointer p_host);
   /* The current file's original on screen is another texture object than
    * before (its first decode landed, or a reload after a rewrite / a
    * discard decoded it again -- enhance_ctrl_texture_shown): a tool laid
    * out on the previous one lays out again and redraws. Called from
    * inside the window's texture choke point, before the viewer shows it. */
   void (*original_changed)(gpointer p_host);
} EnhanceUIHostOps;

/* Continuation for enhance_ctrl_save_async: b_ok is TRUE on a real write. */
typedef void (*EnhanceSaveDoneFn)(gboolean b_ok, gpointer p_data);

/* Construct a controller bound to p_host. p_ops is borrowed for the
 * controller's lifetime (must outlive it). Creates the Enhancer engine. */
EnhanceCtrl *enhance_ctrl_new(const EnhanceUIHostOps *p_ops, gpointer p_host);

/* Free the controller (in finalize, after enhance_ctrl_dispose ran in
 * dispose). Releases the Enhancer engine and any leftover preview state. */
void enhance_ctrl_delete(EnhanceCtrl *p_ctrl);

/* Cancel + drop every in-flight async and clear owned textures/files (call
 * from window dispose, BEFORE the texturecache is freed). Removes the panel
 * too. */
void enhance_ctrl_dispose(EnhanceCtrl *p_ctrl);

/* --- engine presets --- */
/* Feed the Preferences user presets (SettingsPair*) to the engine, which
 * rebuilds built-ins + user presets itself (enhancer_set_user_presets). */
void             enhance_ctrl_set_user_presets(EnhanceCtrl     *p_ctrl,
                                               const GPtrArray *p_pairs);
const GPtrArray *enhance_ctrl_get_presets(EnhanceCtrl *p_ctrl);
guint8           enhance_ctrl_get_mask(EnhanceCtrl *p_ctrl);

/* --- the geometric transform (rotate 90 / straighten / crop) ------------- */
/* The current Transform (borrowed; the identity when none is active). */
const Transform *enhance_ctrl_get_transform(EnhanceCtrl *p_ctrl);

/* Commit p_xf as THE Transform (dropping any preview override) and re-apply
 * the preview asynchronously, exactly like toggling a preset. A transform
 * that renders the same as what is on screen (transform_equal) is stored
 * without a re-apply, so a tool that pushes its working state on every nudge
 * never renders twice for nothing. With an empty mask and the identity the
 * original is restored. A crop lying entirely outside its base (the
 * straighten shrank the base past it) is kept as it is: the chain crops
 * nothing then, the title says "crop (outside view)", and the crop is
 * applied again as soon as the base grows back over it. */
void enhance_ctrl_set_transform(EnhanceCtrl *p_ctrl, const Transform *p_xf);

/* Render p_xf instead of the committed transform (NULL: back to the
 * committed one) without touching what `s` exports or what counts as work
 * -- the crop tool's view of the base image. Re-applies only when the
 * rendered transform actually changes, and never switches views (a tool
 * abandoned by a view change clears its override from the grid). */
void enhance_ctrl_set_preview_transform(EnhanceCtrl     *p_ctrl,
                                        const Transform *p_xf);

/* `]` (i_dir > 0) / `[` (i_dir < 0): one more quarter turn, one-shot, then
 * re-apply. An active crop turns with the image. */
void enhance_ctrl_rotate_quarter(EnhanceCtrl *p_ctrl, gint i_dir);

/* TRUE while an apply is in flight, i.e. the texture on screen predates the
 * current mask/transform. */
gboolean enhance_ctrl_is_pending(EnhanceCtrl *p_ctrl);

/* TRUE iff p_tex -- what the viewer shows -- is exactly the texture the
 * current state renders to: the last landed apply for the current mask and
 * render transform (a tool's override, else the committed one), with none
 * newer in flight; or, when that state needs no GEGL at all, the original
 * of the current file as the viewer last showed it (learned through
 * enhance_ctrl_texture_shown; an original not shown since the last
 * navigation or rewrite is never current). It is FALSE for the picture
 * that is still up while a render is pending, for the original shown under
 * a held Space, and for another file's texture waiting for a load to land.
 * Pure identity comparisons, no cache lookup: safe per snapshot and per
 * pointer motion, and a `touch` on the file (which stales its cache entry)
 * cannot make an overlay vanish. The tools draw the crop rectangle over,
 * and measure drags on, only a texture this says yes to: a size comparison
 * could not tell 0 from 180 degrees, +a from -a, or a preset toggled under
 * the tool from the base it replaced. */
gboolean enhance_ctrl_is_current_render(EnhanceCtrl *p_ctrl, GdkTexture *p_tex);

/* TRUE while Space is held and the original is on screen in place of the
 * preview (enhance_ctrl_set_hold_original). */
gboolean enhance_ctrl_is_hold_original(EnhanceCtrl *p_ctrl);

/* The size of the base image the crop rectangle refers to (the original
 * after the current turn and straighten, crop ignored --
 * transform_base_size), or FALSE while the original's size is not known:
 * the viewer has not shown the file's decode yet (enhance_ctrl_texture_
 * shown) and no apply has landed for it. A pure read, safe from a draw
 * callback: nothing is looked up. */
gboolean enhance_ctrl_get_base_size(EnhanceCtrl *p_ctrl, gint *p_w, gint *p_h);

/* The original's upright size on the same terms (FALSE when unknown): what
 * the straighten tool feeds transform_rebase_crop to keep a crop over the
 * same content while the angle changes the base, and what tells it the
 * original is known before it measures a horizon at 0 degrees. */
gboolean enhance_ctrl_get_orig_size(EnhanceCtrl *p_ctrl, gint *p_w, gint *p_h);

/* How many preview renders (full decode + chain) this controller has
 * launched so far. A test seam for the render coalescing: N rapid changes
 * must cost at most two launches (tests/test_enhance_flow.c). */
guint enhance_ctrl_get_render_count(EnhanceCtrl *p_ctrl);

/* How many thumbnail-preview batches the open panel has started so far
 * (only batches that really launched: label-only cards and "no current
 * file" start none). A test seam: an open with the panel up must re-point
 * it at the new file with one batch (tests/test_enhance_flow.c, gd2). */
guint enhance_ctrl_get_preview_count(EnhanceCtrl *p_ctrl);

/* --- state queries --- */
/* TRUE iff a GEGL preview is on screen (a preset enabled or a non-identity
 * transform), saved or not. */
gboolean enhance_ctrl_is_active(EnhanceCtrl *p_ctrl);

/* TRUE iff a GEGL enhance preview is active AND has not been exported since
 * its last change (mask != 0 and not saved). This is what the
 * Save/Discard/Cancel gate asks: a preview `s` already wrote is not dirty. */
gboolean enhance_ctrl_is_dirty(EnhanceCtrl *p_ctrl);

/* TRUE iff the side panel is open. */
gboolean enhance_ctrl_is_open(EnhanceCtrl *p_ctrl);

/* The hot-path override: returns the texture the viewer should show given the
 * natural candidate p_tex. An active, non-hold-original preview wins; else
 * p_tex is returned unchanged. Called from the window's single texture
 * choke point (_show_texture). A pure query: it learns nothing. */
GdkTexture *enhance_ctrl_override_texture(EnhanceCtrl *p_ctrl,
                                          GdkTexture  *p_tex);

/* The window is about to show p_tex, a DECODED texture of the current file
 * (a cache hit, a finished load -- never a progressive loader's low-res
 * partial, and never this controller's own render or the original it
 * re-shows under Space, which teach it nothing): remember it as the file's
 * original, with its size. This is the one place the original's identity
 * is learned, so it is exactly the object the viewer holds -- a same-file
 * reload that decodes a new object (the file touched, a preset discarded)
 * refreshes it, where an identity looked up once from the cache went stale
 * and refused every tool action for good. Called from the window's texture
 * choke point right before enhance_ctrl_override_texture, whatever that
 * decides to show. When the object changed, a tool laid out on the old one
 * is told through the host (original_changed). No lookup, no stat. */
void enhance_ctrl_texture_shown(EnhanceCtrl *p_ctrl, GdkTexture *p_tex);

/* Hold-Space compare: TRUE shows the original as the viewer last showed it
 * (this controller's own reference -- an evicted or stale cache entry does
 * not matter); FALSE restores the cached modified texture. No-op if nothing
 * is active, the requested state is already in effect, or the file's decode
 * has not been shown since a rewrite forgot it. */
void enhance_ctrl_set_hold_original(EnhanceCtrl *p_ctrl, gboolean b_hold);

/* --- action entry points (the GActions stay window-side; these do the work) */
/* `a`: open the side panel beside the large view (switching to it first),
 * with a preview thumbnail per card when b_thumbnails, label-only cards
 * otherwise; a second call closes it. A no-op if no folder is open. */
void enhance_ctrl_toggle_open(EnhanceCtrl *p_ctrl, gboolean b_thumbnails);
/* Esc with the panel open: close it (the preview stays). Returns TRUE iff a
 * panel was open, so the window's Esc can stop there. */
gboolean enhance_ctrl_close(EnhanceCtrl *p_ctrl);
/* enhance-N (keys 1-8): toggle preset i_idx (0..7) on/off (layered), then
 * re-apply asynchronously. Out-of-range i_idx is a silent no-op. */
void enhance_ctrl_toggle_preset(EnhanceCtrl *p_ctrl, gint i_idx);
/* `s` / Ctrl+S: export the previewed image with the enabled-preset chain to
 * a non-colliding <stem>-enhanced[-<n>].<ext>, in a worker (the full decode
 * + chain + encode takes seconds and used to freeze the UI). On success the
 * preview counts as saved (no longer dirty) unless the mask changed while
 * the export ran. Reports status itself; fn_done (may be NULL) is called on
 * the main thread with the outcome. Called with nothing to save it reports
 * and completes with FALSE at once. */
void enhance_ctrl_save_async(EnhanceCtrl *p_ctrl, EnhanceSaveDoneFn fn_done,
                             gpointer p_done_data);
/* TRUE iff there is actually an enhance preview to export right now (a
 * folder is open, a preset is enabled, and the preview belongs to a file).
 * Split out so the Save/Discard/Cancel gate can tell "nothing to save" (the
 * preview legitimately vanished while the dialog was up) from a real export
 * failure. */
gboolean enhance_ctrl_can_save(EnhanceCtrl *p_ctrl);

/* --- choke points --- */
/* The navigator "changed" choke point -- and an open's, which replaces the
 * navigator without that signal firing for the new file (window.c
 * _open_rebuild): reset the preview only when the current file's IDENTITY
 * actually changed (see the comment in the .c). An
 * open panel stays open and re-previews the new file. A rescan of the SAME
 * file re-checks the original against the texture cache: rewritten in
 * place (`e`, `!`), possibly with another size, its size and identity are
 * forgotten (the reload that follows shows the fresh decode, which is
 * learned) and an active preview is rendered again from the new contents;
 * the rescan a save's "-enhanced" copy causes finds the entry fresh and
 * changes nothing. The render of a rewritten file may land before the
 * reload's decode: its original size is told to the tool as a new
 * original's is (original_changed), so a crop tool lays out again on it
 * either way. */
void enhance_ctrl_nav_changed(EnhanceCtrl *p_ctrl);

/* Drop the current enhance preview and go back to the original (Esc, `0`,
 * slideshow auto-advance, the SaveGate's Discard). Ends a running tool
 * first (abandon_tool). Never touches the file on disk. */
void enhance_ctrl_discard(EnhanceCtrl *p_ctrl);

G_END_DECLS

#endif /* GGAZE_ENHANCE_CTRL_H */
