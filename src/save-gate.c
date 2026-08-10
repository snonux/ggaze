/* save-gate.c — Save/Discard/Cancel prompt gate (see save-gate.h). */
#include "save-gate.h"

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

/* A gated continuation: the function to run once the (possible)
 * Save/Discard/Cancel prompt resolves, the data it acts on, and the notify
 * that releases that data. save_gate_maybe_save_then OWNS data for the whole
 * flow and releases it through fn_free on EVERY exit path -- including the
 * ones that never run fn (Cancel, dialog dismissal, a failed Save, a parked
 * request dropped because the prompt was cancelled). fn_free is NULL when
 * data is not owned by the request (e.g. the window itself). */
typedef struct {
   GSourceFunc    fn;
   gpointer       data;
   GDestroyNotify fn_free;
} _Request;

/* Give p_req its one chance (run the continuation iff b_proceed), then release
 * its data. The single place that decides "this request is done" -- so no
 * exit path can forget one of the two halves (tu0 review round 2, finding b:
 * Cancel and the dismiss path used to return early and leak the heap ctx, each
 * of which holds an owned host ref and therefore pinned an entire
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

typedef struct {
   SaveGate *p_gate;        /* borrowed; outlives the dialog via the host ref
                             * (the gate is owned by the host window) */
   gpointer p_host;         /* owned ref: outlives the async dialog (mirrors
                             * delete-confirm.c's _DeleteCtx convention) */
   _Request      t_req;     /* the gated request; see _Request for ownership */
   GCancellable *p_cancel;  /* owned ref on the SAME GCancellable the gate
                             * parks in p_prompt_cancel. Held here as well so
                             * the callback can ask the object itself whether
                             * this prompt was cancelled, and so the ref is
                             * released on every exit path even after dispose
                             * has already dropped the gate's own -- see
                             * _save_prompt_outcome and save_gate_dispose. */
   GtkWindow *p_dlg_window; /* owned ref on the dialog's own toplevel, purely
                             * to keep it from being FREED under the GTask
                             * that still points at it -- see
                             * SaveGateHostOps.alert_dialog_window */
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

struct SaveGate {
   const SaveGateHostOps *p_ops;
   gpointer               p_host;
   gboolean b_save_prompt;        /* TRUE while the modal Save/Discard/Cancel
                                   * prompt is outstanding, so a trigger the
                                   * modal grab cannot swallow (Alt+F4 / the
                                   * WM close button reach "close-request"
                                   * without being input events, and a
                                   * single-instance open arrives over D-Bus)
                                   * cannot stack a second dialog on top of
                                   * it, nor slip its continuation past the
                                   * gate while that dialog is still up */
   GCancellable *p_prompt_cancel; /* the outstanding prompt's GCancellable
                                   * (NULL when no prompt is up), handed to
                                   * gtk_alert_dialog_choose so dispose can
                                   * force the dialog's GTask to complete
                                   * instead of leaving it (and the _SaveCtx
                                   * it carries) hanging -- see
                                   * save_gate_dispose. Exactly one prompt is
                                   * ever outstanding (b_save_prompt), so this
                                   * single slot always names THAT prompt */
   gboolean b_prompt_quits;       /* TRUE while the outstanding prompt's OWN
                                   * continuation is the host's quit
                                   * continuation, i.e. answering it in favour
                                   * of proceeding closes the window. A queued
                                   * request must then be dropped rather than
                                   * retried into a window that is going away
                                   * (round 4, finding r). Cleared with the
                                   * slot in _save_prompt_flush */
   gboolean b_disposed;           /* set in save_gate_dispose; the dialog
                                   * callback checks it before touching the
                                   * overlay */
   _Request *p_pending;           /* the ONE request parked while the prompt
                                   * above is outstanding (newest wins), so a
                                   * second, different request is not silently
                                   * swallowed: it is retried through the same
                                   * gate once the prompt resolves in favour
                                   * of proceeding, and dropped with a status
                                   * line when it resolves to Cancel (round 3,
                                   * finding i). NULL when nothing is parked */
};

/* Release the parked request p_req without ever running it, and say so. Takes
 * ownership of p_req (both the _Request box and, via _request_finish, the data
 * it carries).
 *
 * b_window_gone picks the channel. While the host is alive the info overlay is
 * the right place. Once it is closing or disposed there is no overlay left to
 * paint on -- and the requests that reach this slot are precisely the ones
 * that did NOT come from this window's keyboard (a single-instance
 * `ggaze other.jpg` over D-Bus, a drop), so the log is where the person who
 * made the request is actually looking. */
static void
_drop_pending(SaveGate *p_gate, _Request *p_req, gboolean b_window_gone) {
   _request_finish(p_req, FALSE);
   g_free(p_req);
   if (b_window_gone || p_gate->b_disposed) {
      g_message("ggaze: queued request dropped — the window is closing");
      return;
   }
   p_gate->p_ops->show_status(p_gate->p_host, "Queued request dropped");
}

/* Hand the parked request (if any) back to the gate now that the prompt is
 * gone. b_proceed carries the answer: Save or Discard RESOLVED the preview, so
 * the parked request is simply retried through save_gate_maybe_save_then and
 * -- the mask now being clear -- runs straight away. Cancel (and a failed
 * Save) means "stay on this image, keep the preview"; running the parked
 * request anyway would do precisely what the user just refused, so it is
 * dropped instead, with a status line so the intent does not vanish
 * unsignalled.
 *
 * The one answer that proceeds and STILL must not retry is a prompt whose
 * continuation was the quit (b_prompt_quits): by the time the flush runs, that
 * continuation has already called gtk_window_close(), so retrying would
 * rebuild a whole Navigator + GFileMonitor + texture load inside a window
 * that is going away -- observed landing either there or as a silent drop
 * depending on how far dispose had got (round 4, finding r). A closing window
 * can honour no request, so the queue is dropped, deliberately and audibly.
 *
 * Retrying through the gate rather than calling the continuation directly is
 * what keeps this loop-free: the retry either runs immediately (mask clear) or
 * opens ONE fresh prompt that now owns the request as its own continuation, so
 * nothing can be parked twice. */
static void
_save_prompt_flush(SaveGate *p_gate, gboolean b_proceed) {
   _Request *p_req = p_gate->p_pending;
   /* Clear both first: the retry below may park a new request behind a fresh
    * prompt, which then owns the flag as well as the slot. */
   p_gate->p_pending      = NULL;
   gboolean b_was_quit    = p_gate->b_prompt_quits;
   p_gate->b_prompt_quits = FALSE;
   if (p_req == NULL) {
      return;
   }
   if (b_proceed && !b_was_quit) {
      save_gate_maybe_save_then(p_gate, p_req->fn, p_req->data, p_req->fn_free);
      g_free(p_req);
      return;
   }
   /* b_proceed here means the window is closing, so there is no overlay left
    * to paint on (and its widgets may already be gone); a Cancel leaves the
    * window very much alive, so that one is reported on screen as before. */
   _drop_pending(p_gate, p_req, b_proceed);
}

/* Single exit point for the dialog callback: optionally run the continuation
 * (b_proceed), release the continuation's data, then let whatever was parked
 * behind this prompt have its turn. Ordering matters -- fn still needs data
 * and the host, and the flush needs the host too, so the ctx's host ref is
 * dropped last. */
static void
_save_ctx_finish(_SaveCtx *p_ctx, gboolean b_proceed) {
   SaveGate *p_gate = p_ctx->p_gate; /* borrowed until the host unref below */
   gpointer  p_host = p_ctx->p_host; /* borrowed until the unref below */
   _request_finish(&p_ctx->t_req, b_proceed);
   g_object_unref(p_ctx->p_cancel);
   g_clear_object(&p_ctx->p_dlg_window);
   g_free(p_ctx);
   _save_prompt_flush(p_gate, b_proceed);
   g_object_unref(p_host);
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
 * tu0's round-3 probe hit and 2w0 re-measured (see save_gate_dispose for the
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
 * DISMISSED is the arm an Escape or a window manager closing the dialog takes,
 * and it only is because _save_prompt_show configures NO cancel button: with
 * one, GTK's response_cb returns that button's index (cancel_return) for every
 * negative response instead of raising GTK_DIALOG_ERROR_DISMISSED (gtk 4.22.4
 * gtkalertdialog.c:620), which is exactly the silence task 8w0 was sent to
 * explain. See _save_prompt_show for that decision and its price. The delete
 * confirm still sets one, for the opposite reason (aw0). */
static void
_report_unanswered_prompt(_PromptOutcome e_outcome) {
   if (e_outcome == _PROMPT_CANCELLED) {
      g_message("ggaze: Save/Discard/Cancel prompt cancelled — the window is "
                "going away");
      return;
   }
   g_message("ggaze: Save/Discard/Cancel prompt dismissed — treated as Cancel");
}

/* Alert-dialog response: 0=Cancel, 1=Discard, 2=Save.
 *
 * "0" really does mean the Cancel button was pressed, because
 * _save_prompt_show configures no cancel button: a prompt that goes away
 * without a press comes back as -1 plus GTK_DIALOG_ERROR_DISMISSED and is
 * reported by _report_unanswered_prompt above. What is still NOT
 * distinguishable is Escape from a window manager closing the dialog -- both
 * are one signal by the time GTK sees them (gtkmain.c turns GDK_DELETE into
 * gtk_window_emit_close_request, gtk_window_close() does the same for Escape,
 * and gtk/deprecated/gtkdialog.c's close_request answers
 * GTK_RESPONSE_DELETE_EVENT for either) -- but that split is not the useful
 * one. The useful one, "a button was pressed" vs "nobody pressed anything",
 * is what this callback now has, and it is what task 8w0 lacked when a prompt
 * vanished on a managed display with nothing in the log; see "A LIVE
 * COMPOSITOR IS NOT A NEUTRAL DISPLAY EITHER" in tests/meson.build.
 *
 * All three end in the same place either way: keep the preview, do not
 * proceed. */
static void
_save_dialog_cb(GObject *p_dlg, GAsyncResult *p_res, gpointer p_data) {
   _SaveCtx *p_ctx  = (_SaveCtx *)p_data;
   SaveGate *p_gate = p_ctx->p_gate;
   GError   *p_err  = NULL;
   gint      i_btn =
      gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(p_dlg), p_res, &p_err);
   g_object_unref(GTK_ALERT_DIALOG(p_dlg));
   _PromptOutcome e_outcome = _save_prompt_outcome(p_ctx, p_err);
   g_clear_error(&p_err);
   /* This prompt is gone, and so is its cancellable's job: a later trigger may
    * open a fresh one, which brings its own (see save_gate_maybe_save_then).
    * Clearing the gate's slot here is safe even after save_gate_dispose
    * already cleared it -- the ctx keeps the object alive until
    * _save_ctx_finish. */
   p_gate->b_save_prompt = FALSE;
   g_clear_object(&p_gate->p_prompt_cancel);
   if (e_outcome != _PROMPT_ANSWERED) { /* cancelled/dismissed -> as Cancel */
      _report_unanswered_prompt(e_outcome);
      _save_ctx_finish(p_ctx, FALSE);
      return;
   }
   if (i_btn == 2 && !p_gate->p_ops->do_save(p_gate->p_host)) { /* Save, but
                                                                 * it failed */
      _save_ctx_finish(p_ctx, FALSE);
      return;
   }
   if (i_btn == 1 || i_btn == 2) { /* Discard, or a Save that may proceed */
      p_gate->p_ops->discard(p_gate->p_host);
      _save_ctx_finish(p_ctx, TRUE);
      return;
   }
   _save_ctx_finish(p_ctx, FALSE); /* Cancel: keep the preview, do not
                                    * proceed -- but still free the ctx */
}

/* Build and show the modal Save/Discard/Cancel prompt, handing the request
 * over to _save_dialog_cb. Split out of save_gate_maybe_save_then to keep both
 * under the ~30-line convention.
 *
 * b_prompt_quits records whether THIS prompt's own continuation closes the
 * window, which is what _save_prompt_flush needs to know: a queued request
 * must not be retried into a window the answer is about to close (round 4,
 * finding r). Both quit entry points -- win.quit and the native
 * "close-request" -- share the host's quit continuation, so comparing against
 * it catches both.
 *
 * The GCancellable is not optional bookkeeping: without one, nothing can ever
 * finish this dialog's GTask except a button press, so a dispose under a live
 * prompt abandoned the _SaveCtx and every ctx the request carries -- 479 bytes
 * in 11 allocations, measured under ASan (task 2w0). Both this ctx and the
 * gate hold a ref on it; see save_gate_dispose for the cancelling end.
 *
 * What that does NOT cover is a plain gtk_window_destroy() with the prompt
 * still up, because it never reaches dispose: gtk_window_destroy() drops ONE
 * reference, the toplevel list's, and the _SaveCtx's own host ref keeps the
 * count off zero (measured on gtk 4.22.4: one ref dropped both for a window
 * built with a GtkApplication, as production does, and for one built without,
 * as the test harness does -- the application's window list holds no counted
 * reference of its own; the count then holds steady across 5 s of draining).
 * GTK wires destroy-with-parent to the parent's ::destroy, which GtkWidget
 * emits from dispose -- so the dialog stays up and clickable, and answering it
 * then still resolves everything normally (measured: the answer arrives as the
 * pressed button, refcount back to the caller's own).
 *
 * Nothing in src/ calls gtk_window_destroy(), and the host's close-request
 * handler refuses a close while the prompt is outstanding, so no ggaze code
 * path reaches that state. What is left uncovered is a process that EXITS
 * under the dialog (SIGTERM, a session logout, ^C): dispose never runs at all
 * there, so the cancel below never fires and the contexts go down with the
 * process. */
static void
_save_prompt_show(SaveGate *p_gate, const _Request *p_req) {
   GtkAlertDialog *p_dlg =
      gtk_alert_dialog_new("Save the enhanced copy before leaving this image?");
   static const char *const c_btns[] = {"Cancel", "Discard", "Save", NULL};
   gtk_alert_dialog_set_buttons(p_dlg, c_btns);
   gtk_alert_dialog_set_default_button(p_dlg, 2);
   /* No gtk_alert_dialog_set_cancel_button() on purpose (8w0), unlike
    * delete-confirm's ask. With one set, GTK's response_cb answers every
    * negative response -- Escape and a WM close both arrive as
    * GTK_RESPONSE_DELETE_EVENT -- with that button's index (gtk 4.22.4
    * gtkalertdialog.c:620), so "the prompt vanished without an answer" was
    * indistinguishable from "the user pressed Cancel" and nothing was logged.
    * Without one, that close comes back as -1 plus GTK_DIALOG_ERROR_DISMISSED,
    * which _report_unanswered_prompt says out loud. The OUTCOME is unchanged:
    * index 0 and _PROMPT_DISMISSED both end in _save_ctx_finish(p_ctx, FALSE).
    * The price, stated because it is a real one: an ordinary user pressing
    * Escape now logs a line too. It buys the only split that was ever
    * available -- a button was pressed vs nobody pressed anything -- and
    * nothing destructive rides on it: unlike the delete confirm, where -1 read
    * as a gboolean meant "yes, delete" (aw0), -1 here matches neither the
    * Discard nor the Save arm and falls through to the same FALSE. */
   gtk_alert_dialog_set_modal(p_dlg, TRUE);
   _SaveCtx *p_ctx = g_new(_SaveCtx, 1);
   p_ctx->p_gate   = p_gate;
   p_ctx->p_host   = g_object_ref(p_gate->p_host);
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
   /* Already NULL -- save_gate_maybe_save_then's b_save_prompt guard is what
    * gets us here, and _save_dialog_cb clears the slot with that flag. Cleared
    * anyway so the slot can never silently accumulate a second ref. */
   g_clear_object(&p_gate->p_prompt_cancel);
   p_gate->p_prompt_cancel = (GCancellable *)g_object_ref(p_ctx->p_cancel);
   p_gate->b_save_prompt   = TRUE;
   p_gate->b_prompt_quits  = (p_gate->p_ops->quit_continuation != NULL &&
                              p_req->fn == p_gate->p_ops->quit_continuation);
   gtk_alert_dialog_choose(p_dlg, GTK_WINDOW(p_gate->p_host), p_ctx->p_cancel,
                           _save_dialog_cb, p_ctx);
   p_ctx->p_dlg_window = p_gate->p_ops->alert_dialog_window(p_gate->p_host);
}

/* Park p_req until the outstanding prompt is answered. Exactly one slot, and
 * the newest request wins: a user who hits Alt+F4 five times is asking for one
 * quit, not five, and the alternative (an unbounded queue) would replay a
 * backlog of actions the user can no longer see the reason for. The request it
 * replaces is released, never run.
 *
 * What keeps the user's second request from LOOKING ignored is not the status
 * line below -- that one is best-effort at best, since the modal dialog is
 * covering the very overlay it paints on and the host's show_status auto-hides
 * it after a couple of seconds anyway (round 4, finding s). It is what happens
 * AFTER the answer: the request either runs (with whatever visible effect it
 * has of its own) or is dropped with an explicit "Queued request dropped". The
 * line here is kept because a dialog the user has dragged aside does reveal
 * it, and it costs nothing -- but the guarantee lives on the far side of the
 * prompt.
 *
 * p_req->data needs no ref of its own: the host is kept alive by the
 * outstanding prompt's _SaveCtx, and _save_ctx_finish flushes this slot before
 * releasing that ref. */
static void
_save_prompt_queue(SaveGate *p_gate, const _Request *p_req) {
   if (p_gate->p_pending != NULL) { /* displace: newest wins */
      _request_finish(p_gate->p_pending, FALSE);
      g_free(p_gate->p_pending);
   }
   p_gate->p_pending  = g_new(_Request, 1);
   *p_gate->p_pending = *p_req;
   p_gate->p_ops->show_status(p_gate->p_host,
                              "Answer the Save/Discard/Cancel prompt first");
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
void
save_gate_maybe_save_then(SaveGate *p_gate, GSourceFunc fn, gpointer data,
                          GDestroyNotify fn_free_data) {
   g_return_if_fail(p_gate != NULL);
   _Request t_req = {fn, data, fn_free_data};
   if (p_gate->b_save_prompt) {
      _save_prompt_queue(p_gate, &t_req);
      return;
   }
   if (!p_gate->p_ops->is_dirty(p_gate->p_host)) {
      _request_finish(&t_req, TRUE);
      return;
   }
   _save_prompt_show(p_gate, &t_req);
}

SaveGate *
save_gate_new(const SaveGateHostOps *p_ops, gpointer p_host) {
   g_return_val_if_fail(p_ops != NULL, NULL);
   SaveGate *p_gate = g_new0(SaveGate, 1);
   p_gate->p_ops    = p_ops;
   p_gate->p_host   = p_host;
   return (p_gate);
}

void
save_gate_delete(SaveGate *p_gate) {
   if (p_gate == NULL) {
      return;
   }
   /* A pending request parked behind a never-answered prompt: dispose already
    * dropped it (and cleared p_prompt_cancel), so this is defence-in-depth for
    * a gate freed without a dispose. */
   g_clear_pointer(&p_gate->p_pending, g_free);
   g_clear_object(&p_gate->p_prompt_cancel);
   g_free(p_gate);
}

gboolean
save_gate_outstanding(const SaveGate *p_gate) {
   g_return_val_if_fail(p_gate != NULL, FALSE);
   return (p_gate->b_save_prompt);
}

/* Cancel + drop any outstanding prompt (window dispose / close). Mirrors
 * delete-confirm's dispose: nothing but the dialog itself can finish its GTask
 * except a button press, so without this cancel a dispose under a live prompt
 * abandoned the _SaveCtx and every ctx the request carries (task 2w0).
 *
 * The cancel resolves the dialog as _PROMPT_CANCELLED, which _save_dialog_cb
 * treats as Cancel (do not proceed) -- the only honest answer here, and safe
 * twice over: the host's continuation is never run, and the host is already
 * tearing down. The b_disposed flag also steers _drop_pending's "queued
 * request dropped" message to the log rather than a gone overlay. */
void
save_gate_dispose(SaveGate *p_gate) {
   g_return_if_fail(p_gate != NULL);
   p_gate->b_disposed = TRUE;
   if (p_gate->p_pending != NULL) {
      _Request *p_req   = p_gate->p_pending;
      p_gate->p_pending = NULL;
      _drop_pending(p_gate, p_req, TRUE);
   }
   p_gate->b_prompt_quits = FALSE;
   g_cancellable_cancel(p_gate->p_prompt_cancel);
   g_clear_object(&p_gate->p_prompt_cancel);
}