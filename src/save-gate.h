#ifndef GGAZE_SAVE_GATE_H
#define GGAZE_SAVE_GATE_H

/*:*
 * ggaze — Save/Discard/Cancel prompt gate
 *
 * The async state machine that sits in front of every continuation that would
 * discard or overwrite an unsaved GEGL enhance preview: navigation (h/l/g/G/
 * scroll), grid selection, open/drop, trash, delete, move, and quit / native
 * close. If a preview is active and unsaved it raises a modal
 * Save/Discard/Cancel GtkAlertDialog and runs the continuation only once that
 * resolves in favour of proceeding (Save or Discard); Cancel (and a real
 * export failure) keep the preview and drop the continuation. At most ONE
 * prompt is ever outstanding per window, and a second request that arrives
 * while one is up is parked (newest wins) and retried through the same gate
 * once the prompt resolves -- or dropped, audibly, when the answer is Cancel
 * or the prompt's own continuation closes the window.
 *
 * This module owns that state machine -- the prompt's cancellable, the
 * outstanding-flag, the parked-request slot, and the per-prompt ctx -- so
 * window.c's gated paths just call save_gate_maybe_save_then(). The actual
 * "is a preview dirty", "export it", and "drop the in-memory preview" decisions
 * stay in window.c (and reach the enhancer through it) and are invoked through
 * the host ops, so the helper touches no GtkWidget and no engine state
 * directly. It DOES own the GtkAlertDialog (the one GtkWidget the prompt is),
 * mirroring delete-confirm.h's split.
 *
 * The gate exists in EVERY build (like delete-confirm): without GEGL the host's
 * is_dirty op is always FALSE, so save_gate_maybe_save_then runs every
 * continuation straight away and no prompt is ever raised.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct SaveGate SaveGate;

/* Window-side operations the gate calls back through. p_host is the window
 * (borrowed for the duration of each call, and IS the GtkWindow the dialog is
 * transient for). */
typedef struct {
   /* TRUE iff an unsaved enhance preview is active. The gate prompts only
    * when dirty; otherwise it runs the continuation at once. */
   gboolean (*is_dirty)(gpointer p_host);
   /* The Save button: export the enhanced copy. Return TRUE iff the user's
    * action may proceed -- a successful export, OR "nothing to save" (which
    * is NOT a failure: the preview can legitimately be gone by the time the
    * dialog is answered). Return FALSE on a real export failure, so the gate
    * keeps the preview and does not proceed. The op reports status itself. */
   gboolean (*do_save)(gpointer p_host);
   /* Drop the in-memory enhance preview (the mask/preview), called after a
    * Discard and after a Save that may proceed. */
   void (*discard)(gpointer p_host);
   /* Transient status line (the window's info overlay). */
   void (*show_status)(gpointer p_host, const char *c_msg);
   /* The GtkAlertDialog's own toplevel, ref'd so it is not freed under the
    * GTask that still points at it (transfer full). May return NULL. */
   GtkWindow *(*alert_dialog_window)(gpointer p_host);
   /* The continuation that closes the window, compared by IDENTITY to decide
    * whether a queued request must not be retried after the answer (a closing
    * window can honour no request). May be NULL when the host has no such
    * continuation, in which case the quit-shortcut is never recognised. */
   GSourceFunc quit_continuation;
} SaveGateHostOps;

/* Construct a gate bound to p_host. p_ops is borrowed for the gate's lifetime.
 */
SaveGate *save_gate_new(const SaveGateHostOps *p_ops, gpointer p_host);

/* Free the gate. The outstanding dialog's ctx holds its own ref on the host,
 * so the gate (like the dialog's host) outlives dispose; call this in
 * finalize, after save_gate_dispose has run in dispose. */
void save_gate_delete(SaveGate *p_gate);

/* If dirty, ask Save/Discard/Cancel before running fn(data); else run fn now.
 * Owns data for the whole flow and releases it through fn_free on EVERY exit
 * path -- including the ones that never run fn (Cancel, dialog dismissal, a
 * failed Save, a parked request dropped because the prompt was cancelled).
 * fn_free may be NULL when data is not owned by the request. */
void save_gate_maybe_save_then(SaveGate *p_gate, GSourceFunc fn, gpointer data,
                               GDestroyNotify fn_free);

/* TRUE while the modal Save/Discard/Cancel prompt is outstanding (the window
 * refuses a native close while one is up). */
gboolean save_gate_outstanding(const SaveGate *p_gate);

/* Cancel + drop any outstanding prompt (window dispose / close). Sets the
 * gate's gone flag so a later dialog-completion callback drops a parked
 * request through the log channel rather than touching a gone overlay. */
void save_gate_dispose(SaveGate *p_gate);

G_END_DECLS

#endif /* GGAZE_SAVE_GATE_H */