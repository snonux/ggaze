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
 * for the ~10 window-side operations it needs (show a texture in the large
 * viewer, refresh the title, show a status line, reload the current file,
 * and getters for the current file, the texturecache, the preview-thumbnails
 * setting, the grid/large stack, the toplevel window widget, and whether the
 * window is disposed). The pure widget construction is delegated to
 * enhance-ui.c (enhance_ui_build_content); this module owns the built widgets
 * and wires their signals.
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
 * borrowed reference unless noted. */
typedef struct {
   /* Show a texture in the large viewer. The apply-completion, hold-Space
    * restore, and mask-empty restore all funnel through here (as they did
    * through window.c's _show_texture). */
   void (*show_texture)(gpointer p_host, GdkTexture *p_tex);
   /* Refresh the window title (preset names are appended when layered, by
    * the caller reading enhance_ctrl_get_mask / _get_presets). */
   void (*update_header)(gpointer p_host);
   /* Transient status line (the window's info overlay). */
   void (*show_status)(gpointer p_host, const char *c_msg);
   /* Reload the current file's original into the viewer (the mask-empty
    * restore path -- texturecache is cheap, no GEGL). */
   void (*load_current)(gpointer p_host);

   /* The navigator's current file (NULL if no folder is open or the folder
    * is empty). Safe to call in any state. */
   GFile *(*get_current_file)(gpointer p_host);
   /* The cached texture for p_file from the texturecache (NULL if evicted),
    * for hold-Space compare. */
   GdkTexture *(*get_cached_texture)(gpointer p_host, GFile *p_file);
   /* Whether preview-thumbnails mode is on (gallery vs compact popover). */
   gboolean (*get_preview_thumbnails)(gpointer p_host);
   /* The grid/large GtkStack: the compact popover's parent and the "large"
    * switch target in apply_begin. */
   GtkWidget *(*get_stack)(gpointer p_host);
   /* The toplevel window widget: the gallery window's transient parent and
    * sizing reference. */
   GtkWidget *(*get_window_widget)(gpointer p_host);
   /* TRUE once the host's dispose has run; async callbacks check it before
    * touching any widget. */
   gboolean (*is_disposed)(gpointer p_host);
   /* TRUE iff a folder is open (the navigator is non-NULL). The apply path
    * guards on this (NOT on get_current_file) so an EMPTY folder -- navigator
    * present, current NULL -- still reaches the mask-reset branch rather than
    * early-returning, matching the old p_nav != NULL guard. */
   gboolean (*has_navigator)(gpointer p_host);
} EnhanceUIHostOps;

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

/* --- engine presets (window's _load_engine_lists builds the merged list) -- */
void enhance_ctrl_set_presets(EnhanceCtrl *p_ctrl, const GPtrArray *p_presets);
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
/* `a`: open/close the enhance gallery window (preview mode) or compact
 * popover. A no-op if no folder is open. */
void enhance_ctrl_toggle_open(EnhanceCtrl *p_ctrl);
/* enhance-N (keys 1-8): toggle preset i_idx (0..7) on/off (layered), then
 * re-apply asynchronously. Out-of-range i_idx is a silent no-op. */
void enhance_ctrl_toggle_preset(EnhanceCtrl *p_ctrl, gint i_idx);
/* `s`: export the previewed image with the enabled-preset chain to a
 * non-colliding <stem>-enhanced[-<n>].<ext>. Returns TRUE on success, FALSE on
 * a real export failure OR when there is nothing to save (the caller tells
 * them apart via enhance_ctrl_can_save). Reports status itself. */
gboolean enhance_ctrl_do_save(EnhanceCtrl *p_ctrl);
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