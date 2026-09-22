#ifndef GGAZE_ENHANCE_CTRL_H
#define GGAZE_ENHANCE_CTRL_H

/*:*
 * ggaze — Enhance/GEGL UI orchestration controller
 *
 * Owns the entire GEGL "enhance" feature's state and orchestration: the
 * preset mask, the in-flight apply/preview cancellables and generation
 * counters, the cached enhanced texture, the hold-Space compare flag, the
 * file the preview applies to, the enhance UI widget (gallery window or
 * compact popover) and every row/picture it is built of, and the Enhancer
 * engine itself. window.c forwards only the a/s/digit/Space actions and a
 * few choke-point queries (is_dirty, override_texture, nav_changed); every
 * other enhance concern lives here (SRP: window.c is layout + action
 * routing, this module is the enhance feature).
 *
 * The controller is a plain struct (not a GtkWidget), mirroring SaveGate /
 * DeleteConfirm: it reaches the window through a host vtable (EnhanceUIHostOps)
 * for the window-side operations it needs (show a texture, refresh the
 * title, show a status line, reload the current file, make the large view
 * visible, the current file, a cached texture, a popover parent, the
 * transient parent, and binding the window's shortcuts onto the gallery
 * window). It tracks its own disposed state. The pure widget construction is
 * delegated to enhance-ui.c (enhance_ui_build_content); this module owns the
 * built widgets and wires their signals.
 *
 * Compiled only when GEGL is enabled (alongside enhancer.c / enhance-ui.c):
 * every caller is under #if GGAZE_HAVE_GEGL, and there is no enhance feature
 * without GEGL. The controller touches GtkWidgets and GEGL buffers, so it has
 * no unit-test safety net; the 44-subtest test_enhance_flow integration suite
 * is its net.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <glib.h>
#include <gtk/gtk.h>

#include "enhancer.h" /* Enhancer, EnhancerPreset, GPtrArray of presets */

G_BEGIN_DECLS

typedef struct EnhanceCtrl EnhanceCtrl;

/* Window-side operations the controller calls back through. p_host is the
 * window, borrowed for the duration of each call. Every getter returns a
 * borrowed reference unless noted. The ops say what the controller NEEDS
 * ("make the large view visible", "a widget to parent the popover to"), not
 * how the window is laid out. */
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
   /* Bring the large view on screen (a preset was toggled). */
   void (*ensure_large_view)(gpointer p_host);

   /* The navigator's current file (NULL if no folder is open or the folder
    * is empty). Safe to call in any state. */
   GFile *(*get_current_file)(gpointer p_host);
   /* The cached texture for p_file (NULL if evicted), for hold-Space. */
   GdkTexture *(*get_cached_texture)(gpointer p_host, GFile *p_file);
   /* A widget in the window's tree to parent the compact popover to. */
   GtkWidget *(*popover_parent)(gpointer p_host);
   /* The toplevel the gallery window is transient for. */
   GtkWindow *(*transient_parent)(gpointer p_host);
   /* Give p_toplevel (the gallery window, its own GtkRoot) the window's
    * actions and key table, so `s`, `a`, h/l, q, ... work while it has
    * focus. */
   void (*bind_shortcuts)(gpointer p_host, GtkWidget *p_toplevel);
   /* TRUE iff a folder is open (the navigator is non-NULL). The apply path
    * guards on this (NOT on get_current_file) so an EMPTY folder -- navigator
    * present, current NULL -- still reaches the mask-reset branch rather than
    * early-returning. */
   gboolean (*has_navigator)(gpointer p_host);
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
 * from window dispose, BEFORE the texturecache is freed). Destroys the UI
 * widget too. */
void enhance_ctrl_dispose(EnhanceCtrl *p_ctrl);

/* --- engine presets --- */
/* Feed the Preferences user presets (SettingsPair*) to the engine, which
 * rebuilds built-ins + user presets itself (enhancer_set_user_presets). */
void             enhance_ctrl_set_user_presets(EnhanceCtrl     *p_ctrl,
                                               const GPtrArray *p_pairs);
const GPtrArray *enhance_ctrl_get_presets(EnhanceCtrl *p_ctrl);
guint8           enhance_ctrl_get_mask(EnhanceCtrl *p_ctrl);

/* --- state queries --- */
/* TRUE iff a GEGL enhance preview is active and unsaved (mask != 0). */
gboolean enhance_ctrl_is_dirty(EnhanceCtrl *p_ctrl);

/* The hot-path override: returns the texture the viewer should show given the
 * natural candidate p_tex. An active, non-hold-original preview wins; else
 * p_tex is returned unchanged. Called from the window's single texture
 * choke point (_show_texture). */
GdkTexture *enhance_ctrl_override_texture(EnhanceCtrl *p_ctrl,
                                          GdkTexture  *p_tex);

/* Hold-Space compare: TRUE shows the cached original (texturecache, cheap);
 * FALSE restores the cached modified texture. No-op if nothing is dirty or
 * the requested state is already in effect. */
void enhance_ctrl_set_hold_original(EnhanceCtrl *p_ctrl, gboolean b_hold);

/* --- action entry points (the GActions stay window-side; these do the work) */
/* `a`: open/close the enhance gallery window (b_previews: thumbnail
 * previews in their own window) or the compact popover. A no-op if no
 * folder is open. */
void enhance_ctrl_toggle_open(EnhanceCtrl *p_ctrl, gboolean b_previews);
/* enhance-N (keys 1-8): toggle preset i_idx (0..7) on/off (layered), then
 * re-apply asynchronously. Out-of-range i_idx is a silent no-op. */
void enhance_ctrl_toggle_preset(EnhanceCtrl *p_ctrl, gint i_idx);
/* `s`: export the previewed image with the enabled-preset chain to a
 * non-colliding <stem>-enhanced[-<n>].<ext>, in a worker (the full decode +
 * chain + encode takes seconds and used to freeze the UI). Reports status
 * itself; fn_done (may be NULL) is called on the main thread with the
 * outcome. Called with nothing to save it reports and completes with FALSE
 * at once. */
void enhance_ctrl_save_async(EnhanceCtrl *p_ctrl, EnhanceSaveDoneFn fn_done,
                             gpointer p_done_data);
/* TRUE iff there is actually an enhance preview to export right now (a
 * folder is open, a preset is enabled, and the preview belongs to a file).
 * Split out so the Save/Discard/Cancel gate can tell "nothing to save" (the
 * preview legitimately vanished while the dialog was up) from a real export
 * failure. */
gboolean enhance_ctrl_can_save(EnhanceCtrl *p_ctrl);

/* --- choke points --- */
/* The navigator "changed" choke point: reset the preview only when the
 * current file's IDENTITY actually changed (see the comment in the .c). */
void enhance_ctrl_nav_changed(EnhanceCtrl *p_ctrl);

/* Drop the current enhance preview and go back to the original (Esc,
 * slideshow auto-advance, the SaveGate's Discard). Never touches the file
 * on disk. */
void enhance_ctrl_discard(EnhanceCtrl *p_ctrl);

G_END_DECLS

#endif /* GGAZE_ENHANCE_CTRL_H */